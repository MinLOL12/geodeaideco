#pragma once
#include "json.hpp"
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <regex>
#include <locale>

namespace deco {
using Json = nlohmann::json;
struct Bounds { double x, y, w, h; };
struct Tile { double x, y, sx, sy, rot; int channel; };

inline bool validEndpoint(std::string const& url) {
    static const std::regex pat(R"(^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?(/[A-Za-z0-9_./~%-]*)?/chat/completions$)");
    return std::regex_match(url, pat);
}

inline double number(Json const& obj, char const* key, double lo, double hi) {
    if (!obj.contains(key) || !obj.at(key).is_number()) throw std::runtime_error("Missing numeric field.");
    double v = obj.at(key).get<double>();
    if (!std::isfinite(v) || v < lo || v > hi) throw std::runtime_error("A decoration value is outside its allowed range.");
    return v;
}

inline std::vector<Tile> parsePlan(std::string const& text, Bounds b) {
    if (text.size() > 200000) throw std::runtime_error("The decoration response is too large.");
    auto root = Json::parse(text);
    if (!root.is_object() || !root.contains("objects") || !root["objects"].is_array()) throw std::runtime_error("Expected JSON with an objects array.");
    auto const& arr = root["objects"];
    if (arr.empty() || arr.size() > 300) throw std::runtime_error("The plan must contain 1 to 300 objects.");
    std::vector<Tile> tiles;
    for (auto const& obj : arr) {
        if (!obj.is_object()) throw std::runtime_error("Invalid decoration object.");
        for (auto const& item : obj.items()) {
            if (item.key() != "x" && item.key() != "y" && item.key() != "sx" && item.key() != "sy" && item.key() != "rotation" && item.key() != "channel")
                throw std::runtime_error("Unexpected decoration field.");
        }
        Tile t{number(obj,"x",-120,b.w+120), number(obj,"y",-120,b.h+120), number(obj,"sx",.05,8), number(obj,"sy",.05,8), number(obj,"rotation",-360,360),0};
        if (!obj.contains("channel") || !obj["channel"].is_number_integer()) throw std::runtime_error("Invalid color channel.");
        auto ch = obj["channel"].get<long long>();
        if (ch < 1 || ch > 3) throw std::runtime_error("Only color channels 1, 2 and 3 are allowed.");
        t.channel = static_cast<int>(ch);
        double r = t.rot * 3.141592653589793 / 180.;
        double ex = 15 * (std::abs(std::cos(r))*t.sx + std::abs(std::sin(r))*t.sy);
        double ey = 15 * (std::abs(std::sin(r))*t.sx + std::abs(std::cos(r))*t.sy);
        if (t.x-ex < -120 || t.x+ex > b.w+120 || t.y-ey < -120 || t.y+ey > b.h+120)
            throw std::runtime_error("A decoration extends beyond the selection margin.");
        tiles.push_back(t);
    }
    return tiles;
}

inline std::string serialize(std::vector<Tile> const& tiles, Bounds b, int layer) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(8);
    for (auto const& t : tiles)
        out << "1,211,2," << b.x+t.x << ",3," << b.y+t.y << ",6," << t.rot
            << ",21," << t.channel << ",20," << layer << ",24,-1,25,-5,128," << t.sx << ",129," << t.sy << ';';
    return out.str();
}
}
