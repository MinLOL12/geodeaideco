#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/LoadingSpinner.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include "Plan.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <random>

using namespace geode::prelude;
using deco::Json;

// AI Decorator v2.
//
// What changed vs v1 (and why v1 felt "fake"):
// * v1 only had ONE object it could place (id 211, a square), scaled and
//   rotated. v2 gives the model a named vocabulary of real decoration
//   pieces (glow, outlines, shine, gears, chains, grass, pipes...), each
//   probed at runtime so only objects the game itself reports as
//   "Decoration" type can ever be placed.
// * v1 sent bare object IDs + centers and assumed every object was 30x30,
//   so the model could not know what it was decorating. v2 reads each
//   selected object's real type (solid/slope/hazard/pad/orb/portal/...)
//   and real footprint from the editor, and sends an ASCII grid map too.
//   Placements can be ANCHORED to a specific selected object, so deco
//   attaches to structures instead of floating randomly in the bounds.
// * v1 sent no temperature and a byte-identical request every time, so
//   providers returned the same plan again and again. v2 sends a
//   temperature (Creativity setting), a fresh variation nonce, and on
//   Regenerate tells the model what the last plan used and demands a
//   different composition; if the new plan still fingerprints as ~the
//   same, it automatically re-rolls once.
// * v1 rejected markdown fences / extra fields and hid the real error
//   behind "invalid plan". v2 parses tolerantly, auto-repairs once by
//   feeding the validation error back to the model, and shows the actual
//   problem in the UI.

namespace {
std::string sessionKey;

void notice(std::string const& msg) {
    FLAlertLayer::create("AI Decorator", msg, "OK")->show();
}

std::string truncate(std::string s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n - 1) + "~";
}

std::string endpointHost(std::string const& url) {
    auto a = url.find("://");
    if (a == std::string::npos) return url;
    auto b = url.find('/', a + 3);
    return url.substr(a + 3, (b == std::string::npos ? url.size() : b) - (a + 3));
}

int nextVariation() {
    static std::mt19937 rng{std::random_device{}()};
    static std::uniform_int_distribution<int> dist(1000, 999999);
    return dist(rng);
}

// GameObjectType values (Geode bindings Enums.hpp). Compared as ints so a
// pinned-bindings enum rename can never break this file.
constexpr int kTypeSolid = 0, kTypeHazard = 2, kTypeDecoration = 7,
              kTypeModifier = 20, kTypeBreakable = 21, kTypeSecretCoin = 22,
              kTypeSlope = 25, kTypeCollectible = 30, kTypeUserCoin = 31,
              kTypeCollision = 39, kTypeSpecial = 40, kTypeAnimatedHazard = 47;

deco::ObjKind kindFromType(int t) {
    using K = deco::ObjKind;
    if (t == kTypeSolid || t == kTypeBreakable || t == kTypeCollision) return K::Solid;
    if (t == kTypeSlope) return K::Slope;
    if (t == kTypeHazard || t == kTypeAnimatedHazard) return K::Hazard;
    if (t == 8 || t == 9 || t == 10 || t == 34 || t == 44) return K::Pad;          // jump/gravity/spider pads
    if (t == 11 || t == 12 || t == 13 || t == 29 || t == 32 || t == 35 ||
        t == 36 || t == 37 || t == 38 || t == 43 || t == 46) return K::Orb;        // rings/orbs
    if ((t >= 3 && t <= 6) || (t >= 14 && t <= 19) || t == 23 || t == 24 ||
        t == 26 || t == 27 || t == 28 || t == 33 || t == 41 || t == 42) return K::Portal;
    if (t == kTypeDecoration) return K::Deco;
    if (t == kTypeModifier) return K::Trigger;
    if (t == kTypeSecretCoin || t == kTypeCollectible || t == kTypeUserCoin) return K::Collectible;
    return K::Other;
}

struct Selection {
    deco::Bounds bounds{};
    std::vector<deco::SrcObject> objects;
    int layer = 0;
    std::string summary;
};

Selection readSelection(EditorUI* ui) {
    auto arr = ui->getSelectedObjects();
    if (!arr || !arr->count()) throw std::runtime_error("Select some objects first, then open AI Decorator.");
    if (arr->count() > 400)
        throw std::runtime_error("Select at most 400 objects per request so the model gets a section it can actually reason about. Decorate big sections in a few passes.");

    struct Raw { deco::SrcObject o; double cx, cy; };
    std::vector<Raw> raw;
    double left = std::numeric_limits<double>::max(), bottom = left;
    double right = -left, top = right;
    int layer = 0;
    std::map<deco::ObjKind, int> counts;

    for (auto obj : CCArrayExt<GameObject*>(arr)) {
        auto p = obj->getPosition();
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::runtime_error("Invalid selection position.");
        // Real footprint from the object itself (v1 assumed 30x30 for everything).
        obj->dirtifyObjectRect();
        auto rect = obj->getObjectRect();
        double w = rect.size.width, h = rect.size.height;
        if (w < 1 || h < 1) {
            auto cs = obj->getContentSize();
            w = cs.width * std::abs(obj->getScaleX());
            h = cs.height * std::abs(obj->getScaleY());
        }
        if (w < 1) w = 30;
        if (h < 1) h = 30;
        w = std::min(w, 3000.0); h = std::min(h, 3000.0);
        double rot = obj->getRotation();
        double r = rot * 3.141592653589793 / 180.0;
        double ew = (std::abs(std::cos(r)) * w + std::abs(std::sin(r)) * h) / 2;
        double eh = (std::abs(std::sin(r)) * w + std::abs(std::cos(r)) * h) / 2;
        left = std::min(left, double(p.x) - ew);
        bottom = std::min(bottom, double(p.y) - eh);
        right = std::max(right, double(p.x) + ew);
        top = std::max(top, double(p.y) + eh);
        layer = std::max(layer, int(obj->m_editorLayer));
        auto kind = kindFromType(static_cast<int>(obj->m_objectType));
        ++counts[kind];
        raw.push_back({{kind, 0, 0, w, h, rot}, p.x, p.y});
    }

    Selection s;
    s.bounds = {left, bottom, right - left, top - bottom};
    if (s.bounds.w > 6000 || s.bounds.h > 6000)
        throw std::runtime_error("Select a section smaller than 6000 units on each axis.");
    for (auto& it : raw) {
        it.o.x = it.cx - left;
        it.o.y = it.cy - bottom;
        s.objects.push_back(it.o);
    }
    s.layer = std::min(999, layer + 1);

    std::string kinds;
    for (auto k : {deco::ObjKind::Solid, deco::ObjKind::Slope, deco::ObjKind::Hazard,
                   deco::ObjKind::Pad, deco::ObjKind::Orb, deco::ObjKind::Portal,
                   deco::ObjKind::Deco, deco::ObjKind::Trigger, deco::ObjKind::Collectible,
                   deco::ObjKind::Other}) {
        if (counts[k]) {
            if (!kinds.empty()) kinds += ", ";
            kinds += std::to_string(counts[k]) + " " + deco::kindName(k);
        }
    }
    s.summary = fmt::format("{} objects - {}x{} units - {}", s.objects.size(),
                            int(std::round(s.bounds.w)), int(std::round(s.bounds.h)), kinds);
    return s;
}

// ---------------------------------------------------------------------------
// Runtime vocabulary probe: ask the GAME which candidate IDs are true
// decoration objects (no hitbox, no gameplay effect). Only those are ever
// offered to the model. If the probe returns suspiciously little (should
// not happen), fall back to the full candidate list but re-check every
// piece again at Apply time using the per-ID results.
// ---------------------------------------------------------------------------
struct ProbeResult {
    std::vector<deco::VocabEntry> effective;
    std::map<int, bool> isDecoration;
    bool reliable = false;
};

ProbeResult const& probeVocabulary() {
    static std::optional<ProbeResult> cached;
    if (cached) return *cached;
    ProbeResult pr;
    for (auto entry : deco::vocabulary()) {
        auto* obj = GameObject::createWithKey(entry.id);
        bool ok = obj && static_cast<int>(obj->m_objectType) == kTypeDecoration;
        pr.isDecoration[entry.id] = ok;
        if (obj) {
            auto cs = obj->getContentSize();
            if (cs.width >= 1 && cs.height >= 1) { entry.w = cs.width; entry.h = cs.height; }
        }
        if (ok) pr.effective.push_back(entry);
        else log::warn("[AI Decorator] vocabulary piece '{}' (id {}) is not a decoration object in this game version; excluding it", entry.name, entry.id);
    }
    pr.reliable = pr.effective.size() >= 5;
    if (!pr.reliable) {
        log::warn("[AI Decorator] vocabulary probe found only {} decoration pieces; falling back to candidate list with apply-time checks", pr.effective.size());
        pr.effective = deco::vocabulary();
        for (auto& e : pr.effective) {
            auto* obj = GameObject::createWithKey(e.id);
            if (obj) {
                auto cs = obj->getContentSize();
                if (cs.width >= 1 && cs.height >= 1) { e.w = cs.width; e.h = cs.height; }
            }
        }
    }
    log::info("[AI Decorator] vocabulary ready: {} pieces", pr.effective.size());
    cached = std::move(pr);
    return *cached;
}

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------
CCMenuItemSpriteExtra* addButton(CCMenu* menu, CCObject* target, SEL_MenuHandler cb,
                                 char const* text, CCPoint pos, float scale = .65f, int fixedWidth = 0) {
    ButtonSprite* spr = fixedWidth > 0
        ? ButtonSprite::create(text, fixedWidth, 0, scale, true)
        : ButtonSprite::create(text);
    if (!fixedWidth) spr->setScale(scale);
    auto btn = CCMenuItemSpriteExtra::create(spr, target, cb);
    btn->setPosition(pos);
    menu->addChild(btn);
    return btn;
}

CCLabelBMFont* addLabel(CCNode* parent, std::string const& text, CCPoint pos, float scale,
                        char const* font = "chatFont.fnt", float anchorX = .5f) {
    auto label = CCLabelBMFont::create(text.c_str(), font);
    label->setScale(scale);
    label->setAnchorPoint({anchorX, .5f});
    label->setPosition(pos);
    parent->addChild(label);
    return label;
}

// ---------------------------------------------------------------------------
// Preview popup: draws the actual pieces (shapes per type, real source
// footprints by kind), with Apply / Regenerate / Back / Discard.
// ---------------------------------------------------------------------------
class Preview : public Popup {
    WeakRef<EditorUI> m_ui;
    // Callbacks into the owning Decorator (avoids a WeakRef<Decorator> member,
    // which would require Decorator to be a complete type here).
    std::function<void()> m_onRegenerate;
    std::function<void()> m_onCloseAll;
    std::function<bool()> m_isBusy;
    Selection m_src;
    deco::PlanResult m_plan;
    std::string m_stats;
    CCDrawNode* m_draw = nullptr;
    CCNode* m_root = nullptr;
    CCLayerColor* m_bg = nullptr;
    CCLabelBMFont* m_statsLabel = nullptr;
    CCLabelBMFont* m_warnLabel = nullptr;
    CCLabelBMFont* m_busyLabel = nullptr;
    LoadingSpinner* m_spinner = nullptr;
    CCMenuItemSpriteExtra* m_apply = nullptr;
    CCMenuItemSpriteExtra* m_regen = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_buttons;
    bool m_closing = false;

    // --- shape painting (draw-node local coords = level units + 120 margin) ---
    void poly(std::vector<CCPoint> pts, ccColor4F fill, float borderW = 0, ccColor4F border = {0, 0, 0, 0});
    void drawSources();
    void drawPlacement(deco::Placement const& p);
    void redraw();

    void onApply(CCObject*);
    void onRegenerate(CCObject*);
    void onBack(CCObject*);
    void onDiscard(CCObject*);

public:
    bool init(EditorUI* ui, Selection src);
    void setPlan(deco::PlanResult plan, std::string stats);
    void setBusy(bool busy, std::string const& status);
    void closeSelf() { onClose(nullptr); }
    void setCallbacks(std::function<void()> onRegenerate, std::function<void()> onCloseAll,
                      std::function<bool()> isBusy) {
        m_onRegenerate = std::move(onRegenerate);
        m_onCloseAll = std::move(onCloseAll);
        m_isBusy = std::move(isBusy);
    }
    static Preview* create(EditorUI* ui, Selection src);
};

// ---------------------------------------------------------------------------
// API setup popup (key / endpoint / model + provider presets)
// ---------------------------------------------------------------------------
class ApiSetup : public Popup {
    TextInput* m_key = nullptr;
    TextInput* m_endpoint = nullptr;
    TextInput* m_model = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    std::function<void()> m_onDone;

    TextInput* input(char const* title, char const* hint, float y, size_t limit) {
        auto field = TextInput::create(400, hint);
        field->setLabel(title);
        field->setCommonFilter(CommonFilter::Any);
        field->setMaxCharCount(limit);
        field->setPosition({230, y});
        m_mainLayer->addChild(field);
        return field;
    }

    void preset(char const* url, char const* hint) {
        m_endpoint->setString(url);
        m_hint->setString(hint);
    }
    void onOpenAI(CCObject*) { preset("https://api.openai.com/v1/chat/completions", "Model examples: gpt-4o, gpt-4o-mini  (API billing is separate from ChatGPT)"); }
    void onOpenRouter(CCObject*) { preset("https://openrouter.ai/api/v1/chat/completions", "Model examples: openai/gpt-4o-mini, anthropic/claude-sonnet-4"); }
    void onGroq(CCObject*) { preset("https://api.groq.com/openai/v1/chat/completions", "Model examples: llama-3.3-70b-versatile, openai/gpt-oss-120b"); }
    void onLocal(CCObject*) { preset("http://127.0.0.1:11434/v1/chat/completions", "Ollama on this PC (free, no key needed - type anything). Model: qwen2.5-coder, llama3.1"); }

    void onSave(CCObject*) {
        auto endpoint = m_endpoint->getString();
        auto model = m_model->getString();
        auto key = m_key->getString();
        if (!deco::validEndpoint(endpoint)) { notice("Use a full endpoint ending in /chat/completions (HTTPS, or http://127.0.0.1 for a local model)."); return; }
        if (model.empty()) { notice("Enter a model ID your account / local server provides."); return; }
        if (key.find_first_of("\r\n") != std::string::npos) { notice("The API key contains invalid characters."); return; }
        if (!key.empty()) sessionKey = key;
        Mod::get()->setSavedValue("endpoint", endpoint);
        Mod::get()->setSavedValue("model", model);
        onClose(nullptr);
    }

    void onClearKey(CCObject*) {
        sessionKey.clear();
        m_key->setString("");
        Notification::create("Session API key cleared", NotificationIcon::Info)->show();
    }

    void onClose(CCObject* sender) override {
        if (m_key) m_key->setString("");
        Popup::onClose(sender);
        if (m_onDone) m_onDone();
    }

    bool init(std::function<void()> onDone) {
        if (!Popup::init(460, 305)) return false;
        m_onDone = std::move(onDone);
        setTitle("API Setup");
        m_key = input("API key (kept in memory for this game session only)", "Paste API key", 250, 512);
        m_key->setPasswordMode(true);
        m_key->setString(sessionKey);
        m_endpoint = input("Chat Completions endpoint", "https://provider/v1/chat/completions", 204, 500);
        m_endpoint->setString(Mod::get()->getSavedValue<std::string>("endpoint", "https://api.openai.com/v1/chat/completions"));
        m_model = input("Model ID", "Your provider's model ID", 158, 160);
        m_model->setString(Mod::get()->getSavedValue<std::string>("model", ""));

        addLabel(m_mainLayer, "Presets:", {32, 126}, .5f, "bigFont.fnt", 0.f);
        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onOpenAI), "OpenAI", {118, 122}, .45f);
        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onOpenRouter), "OpenRouter", {205, 122}, .45f);
        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onGroq), "Groq", {285, 122}, .45f);
        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onLocal), "Ollama (local)", {378, 122}, .45f);
        m_hint = addLabel(m_mainLayer, "Pick a preset or paste any OpenAI-compatible endpoint.", {230, 96}, .42f);
        addLabel(m_mainLayer, "Sends only: your prompt + selected objects' types/positions/sizes.", {230, 76}, .42f);
        addLabel(m_mainLayer, "No key is written to disk. Requests go only to the endpoint above.", {230, 60}, .42f);

        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onSave), "Save", {320, 30}, .7f);
        addButton(m_buttonMenu, this, menu_selector(ApiSetup::onClearKey), "Clear key", {140, 30}, .55f);
        return true;
    }
public:
    static ApiSetup* create(std::function<void()> onDone) {
        auto p = new ApiSetup;
        if (p->init(std::move(onDone))) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Main decorator popup
// ---------------------------------------------------------------------------
struct RequestCtx {
    deco::GenOptions opt;
    int phase = 0; // 0 = normal, 1 = repair after invalid output, 2 = forced re-roll (too similar)
    bool noTemperature = false;
    std::string feedback;
    std::string prevText;
};

class Decorator : public Popup {
    WeakRef<EditorUI> m_ui;
    Selection m_src;
    std::vector<deco::VocabEntry> m_vocab;

    TextInput* m_prompt = nullptr;
    CCLabelBMFont* m_connLabel = nullptr;
    CCLabelBMFont* m_status = nullptr;
    LoadingSpinner* m_spinner = nullptr;
    CCMenuItemSpriteExtra* m_generate = nullptr;
    ButtonSprite* m_creativitySpr = nullptr;
    ButtonSprite* m_densitySpr = nullptr;
    ButtonSprite* m_layerSpr = nullptr;

    async::TaskHolder<web::WebResponse> m_request;
    bool m_busy = false;
    WeakRef<Preview> m_preview;
    std::vector<deco::Placement> m_lastPlan;

    // UI state (persisted): 0..3 creativity, 0..2 density, 0..2 layer
    int m_creativity = 1, m_density = 1, m_layer = 0;

    static constexpr std::array<const char*, 4> kCreativity = {"Precise", "Balanced", "Creative", "Wild"};
    static constexpr std::array<double, 4> kTemps = {0.35, 0.85, 1.1, 1.35};
    static constexpr std::array<const char*, 3> kDensity = {"Sparse", "Balanced", "Dense"};
    static constexpr std::array<const char*, 3> kLayer = {"Behind", "Mixed", "In front"};

    void refreshConnLabel() {
        auto model = Mod::get()->getSavedValue<std::string>("model", "");
        auto endpoint = Mod::get()->getSavedValue<std::string>("endpoint", "https://api.openai.com/v1/chat/completions");
        std::string text;
        if (sessionKey.empty() && endpointHost(endpoint).find("127.0.0.1") == std::string::npos &&
            endpointHost(endpoint).find("localhost") == std::string::npos)
            text = "No API key set - tap API Setup";
        else if (model.empty())
            text = "No model set - tap API Setup";
        else
            text = fmt::format("{}  @  {}", model, endpointHost(endpoint));
        m_connLabel->setString(truncate(text, 52).c_str());
    }

    void setBusy(bool busy, std::string const& status) {
        m_busy = busy;
        m_generate->setEnabled(!busy);
        m_prompt->setEnabled(!busy);
        m_spinner->setVisible(busy);
        m_status->setString(status.c_str());
        if (auto pv = m_preview.lock()) pv->setBusy(busy, status);
    }

    void onClose(CCObject* sender) override {
        m_request.cancel();
        m_busy = false;
        Popup::onClose(sender);
    }

    // --- control handlers ---
    void onCycleCreativity(CCObject*) {
        m_creativity = (m_creativity + 1) % 4;
        m_creativitySpr->setString(kCreativity[m_creativity]);
    }
    void onCycleDensity(CCObject*) {
        m_density = (m_density + 1) % 3;
        m_densitySpr->setString(kDensity[m_density]);
    }
    void onCycleLayer(CCObject*) {
        m_layer = (m_layer + 1) % 3;
        m_layerSpr->setString(kLayer[m_layer]);
    }

    void applyPreset(std::string const& text) {
        m_prompt->setString(text);
        m_status->setString("Preset applied - edit it or hit Generate");
    }
    void onPresetIndustrial(CCObject*) { applyPreset("industrial factory: square pipes running behind the structures, gears in the gaps, rivet dots along edges, panel backplates with thin glow lines, chains holding floating parts"); }
    void onPresetNeon(CCObject*) { applyPreset("neon tech: large soft glow behind structures, neon panels framing the blocks, pulsing circles and split rings, sparse shine sparkles, thin glow lines tracing the layout"); }
    void onPresetNature(CCObject*) { applyPreset("overgrown nature: grass tufts on top of every solid surface, soft glow like sunlight behind, clouds in empty sky areas, brick panels as ruined walls, dots as fireflies"); }
    void onPresetCrystal(CCObject*) { applyPreset("crystal cave: quarter discs and rotated panels as crystal shards clustered at structure bases, pulsing discs, shine sparkles on tips, deep glow behind everything"); }
    void onPresetMinimal(CCObject*) { applyPreset("clean minimal: thin glow lines under platforms, single outline frames around key blocks, very sparse dots, one soft glow behind the main structure"); }
    void onPresetSpooky(CCObject*) { applyPreset("spooky ruins: brick background walls, chains hanging from floating blocks, dim glow behind structures, scattered dots like dust, broken outline frames"); }

    void onSetup(CCObject*) {
        if (m_busy) return;
        auto weak = WeakRef(this);
        if (auto p = ApiSetup::create([weak] { if (auto self = weak.lock()) self->refreshConnLabel(); }))
            p->show();
    }

    void onHelp(CCObject*) {
        notice(
            "1) Select a section of your level (max 400 objects).\n"
            "2) Set editor colors: channel 1 = primary, 2 = accent, 3 = highlight.\n"
            "3) API Setup: your key (session only), endpoint and model. Local Ollama works too.\n"
            "4) Describe the look, pick creativity/amount/layer, then Generate.\n"
            "5) In the preview: Apply, Regenerate for a different variation, or Back to edit.\n\n"
            "Only true decoration objects (no hitboxes/triggers) can be generated, "
            "verified against the game at runtime. Nothing is placed until you press Apply; editor Undo removes it all.");
    }

    // --- generation ---
    deco::GenOptions currentOptions(bool isRegen) {
        deco::GenOptions opt;
        opt.prompt = m_prompt->getString();
        opt.variation = nextVariation();
        opt.temperature = kTemps[m_creativity];
        deco::densityTargets(m_density, m_src.objects.size(), opt.targetMin, opt.targetMax);
        opt.layerHint = m_layer == 0 ? "back" : m_layer == 1 ? "mixed" : "front";
        if (isRegen && !m_lastPlan.empty())
            opt.previousSummary = deco::summarizePlan(m_lastPlan);
        return opt;
    }

    void onGenerate(CCObject*) {
        if (m_busy) return;
        if (m_prompt->getString().empty()) { notice("Describe the decoration first (or tap a style preset)."); return; }
        auto endpoint = Mod::get()->getSavedValue<std::string>("endpoint", "https://api.openai.com/v1/chat/completions");
        auto model = Mod::get()->getSavedValue<std::string>("model", "");
        bool local = endpointHost(endpoint).find("127.0.0.1") != std::string::npos ||
                     endpointHost(endpoint).find("localhost") != std::string::npos;
        if (model.empty() || (sessionKey.empty() && !local)) {
            notice("Set your API key, endpoint and model first.");
            onSetup(nullptr);
            return;
        }
        if (!deco::validEndpoint(endpoint)) { notice("The saved endpoint is not a valid /chat/completions URL. Open API Setup to fix it."); return; }
        Mod::get()->setSavedValue("prompt", std::string(m_prompt->getString()));
        Mod::get()->setSavedValue("creativity", m_creativity);
        Mod::get()->setSavedValue("density", m_density);
        Mod::get()->setSavedValue("layer", m_layer);
        m_lastPlan.clear();
        sendRequest(RequestCtx{currentOptions(false), 0, false, "", ""});
    }

    void sendRequest(RequestCtx ctx) {
        auto endpoint = Mod::get()->getSavedValue<std::string>("endpoint", "https://api.openai.com/v1/chat/completions");
        auto model = Mod::get()->getSavedValue<std::string>("model", "");
        auto system = deco::buildSystemPrompt(m_vocab, ctx.opt);
        auto context = deco::buildContext(m_src.objects, m_src.bounds, ctx.opt);

        Json messages = Json::array({
            {{"role", "system"}, {"content", system}},
            {{"role", "user"}, {"content", context.dump()}},
        });
        if (ctx.phase == 1) {
            messages.push_back({{"role", "assistant"}, {"content", ctx.prevText}});
            messages.push_back({{"role", "user"},
                {"content", "That output was invalid: " + ctx.feedback +
                            " Return ONLY the corrected JSON object, following every rule and using only the listed object names."}});
        }
        Json payload = {{"model", model}, {"stream", false}, {"messages", messages}};
        if (!ctx.noTemperature) payload["temperature"] = ctx.opt.temperature;

        auto body = matjson::parse(payload.dump());
        if (!body) { notice("Could not encode the request."); return; }

        std::string status = ctx.phase == 1 ? "Model's answer was invalid - asking it to fix it..."
                             : ctx.phase == 2 ? "That looked the same as before - re-rolling..."
                             : fmt::format("Asking {} (variation #{})...", truncate(model, 24), ctx.opt.variation);
        setBusy(true, status);

        auto weak = WeakRef(this);
        auto key = sessionKey.empty() ? std::string("local") : sessionKey;
        m_request.spawn(web::WebRequest().header("Authorization", "Bearer " + key)
                .header("Content-Type", "application/json").timeout(std::chrono::seconds(120))
                .followRedirects(false).bodyJSON(body.unwrap()).post(endpoint),
            [weak, ctx](web::WebResponse resp) mutable {
                auto self = weak.lock();
                if (!self) return;
                if (!resp.ok()) {
                    auto text = resp.string();
                    std::string bodyStr = text ? text.unwrap() : std::string();
                    // Some models reject the temperature parameter: retry once without it.
                    if (resp.code() == 400 && !ctx.noTemperature &&
                        deco::lower(bodyStr).find("temperature") != std::string::npos) {
                        ctx.noTemperature = true;
                        self->sendRequest(ctx);
                        return;
                    }
                    self->setBusy(false, "Request failed - nothing was added");
                    for (char& c : bodyStr) if (c == '\n' || c == '\r') c = ' ';
                    notice(fmt::format("Request failed (HTTP {}).\n{}\n\nCheck your endpoint, key, model access and API credit in API Setup. No objects were added.",
                                       resp.code(), truncate(bodyStr, 260)));
                    return;
                }
                if (resp.data().size() > 1000000) {
                    self->setBusy(false, "Response too large");
                    notice("The API response is too large. Nothing was added.");
                    return;
                }
                try {
                    auto text = resp.string();
                    if (!text) throw std::runtime_error("Could not read the API response.");
                    auto content = deco::extractAssistantText(text.unwrap());
                    auto plan = deco::parsePlan(content, self->m_src.objects, self->m_src.bounds, self->m_vocab);

                    // The v1 complaint, handled: if a regenerate comes back as
                    // (nearly) the same arrangement, re-roll once automatically.
                    if (!self->m_lastPlan.empty() && ctx.phase != 2 &&
                        deco::similarity(self->m_lastPlan, plan.placements) > 0.85) {
                        auto next = RequestCtx{self->currentOptions(true), 2, ctx.noTemperature, "", ""};
                        next.opt.previousSummary = deco::summarizePlan(plan.placements);
                        self->sendRequest(std::move(next));
                        return;
                    }
                    self->deliverPlan(std::move(plan), ctx.opt.variation);
                } catch (std::exception const& e) {
                    // One automatic repair round-trip with the concrete error.
                    if (ctx.phase == 0) {
                        std::string bad;
                        try {
                            auto text = resp.string();
                            if (text) bad = truncate(deco::extractAssistantText(text.unwrap()), 6000);
                        } catch (...) {}
                        auto next = RequestCtx{ctx.opt, 1, ctx.noTemperature, e.what(), std::move(bad)};
                        self->sendRequest(std::move(next));
                        return;
                    }
                    self->setBusy(false, "Invalid answer - nothing was added");
                    notice(fmt::format("The model returned an invalid decoration plan:\n\n{}\n\nTry a simpler prompt, lower Creativity, or a model that follows JSON instructions well. No objects were added.", e.what()));
                }
            });
    }

    void deliverPlan(deco::PlanResult plan, int variation); // defined after Preview
    friend class Preview;

    bool init(EditorUI* ui, Selection src) {
        if (!Popup::init(500, 310)) return false;
        m_ui = ui;
        m_src = std::move(src);
        m_vocab = probeVocabulary().effective;
        m_creativity = std::clamp(Mod::get()->getSavedValue<int>("creativity", 1), 0, 3);
        m_density = std::clamp(Mod::get()->getSavedValue<int>("density", 1), 0, 2);
        m_layer = std::clamp(Mod::get()->getSavedValue<int>("layer", 0), 0, 2);

        setTitle("AI Decorator");
        addLabel(m_mainLayer, truncate(m_src.summary, 74), {250, 274}, .44f);

        m_prompt = TextInput::create(460, "e.g. dark industrial frames, thin glow accents, grass on top");
        m_prompt->setLabel("Describe the decoration");
        m_prompt->setCommonFilter(CommonFilter::Any);
        m_prompt->setMaxCharCount(600);
        m_prompt->setPosition({250, 240});
        m_prompt->setString(Mod::get()->getSavedValue<std::string>("prompt", ""));
        m_mainLayer->addChild(m_prompt);

        addLabel(m_mainLayer, "Style presets:", {22, 212}, .5f, "bigFont.fnt", 0.f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetIndustrial), "Industrial", {128, 210}, .42f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetNeon), "Neon", {208, 210}, .42f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetNature), "Nature", {273, 210}, .42f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetCrystal), "Crystal", {340, 210}, .42f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetMinimal), "Minimal", {408, 210}, .42f);
        addButton(m_buttonMenu, this, menu_selector(Decorator::onPresetSpooky), "Spooky", {464, 210}, .42f);

        addLabel(m_mainLayer, "Creativity", {86, 178}, .48f, "bigFont.fnt");
        addLabel(m_mainLayer, "Amount", {250, 178}, .48f, "bigFont.fnt");
        addLabel(m_mainLayer, "Layer", {414, 178}, .48f, "bigFont.fnt");

        auto mkCycle = [&](SEL_MenuHandler cb, const char* text, float x, ButtonSprite*& sprOut) {
            auto spr = ButtonSprite::create(text, 130, 0, .6f, true);
            sprOut = spr;
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({x, 152});
            m_buttonMenu->addChild(btn);
        };
        mkCycle(menu_selector(Decorator::onCycleCreativity), kCreativity[m_creativity], 86, m_creativitySpr);
        mkCycle(menu_selector(Decorator::onCycleDensity), kDensity[m_density], 250, m_densitySpr);
        mkCycle(menu_selector(Decorator::onCycleLayer), kLayer[m_layer], 414, m_layerSpr);

        addLabel(m_mainLayer, "Channels: 1 primary - 2 accent - 3 highlight (set colors in editor)", {250, 118}, .42f);

        m_connLabel = addLabel(m_mainLayer, "", {22, 92}, .46f, "chatFont.fnt", 0.f);
        refreshConnLabel();
        addButton(m_buttonMenu, this, menu_selector(Decorator::onSetup), "API Setup", {428, 92}, .52f);

        m_spinner = LoadingSpinner::create(22.f);
        m_spinner->setPosition({60, 64});
        m_spinner->setVisible(false);
        m_mainLayer->addChild(m_spinner);
        m_status = addLabel(m_mainLayer, "Sends your prompt + selection info to your chosen endpoint", {250, 64}, .44f);

        addButton(m_buttonMenu, this, menu_selector(Decorator::onHelp), "Help", {70, 30}, .55f);
        m_generate = addButton(m_buttonMenu, this, menu_selector(Decorator::onGenerate), "Generate", {392, 30}, .8f);
        return true;
    }
public:
    bool busy() const { return m_busy; }
    void regenerateFromPreview() {
        if (m_busy) return;
        sendRequest(RequestCtx{currentOptions(true), 0, false, "", ""});
    }
    void closeAll() {
        if (auto pv = m_preview.lock()) pv->closeSelf();
        onClose(nullptr);
    }
    static Decorator* create(EditorUI* ui, Selection src) {
        auto p = new Decorator;
        if (p->init(ui, std::move(src))) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Preview implementation
// ---------------------------------------------------------------------------
ccColor4F channelColor(int channel, float alpha) {
    switch (channel) {
        case 2: return {0.92f, 0.45f, 1.0f, alpha};
        case 3: return {1.0f, 0.95f, 0.72f, alpha};
        default: return {0.35f, 0.75f, 1.0f, alpha};
    }
}

// Transform a piece-local point (units, centered) into draw coordinates.
// GD rotation is clockwise; in y-up draw space that is (x cos + y sin, -x sin + y cos).
struct Xform {
    double cx, cy, c, s, sx, sy;
    CCPoint operator()(double lx, double ly) const {
        double x = lx * sx, y = ly * sy;
        return {float(cx + x * c + y * s), float(cy - x * s + y * c)};
    }
};

Xform xformFor(deco::Placement const& p) {
    double r = p.rot * 3.141592653589793 / 180.0;
    return {p.x + 120, p.y + 120, std::cos(r), std::sin(r),
            p.sx * (p.flipX ? -1 : 1), p.sy * (p.flipY ? -1 : 1)};
}

void Preview::poly(std::vector<CCPoint> pts, ccColor4F fill, float borderW, ccColor4F border) {
    if (pts.size() < 3) return;
    m_draw->drawPolygon(pts.data(), unsigned(pts.size()), fill, borderW, border);
}

void Preview::drawSources() {
    using K = deco::ObjKind;
    for (auto const& o : m_src.objects) {
        double r = o.rot * 3.141592653589793 / 180.0;
        Xform xf{o.x + 120, o.y + 120, std::cos(r), std::sin(r), 1, 1};
        double hw = o.w / 2, hh = o.h / 2;
        auto rect = [&] { return std::vector<CCPoint>{xf(-hw, -hh), xf(hw, -hh), xf(hw, hh), xf(-hw, hh)}; };
        auto circle = [&](double rad, int segs = 20) {
            std::vector<CCPoint> pts;
            for (int i = 0; i < segs; ++i) {
                double a = 2 * 3.141592653589793 * i / segs;
                pts.push_back(xf(std::cos(a) * rad, std::sin(a) * rad));
            }
            return pts;
        };
        switch (o.kind) {
            case K::Solid:
                poly(rect(), {0.55f, 0.58f, 0.65f, 0.55f}, 1.5f, {0.85f, 0.88f, 0.95f, 0.8f});
                break;
            case K::Slope:
                poly({xf(-hw, -hh), xf(hw, -hh), xf(hw, hh)}, {0.55f, 0.58f, 0.65f, 0.55f}, 1.5f, {0.85f, 0.88f, 0.95f, 0.8f});
                break;
            case K::Hazard:
                poly({xf(-hw, -hh), xf(hw, -hh), xf(0, hh)}, {1.f, 0.35f, 0.25f, 0.75f}, 1.5f, {1.f, 0.6f, 0.5f, 0.9f});
                break;
            case K::Pad:
                poly(circle(std::max(hw, hh)), {1.f, 0.85f, 0.2f, 0.7f}, 1.5f, {1.f, 1.f, 0.6f, 0.9f});
                break;
            case K::Orb:
                poly(circle(std::max(hw, hh)), {0.2f, 0.9f, 1.f, 0.25f}, 2.f, {0.2f, 0.9f, 1.f, 0.95f});
                m_draw->drawDot(xf(0, 0), 3.f, {0.2f, 0.9f, 1.f, 0.95f});
                break;
            case K::Portal:
                poly(rect(), {0.9f, 0.3f, 1.f, 0.18f}, 2.f, {0.9f, 0.3f, 1.f, 0.9f});
                break;
            case K::Collectible:
                poly(circle(std::max(hw, hh) * 0.7), {1.f, 0.9f, 0.3f, 0.8f}, 1.5f, {1.f, 1.f, 0.8f, 0.9f});
                break;
            default:
                poly(rect(), {0.5f, 0.5f, 0.55f, 0.16f}, 1.f, {0.6f, 0.6f, 0.65f, 0.4f});
                break;
        }
    }
}

void Preview::drawPlacement(deco::Placement const& p) {
    auto fill = channelColor(p.channel, p.front ? 0.95f : 0.8f);
    auto edge = p.detail ? channelColor(p.detail, 0.95f) : fill;
    auto xf = xformFor(p);
    double hw = p.nativeW / 2, hh = p.nativeH / 2;
    auto rectPts = [&](double inset = 0) {
        double a = hw - inset, b = hh - inset;
        return std::vector<CCPoint>{xf(-a, -b), xf(a, -b), xf(a, b), xf(-a, b)};
    };
    auto circlePts = [&](double rx, double ry, int segs = 24) {
        std::vector<CCPoint> pts;
        for (int i = 0; i < segs; ++i) {
            double a = 2 * 3.141592653589793 * i / segs;
            pts.push_back(xf(std::cos(a) * rx, std::sin(a) * ry));
        }
        return pts;
    };
    using S = deco::Shape;
    switch (p.shape) {
        case S::Panel:
            poly(rectPts(), fill, 1.f, edge);
            break;
        case S::Bricks: {
            poly(rectPts(), {fill.r, fill.g, fill.b, 0.55f}, 1.f, edge);
            for (double yy = -hh + p.nativeH / 3; yy < hh - 1; yy += p.nativeH / 3)
                m_draw->drawSegment(xf(-hw, yy), xf(hw, yy), 0.8f, {0, 0, 0, 0.35f});
            break;
        }
        case S::Outline:
            poly(rectPts(), {fill.r, fill.g, fill.b, 0.08f}, 2.2f, edge);
            break;
        case S::Glow:
            for (int i = 3; i >= 1; --i)
                poly(rectPts((i - 1) * std::min(hw, hh) / 3.0), {fill.r, fill.g, fill.b, 0.10f + (3 - i) * 0.05f});
            break;
        case S::GlowLine:
        case S::Highlight: {
            auto a = p.shape == S::Highlight ? xf(-hw, -hh) : xf(-hw, 0);
            auto b = p.shape == S::Highlight ? xf(hw, hh) : xf(hw, 0);
            m_draw->drawSegment(a, b, float(std::max(1.2, p.nativeH * p.sy / 8)), fill);
            break;
        }
        case S::Shine: {
            std::vector<CCPoint> pts;
            double rOut = std::max(hw, hh), rIn = rOut * 0.22;
            for (int i = 0; i < 8; ++i) {
                double a = 3.141592653589793 / 2 + i * 3.141592653589793 / 4;
                double rad = i % 2 == 0 ? rOut : rIn;
                pts.push_back(xf(std::cos(a) * rad, std::sin(a) * rad));
            }
            poly(pts, fill);
            break;
        }
        case S::Circle:
            poly(circlePts(hw, hh), {fill.r, fill.g, fill.b, 0.10f}, 2.2f, edge);
            break;
        case S::Ring:
            poly(circlePts(hw, hh), {fill.r, fill.g, fill.b, 0.10f}, 3.f, edge);
            m_draw->drawSegment(xf(-hw * 0.7, 0), xf(hw * 0.7, 0), 1.4f, edge);
            break;
        case S::Disc:
        case S::Dot:
            poly(circlePts(hw, hh), fill);
            break;
        case S::Quarter: {
            std::vector<CCPoint> pts{xf(-hw, -hh)};
            for (int i = 0; i <= 10; ++i) {
                double a = (3.141592653589793 / 2) * i / 10;
                pts.push_back(xf(-hw + std::sin(a) * p.nativeW, -hh + (1 - std::cos(a)) * p.nativeH));
            }
            poly(pts, fill);
            break;
        }
        case S::Gear: {
            poly(circlePts(hw * 0.82, hh * 0.82), fill);
            for (int i = 0; i < 8; ++i) {
                double a = i * 3.141592653589793 / 4;
                m_draw->drawDot(xf(std::cos(a) * hw * 0.92, std::sin(a) * hh * 0.92),
                                float(std::max(1.5, std::min(hw, hh) * p.sx * 0.16)), fill);
            }
            poly(circlePts(hw * 0.34, hh * 0.34), {0.05f, 0.06f, 0.10f, 0.9f});
            break;
        }
        case S::Chain:
            for (double yy = -hh; yy < hh - 4; yy += 9)
                m_draw->drawSegment(xf(0, yy), xf(0, yy + 5), 1.6f, fill);
            break;
        case S::Rod:
            m_draw->drawSegment(xf(0, -hh), xf(0, hh), 2.4f, fill);
            break;
        case S::Grass:
            for (double xx = -hw; xx < hw; xx += std::max(4.0, p.nativeW / 5))
                poly({xf(xx, -hh), xf(xx + p.nativeW / 5, -hh), xf(xx + p.nativeW / 10, hh)}, fill);
            break;
        case S::Cloud:
            poly(circlePts(hw * 0.55, hh * 0.75), {fill.r, fill.g, fill.b, 0.75f});
            m_draw->drawDot(xf(-hw * 0.45, -hh * 0.15), float(hh * p.sy * 0.55), {fill.r, fill.g, fill.b, 0.75f});
            m_draw->drawDot(xf(hw * 0.45, -hh * 0.15), float(hh * p.sy * 0.55), {fill.r, fill.g, fill.b, 0.75f});
            break;
        case S::Pipe:
            poly(rectPts(), {fill.r, fill.g, fill.b, 0.30f}, 2.4f, edge);
            m_draw->drawSegment(xf(-hw, 0), xf(hw, 0), 1.2f, {edge.r, edge.g, edge.b, 0.6f});
            break;
        case S::Arrow:
            poly({xf(-hw, -hh * 0.35), xf(hw * 0.2, -hh * 0.35), xf(hw * 0.2, -hh), xf(hw, 0),
                  xf(hw * 0.2, hh), xf(hw * 0.2, hh * 0.35), xf(-hw, hh * 0.35)}, fill);
            break;
    }
}

void Preview::redraw() {
    m_draw->clear();
    auto const& b = m_src.bounds;
    // editor grid every 30 units + selection bounds
    ccColor4F gridCol{1, 1, 1, 0.07f};
    for (double x = 0; x <= b.w; x += 30)
        m_draw->drawSegment({float(x + 120), 120.f}, {float(x + 120), float(b.h + 120)}, 0.5f, gridCol);
    for (double y = 0; y <= b.h; y += 30)
        m_draw->drawSegment({120.f, float(y + 120)}, {float(b.w + 120), float(y + 120)}, 0.5f, gridCol);
    m_draw->drawSegment({120.f, 120.f}, {float(b.w + 120), 120.f}, 0.9f, {1, 1, 1, 0.30f});
    m_draw->drawSegment({float(b.w + 120), 120.f}, {float(b.w + 120), float(b.h + 120)}, 0.9f, {1, 1, 1, 0.30f});
    m_draw->drawSegment({float(b.w + 120), float(b.h + 120)}, {120.f, float(b.h + 120)}, 0.9f, {1, 1, 1, 0.30f});
    m_draw->drawSegment({120.f, float(b.h + 120)}, {120.f, 120.f}, 0.9f, {1, 1, 1, 0.30f});

    // back pieces (z order), then the selection itself, then front pieces
    auto sorted = m_plan.placements;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](auto const& a, auto const& c) { return a.zBase < c.zBase; });
    for (auto const& p : sorted) if (!p.front) drawPlacement(p);
    drawSources();
    for (auto const& p : sorted) if (p.front) drawPlacement(p);

    m_statsLabel->setString(truncate(m_stats, 88).c_str());
    std::string warn;
    int cover = deco::frontCoverageWarnings(m_plan.placements, m_src.objects);
    if (m_plan.droppedUnknown || m_plan.droppedInvalid)
        warn += fmt::format("Dropped {} unusable pieces from the model's answer.  ",
                            m_plan.droppedUnknown + m_plan.droppedInvalid);
    if (cover)
        warn += fmt::format("{} front piece{} cover gameplay - check readability!", cover, cover == 1 ? "" : "s");
    m_warnLabel->setString(truncate(warn, 92).c_str());
}

void Preview::setPlan(deco::PlanResult plan, std::string stats) {
    m_plan = std::move(plan);
    m_stats = std::move(stats);
    setBusy(false, "");
    redraw();
}

void Preview::setBusy(bool busy, std::string const& status) {
    for (auto* b : m_buttons) b->setEnabled(!busy);
    m_spinner->setVisible(busy);
    m_busyLabel->setVisible(busy);
    if (busy) m_busyLabel->setString(truncate(status, 60).c_str());
}

bool Preview::init(EditorUI* ui, Selection src) {
    if (!Popup::init(520, 320)) return false;
    m_ui = ui;
    m_src = std::move(src);
    setTitle("Decoration Preview");

    m_bg = CCLayerColor::create({10, 14, 24, 255}, 480, 200);
    m_bg->setPosition({20, 96});
    m_mainLayer->addChild(m_bg);

    m_root = CCNode::create();
    auto const& b = m_src.bounds;
    double scale = std::min(476.0 / (b.w + 240), 196.0 / (b.h + 240));
    m_root->setScale(float(scale));
    m_root->setPosition({float((480 - (b.w + 240) * scale) / 2),
                         float((200 - (b.h + 240) * scale) / 2)});
    m_bg->addChild(m_root);
    m_draw = CCDrawNode::create();
    m_root->addChild(m_draw);

    m_spinner = LoadingSpinner::create(30.f);
    m_spinner->setPosition({260, 206});
    m_spinner->setVisible(false);
    m_mainLayer->addChild(m_spinner, 5);
    m_busyLabel = addLabel(m_mainLayer, "", {260, 176}, .5f, "bigFont.fnt");
    m_busyLabel->setVisible(false);

    m_statsLabel = addLabel(m_mainLayer, "", {260, 82}, .44f);
    m_warnLabel = addLabel(m_mainLayer, "", {260, 64}, .42f);
    addLabel(m_mainLayer, "Gray = your selection - blue/purple/cream = channels 1/2/3 (your editor colors apply)", {260, 48}, .38f);

    m_buttons.push_back(addButton(m_buttonMenu, this, menu_selector(Preview::onDiscard), "Discard", {62, 24}, .52f));
    m_buttons.push_back(addButton(m_buttonMenu, this, menu_selector(Preview::onBack), "Back", {168, 24}, .52f));
    m_regen = addButton(m_buttonMenu, this, menu_selector(Preview::onRegenerate), "Regenerate", {300, 24}, .6f);
    m_buttons.push_back(m_regen);
    m_apply = addButton(m_buttonMenu, this, menu_selector(Preview::onApply), "Apply", {438, 24}, .72f);
    m_buttons.push_back(m_apply);
    return true;
}

Preview* Preview::create(EditorUI* ui, Selection src) {
    auto p = new Preview;
    if (p->init(ui, std::move(src))) { p->autorelease(); return p; }
    delete p;
    return nullptr;
}

void Preview::onBack(CCObject*) { onClose(nullptr); }

void Preview::onDiscard(CCObject*) {
    if (m_closing) return;
    m_closing = true;
    if (m_onCloseAll) m_onCloseAll();
    else onClose(nullptr);
}

void Preview::onRegenerate(CCObject*) {
    if (m_isBusy && m_isBusy()) return;
    if (!m_onRegenerate) { notice("The decorator window was closed."); return; }
    setBusy(true, "Generating a new variation...");
    m_onRegenerate();
}

void Preview::onApply(CCObject*) {
    auto ui = m_ui.lock();
    if (!ui || !ui->getParent() || LevelEditorLayer::get() != ui->m_editorLayer) {
        notice("The original editor is no longer open.");
        return;
    }
    // Final safety net: never paste an object the game says is not pure
    // decoration, even if the vocabulary probe had to fall back.
    auto const& probe = probeVocabulary();
    std::vector<deco::Placement> safe;
    int blocked = 0;
    for (auto const& p : m_plan.placements) {
        auto it = probe.isDecoration.find(p.objectId);
        if (it != probe.isDecoration.end() && !it->second) { ++blocked; continue; }
        safe.push_back(p);
    }
    if (safe.empty()) {
        notice("All generated pieces were blocked by the decoration-safety check. Nothing was added.");
        return;
    }
    m_apply->setEnabled(false);
    auto str = deco::serialize(safe, m_src.bounds, m_src.layer);
    auto pasted = ui->pasteObjects(str, true, false);
    if (!pasted || !pasted->count()) {
        m_apply->setEnabled(true);
        notice("The editor could not paste this decoration.");
        return;
    }
    ui->updateButtons();
    ui->updateObjectInfoLabel();
    Notification::create(fmt::format("Added {} decoration objects on editor layer {}{} - Undo removes them",
                                     pasted->count(), m_src.layer,
                                     blocked ? fmt::format(" ({} blocked by safety check)", blocked) : ""),
                         NotificationIcon::Success, 3.5f)->show();
    if (m_onCloseAll) m_onCloseAll();
    else onClose(nullptr);
}

// ---------------------------------------------------------------------------
// Decorator::deliverPlan - show or refresh the preview
// ---------------------------------------------------------------------------
void Decorator::deliverPlan(deco::PlanResult plan, int variation) {
    m_lastPlan = plan.placements;
    int dropped = plan.droppedUnknown + plan.droppedInvalid + plan.duplicatesRemoved;
    std::string stats = fmt::format("{} objects: {} - variation #{}{}",
        plan.placements.size(), deco::summarizePlan(plan.placements), variation,
        dropped ? fmt::format(" - {} dropped as unusable", dropped) : "");
    setBusy(false, "Done - review the preview");

    auto ui = m_ui.lock();
    if (!ui) return;
    if (auto pv = m_preview.lock()) {
        pv->setPlan(std::move(plan), stats);
        return;
    }
    auto preview = Preview::create(ui.data(), m_src);
    if (!preview) { notice("Could not open the preview."); return; }
    auto weakSelf = WeakRef(this);
    preview->setCallbacks(
        [weakSelf] { if (auto s = weakSelf.lock()) s->regenerateFromPreview(); },
        [weakSelf] { if (auto s = weakSelf.lock()) s->closeAll(); },
        [weakSelf] { auto s = weakSelf.lock(); return s && s->busy(); });
    m_preview = preview;
    preview->setPlan(std::move(plan), stats);
    preview->show();
}

} // namespace

class $modify(AIDecoratorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;
        auto menu = CCMenu::create();
        menu->setID("ai-decorator-menu"_spr);
        menu->setPosition({0, 0});
        auto size = CCDirector::sharedDirector()->getWinSize();
        auto btn = addButton(menu, this, menu_selector(AIDecoratorUI::openDecorator), "AI", {size.width - 30, size.height - 72}, .6f);
        btn->setID("ai-decorator-button"_spr);
        addChild(menu, 100);
        return true;
    }

    void openDecorator(CCObject*) {
        try {
            auto popup = Decorator::create(this, readSelection(this));
            if (popup) popup->show();
            else notice("Could not open AI Decorator.");
        } catch (std::exception const& e) { notice(e.what()); }
    }
};

