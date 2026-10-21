#pragma once
// Provider-specific, platform-independent request and response handling.
// Native Claude and Gemini APIs are not OpenAI-compatible chat endpoints.
#include "Plan.hpp"
#include <algorithm>
#include <regex>
#include <stdexcept>
#include <string>

namespace deco {

enum class Provider { OpenAICompatible, Claude, Gemini };
inline constexpr auto kDefaultEndpoint = "https://api.openai.com/v1/chat/completions";
inline constexpr auto kClaudeEndpoint = "https://api.anthropic.com/v1/messages";
inline constexpr auto kGeminiEndpoint = "https://generativelanguage.googleapis.com/v1beta/models";

inline Provider providerForEndpoint(std::string const& endpoint) {
    if (endpoint == kClaudeEndpoint) return Provider::Claude;
    if (endpoint == kGeminiEndpoint) return Provider::Gemini;
    return Provider::OpenAICompatible;
}

inline bool validProviderEndpoint(std::string const& endpoint) {
    return endpoint == kClaudeEndpoint || endpoint == kGeminiEndpoint || validEndpoint(endpoint);
}

// The model is inserted into a URL path for Gemini, never into a query string.
inline bool validModel(Provider provider, std::string const& model) {
    if (model.empty()) return false;
    if (provider != Provider::Gemini) return true;
    static const std::regex safe(R"(^[A-Za-z0-9_.-]+$)");
    return std::regex_match(model, safe);
}

inline std::string requestUrl(Provider provider, std::string const& endpoint, std::string const& model) {
    if (!validProviderEndpoint(endpoint) || provider != providerForEndpoint(endpoint) || !validModel(provider, model))
        throw std::invalid_argument("Invalid provider endpoint or model ID.");
    return provider == Provider::Gemini ? endpoint + "/" + model + ":generateContent" : endpoint;
}

inline Json requestBody(Provider provider, std::string const& model, std::string const& system,
                        std::string const& context, double temperature, bool noTemperature,
                        std::string const& previous = "", std::string const& feedback = "") {
    if (!validModel(provider, model)) throw std::invalid_argument("Invalid model ID.");
    Json messages = Json::array({{{"role", "user"}, {"content", context}}});
    if (!feedback.empty()) {
        messages.push_back({{"role", "assistant"}, {"content", previous}});
        messages.push_back({{"role", "user"}, {"content", "That output was invalid: " + feedback +
            " Return ONLY the corrected JSON object, following every rule and using only the listed object names."}});
    }
    if (provider == Provider::OpenAICompatible) {
        messages.insert(messages.begin(), Json{{"role", "system"}, {"content", system}});
        Json body = {{"model", model}, {"stream", false}, {"messages", messages}};
        if (!noTemperature) body["temperature"] = temperature;
        return body;
    }
    if (provider == Provider::Claude) {
        Json body = {{"model", model}, {"max_tokens", 4096}, {"system", system}, {"messages", messages}};
        if (!noTemperature) body["temperature"] = std::min(temperature, 1.0); // Anthropic accepts 0..1
        return body;
    }
    // Gemini uses contents/parts and systemInstruction instead of messages.
    Json contents = Json::array();
    for (auto const& message : messages) {
        contents.push_back({{"role", message.at("role") == "assistant" ? "model" : "user"},
                            {"parts", Json::array({{{"text", message.at("content")}}})}});
    }
    Json config = {{"maxOutputTokens", 8192}, {"responseMimeType", "application/json"}};
    if (!noTemperature) config["temperature"] = temperature;
    return {{"systemInstruction", {{"parts", Json::array({{{"text", system}}})}}},
            {"contents", contents}, {"generationConfig", config}};
}

inline std::string providerText(Provider provider, std::string const& responseText) {
    if (provider == Provider::OpenAICompatible) return extractAssistantText(responseText);
    auto root = Json::parse(responseText);
    if (provider == Provider::Claude) {
        std::string text;
        for (auto const& block : root.at("content"))
            if (block.is_object() && block.value("type", "") == "text" && block.contains("text") && block["text"].is_string())
                text += block["text"].get<std::string>();
        if (!text.empty()) return text;
    } else {
        auto const& candidates = root.at("candidates");
        if (!candidates.is_array() || candidates.empty()) throw std::runtime_error("Gemini returned no candidates (check safety filters and prompt).");
        std::string text;
        for (auto const& part : candidates.at(0).at("content").at("parts"))
            if (part.is_object() && part.contains("text") && part["text"].is_string())
                text += part["text"].get<std::string>();
        if (!text.empty()) return text;
    }
    throw std::runtime_error("The API response had no text content.");
}

} // namespace deco
