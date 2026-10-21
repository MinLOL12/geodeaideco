#pragma once
// AI Decorator - platform-independent core.
//
// This header contains everything that does not need Geode/cocos: the
// decoration vocabulary the model may use, prompt/context construction,
// tolerant parsing + validation of the model's JSON plan, anchor
// resolution, cleanup (snap/dedupe), plan similarity (so "Regenerate" can
// detect a look-alike result and re-roll), and Geometry Dash level-string
// serialization. It is unit-tested by tests/plan.cpp with a plain C++
// compiler:  g++ -std=c++23 -Ivendor -Isrc tests/plan.cpp
//
// Safety model: the model NEVER supplies raw object IDs, level strings,
// triggers or group IDs. It picks decoration pieces by *name* from the
// fixed vocabulary below; this code maps names to IDs. On top of that,
// main.cpp probes every vocabulary ID at runtime with
// GameObject::createWithKey and removes anything whose in-game object type
// is not "Decoration", so a wrong entry in this table can never place a
// solid block or a hazard.

#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace deco {
using Json = nlohmann::json;

struct Bounds { double x, y, w, h; }; // absolute lower-left + size, level units

// What a selected object *is*, derived in main.cpp from the game's own
// GameObjectType. This is what lets the model decorate real structures
// instead of guessing what a bare numeric object ID means.
enum class ObjKind { Solid, Slope, Hazard, Pad, Orb, Portal, Deco, Trigger, Collectible, Other };

inline const char* kindName(ObjKind k) {
    switch (k) {
        case ObjKind::Solid: return "solid";
        case ObjKind::Slope: return "slope";
        case ObjKind::Hazard: return "hazard";
        case ObjKind::Pad: return "pad";
        case ObjKind::Orb: return "orb";
        case ObjKind::Portal: return "portal";
        case ObjKind::Deco: return "deco";
        case ObjKind::Trigger: return "trigger";
        case ObjKind::Collectible: return "collectible";
        default: return "other";
    }
}

inline char kindGridChar(ObjKind k) {
    switch (k) {
        case ObjKind::Solid: return '#';
        case ObjKind::Slope: return '/';
        case ObjKind::Hazard: return '^';
        case ObjKind::Pad: return '=';
        case ObjKind::Orb: return 'o';
        case ObjKind::Portal: return 'P';
        case ObjKind::Deco: return 'd';
        case ObjKind::Trigger: return 't';
        case ObjKind::Collectible: return '*';
        default: return '?';
    }
}

// Higher priority wins a cell in the ASCII grid map.
inline int kindPriority(ObjKind k) {
    switch (k) {
        case ObjKind::Hazard: return 9;
        case ObjKind::Orb: case ObjKind::Pad: case ObjKind::Portal: return 8;
        case ObjKind::Collectible: return 7;
        case ObjKind::Solid: return 6;
        case ObjKind::Slope: return 5;
        case ObjKind::Trigger: return 3;
        case ObjKind::Deco: return 2;
        default: return 1;
    }
}

// One object the user selected. x/y are the object's center relative to
// the selection bounds' lower-left; w/h are its real footprint in level
// units (measured from the object in the editor, not assumed 30x30).
struct SrcObject {
    ObjKind kind = ObjKind::Other;
    double x = 0, y = 0, w = 30, h = 30, rot = 0;
};

// ---------------------------------------------------------------------------
// Decoration vocabulary: the ONLY pieces the model may place, by name.
// `w`/`h` are fallback native sizes; main.cpp replaces them with sizes
// measured from the real objects in the user's game. `zBase` orders pieces
// within their Z layer (glow behind panels behind sparkles).
// ---------------------------------------------------------------------------
enum class Shape { Panel, Outline, Glow, GlowLine, Shine, Circle, Disc, Ring,
                   Quarter, Gear, Chain, Rod, Grass, Cloud, Pipe, Arrow, Dot,
                   Bricks, Highlight };

struct VocabEntry {
    const char* name;   // name the model uses
    int id;             // real GD object ID (never accepted from the model)
    Shape shape;        // how the preview draws it
    double w, h;        // native size in level units at scale 1
    int zBase;          // base Z order within its Z layer
    const char* note;   // one-line usage hint shown to the model
};

inline const std::vector<VocabEntry>& vocabulary() {
    static const std::vector<VocabEntry> v = {
        {"panel",       211,  Shape::Panel,    30, 30, -4,  "flat filled square; stretch with sx/sy into panels, bars and backplates"},
        {"panel_grid",  251,  Shape::Panel,    30, 30, -4,  "grid-textured filled square; techy panel fill"},
        {"panel_neon",  1186, Shape::Panel,    30, 30, -4,  "neon-edged filled square; bright framed panels"},
        {"panel_brick", 273,  Shape::Bricks,   30, 30, -5,  "non-solid brick-pattern fill; masonry panels and walls"},
        {"outline",     467,  Shape::Outline,  30, 30,  2,  "hollow square outline only; frame blocks, windows, borders"},
        {"glow",        503,  Shape::Glow,     30, 30, -10, "soft additive light gradient; backlight structures, large soft color washes"},
        {"glow_large",  1011, Shape::Glow,     60, 60, -11, "large soft light gradient; wide background light"},
        {"glow_small",  1292, Shape::Glow,     15, 15, -9,  "small soft light gradient; tight local glow"},
        {"glow_line",   1754, Shape::GlowLine, 30,  4, -1,  "thin glowing straight line; trims, underlines, light strips"},
        {"shine",       1021, Shape::Shine,    30, 30,  5,  "small rotating sparkle; sparse highlights and glints"},
        {"shine_large", 1019, Shape::Shine,    60, 60,  5,  "large rotating sparkle; focal glints, use rarely"},
        {"pulse_circle",  51, Shape::Circle,   30, 30,  2,  "pulsing ring outline; halos behind orbs/details"},
        {"pulse_disc",    50, Shape::Disc,     30, 30,  1,  "pulsing filled disc; soft round accents"},
        {"pulse_square", 496, Shape::Outline,  30, 30,  2,  "large pulsing hollow square; animated frames"},
        {"pulse_star",    54, Shape::Shine,    30, 30,  4,  "pulsing star; celebratory accents"},
        {"pulse_arrow",  132, Shape::Arrow,    30, 30,  4,  "pulsing arrow pointing right at rotation 0; direction hints"},
        {"gear",         678, Shape::Gear,     60, 60, -2,  "large colorable gear/cog; machinery motifs"},
        {"gear_small",   680, Shape::Gear,     30, 30, -2,  "small colorable gear/cog"},
        {"chain",         41, Shape::Chain,    30, 90, -3,  "tall hanging chain; suspend platforms from above"},
        {"chain_small",  110, Shape::Chain,    30, 30, -3,  "short chain segment"},
        {"rod",           15, Shape::Rod,      30, 90, -3,  "tall vertical support rod; hold up floating structures"},
        {"grass",        406, Shape::Grass,    30, 15,  3,  "grass tuft; place ON TOP surface of solids (anchor dy = anchor h/2)"},
        {"cloud",        222, Shape::Cloud,    60, 30, -8,  "large round cloud; sky background"},
        {"pipe",         237, Shape::Pipe,     30, 30, -3,  "straight square pipe segment; run along/behind structures"},
        {"pipe_corner",  238, Shape::Pipe,     30, 30, -3,  "corner square pipe segment; turn pipes"},
        {"split_ring",   998, Shape::Ring,     45, 45,  1,  "rotating split ring; techy circular detail"},
        {"dot",         1764, Shape::Dot,      10, 10,  4,  "tiny filled dot; rivets, stars, particle-like detail"},
        {"quarter",     1837, Shape::Quarter,  30, 30, -2,  "filled quarter-disc; rounded corners and arcs"},
        {"bg_bricks",    113, Shape::Bricks,   60, 30, -9,  "large background brick pattern; distant walls"},
        {"highlight",    227, Shape::Highlight,30, 30,  4,  "diagonal shine streak; put on block faces for a glossy look"},
    };
    return v;
}

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline const VocabEntry* findVocab(std::vector<VocabEntry> const& vocab, std::string const& name) {
    auto want = lower(name);
    for (auto const& e : vocab) if (want == lower(e.name)) return &e;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Generation options (set by the UI).
// ---------------------------------------------------------------------------
struct GenOptions {
    std::string prompt;        // user's words
    int variation = 0;         // nonce: changes every generation
    double temperature = 0.9;  // sampling temperature sent to the provider
    int targetMin = 20;        // suggested object count range
    int targetMax = 120;
    std::string layerHint = "back"; // "back" | "mixed" | "front"
    std::string previousSummary;    // on regenerate: what the last plan used
};

// Suggested object-count range from density setting (0 sparse, 1 balanced,
// 2 dense) and how much geometry/structure there is to decorate.
inline void densityTargets(int density, size_t srcCount, int& lo, int& hi) {
    double per = density <= 0 ? 0.55 : density == 1 ? 1.25 : 2.3;
    lo = std::clamp(int(std::round(srcCount * per * 0.7)), 6, 260);
    hi = std::clamp(int(std::round(srcCount * per * 1.3)), lo + 4, 400);
}

// ---------------------------------------------------------------------------
// Endpoint validation. HTTPS anywhere; plain HTTP only for local model
// servers (Ollama / LM Studio etc. on this machine).
// ---------------------------------------------------------------------------
inline bool validEndpoint(std::string const& url) {
    static const std::regex https(R"(^https://[A-Za-z0-9.\-]+(:[0-9]{1,5})?(/[A-Za-z0-9_./~%\-]*)?/chat/completions$)");
    static const std::regex local(R"(^http://(localhost|127\.0\.0\.1|\[::1\])(:[0-9]{1,5})?(/[A-Za-z0-9_./~%\-]*)?/chat/completions$)");
    return std::regex_match(url, https) || std::regex_match(url, local);
}

// ---------------------------------------------------------------------------
// Number helpers
// ---------------------------------------------------------------------------
inline double round1(double v) { return std::round(v * 10.0) / 10.0; }

inline std::string fmtNum(double v) {
    if (std::abs(v) < 0.0005) v = 0; // kill "-0"
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(8);
    out << v;
    return out.str();
}

// ---------------------------------------------------------------------------
// ASCII grid map of the selection. Language models reason about layout far
// better from a map than from a bare coordinate list.
// ---------------------------------------------------------------------------
struct GridMap { std::string text; double cell = 30; int cols = 0, rows = 0; };

inline GridMap buildGridMap(std::vector<SrcObject> const& srcs, Bounds b) {
    GridMap g;
    double cell = 30;
    while ((b.w / cell > 96 || b.h / cell > 48) && cell < 480) cell *= 2;
    g.cell = cell;
    g.cols = std::max(1, int(std::ceil(b.w / cell)));
    g.rows = std::max(1, int(std::ceil(b.h / cell)));
    std::vector<char> cells(g.cols * g.rows, ' ');
    std::vector<int> prio(g.cols * g.rows, 0);
    for (auto const& o : srcs) {
        double r = o.rot * 3.141592653589793 / 180.0;
        double ew = std::abs(std::cos(r)) * o.w + std::abs(std::sin(r)) * o.h;
        double eh = std::abs(std::sin(r)) * o.w + std::abs(std::cos(r)) * o.h;
        int x0 = std::clamp(int((o.x - ew / 2) / cell), 0, g.cols - 1);
        int x1 = std::clamp(int((o.x + ew / 2) / cell), 0, g.cols - 1);
        int y0 = std::clamp(int((o.y - eh / 2) / cell), 0, g.rows - 1);
        int y1 = std::clamp(int((o.y + eh / 2) / cell), 0, g.rows - 1);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx) {
                int idx = cy * g.cols + cx;
                int p = kindPriority(o.kind);
                if (p > prio[idx]) { prio[idx] = p; cells[idx] = kindGridChar(o.kind); }
            }
    }
    std::string out;
    for (int cy = g.rows - 1; cy >= 0; --cy) { // top row first
        out.append(cells.data() + cy * g.cols, g.cols);
        if (cy) out += '\n';
    }
    g.text = std::move(out);
    return g;
}

// ---------------------------------------------------------------------------
// Prompt construction
// ---------------------------------------------------------------------------
inline std::string buildSystemPrompt(std::vector<VocabEntry> const& vocab, GenOptions const& opt) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "You are an expert Geometry Dash level decorator. You decorate EXISTING geometry that the user selected in the editor. Design decoration that visibly belongs to those exact structures: frame them, backlight them, accent their surfaces, support floating parts, and fill nearby empty space with motifs that follow the structures' rhythm. Do NOT scatter random shapes or make a uniform repeating grid.\n\n"
         "OUTPUT: return ONLY one JSON object, no markdown, no commentary:\n"
         "{\"objects\":[{\"obj\":\"panel\",\"x\":60,\"y\":30,\"sx\":2,\"sy\":0.2,\"rotation\":0,\"channel\":1}]}\n\n"
         "Each item places ONE decoration piece chosen by NAME from this list (sizes are native width x height in level units at scale 1):\n";
    for (auto const& e : vocab)
        s << "- " << e.name << " (" << fmtNum(e.w) << "x" << fmtNum(e.h) << "): " << e.note << "\n";
    s << "\nFIELDS per item:\n"
         "- \"obj\" (required): a name from the list above, exactly as written.\n"
         "- Position (required), in level units, origin at the selection's lower-left, positive y is UP. Either \"x\"/\"y\" (the piece's center), OR \"anchor\" + \"dx\"/\"dy\": anchor is a selected object's index i, and the center becomes that object's center + (dx,dy). PREFER anchors to attach decoration to specific objects: an outline framing a block = same anchor, dx=0, dy=0, sx/sy matched to the block size; grass on a block = dy = that block's h/2; glow behind a structure = anchor its center block with a larger scale and layer \"back\"; chains/rods = anchor a floating platform and offset so the support visually connects it upward/downward.\n"
         "- \"sx\",\"sy\": independent scale, 0.05 to 10 (default 1). Stretch panels into bars (thin sy) and glow into washes.\n"
         "- \"rotation\": clockwise degrees (default 0).\n"
         "- \"channel\": 1 = primary, 2 = accent, 3 = highlight (default 1). The user picks the actual colors in the editor; use all three with intent (mostly 1, accents 2, sparing highlights 3).\n"
         "- \"detail\": optional secondary color channel 1-3 for two-tone pieces.\n"
         "- \"flipX\",\"flipY\": optional booleans.\n"
         "- \"layer\": \"back\" (default, behind gameplay) or \"front\". "
      << (opt.layerHint == "front"
            ? "The user wants the decoration mainly on the front layer, but still never hide hazards, orbs, pads or portals.\n"
          : opt.layerHint == "mixed"
            ? "Use \"back\" for most pieces; \"front\" only for small accents that never cover gameplay objects.\n"
            : "Keep everything on \"back\".\n")
      << "\nRULES:\n"
         "- Produce between " << opt.targetMin << " and " << opt.targetMax << " objects (hard cap 450).\n"
         "- Every piece, including its rotated corners, must stay within the selection bounds plus a 120-unit margin on each side.\n"
         "- Decorate the structures in the grid map and object list: continuous frames along connected blocks, glow behind structures (not floating alone), chains/rods where structures float, surface details (grass on top faces, highlights on faces, dots as rivets) repeated with the same spacing as the structure itself.\n"
         "- Keep gameplay readable: never place opaque front-layer pieces over hazard/orb/pad/portal centers; glow behind them is fine.\n"
         "- Align to the 30-unit editor grid when the structures are grid-aligned; use intentional symmetry or repeated spacing where the layout repeats.\n"
         "- Vary piece types and sizes; combine at least 3 different piece names.\n"
         "- This is variation #" << opt.variation << ". Make it a fresh composition, not the most generic/default arrangement.";
    if (!opt.previousSummary.empty())
        s << " A previous attempt mainly used: " << opt.previousSummary
          << ". Produce a noticeably DIFFERENT composition this time (different dominant pieces, placement strategy and rhythm).";
    return s.str();
}

inline Json buildContext(std::vector<SrcObject> const& srcs, Bounds b, GenOptions const& opt) {
    Json objs = Json::array();
    for (size_t i = 0; i < srcs.size(); ++i) {
        auto const& o = srcs[i];
        objs.push_back({{"i", int(i)}, {"k", kindName(o.kind)},
                        {"x", round1(o.x)}, {"y", round1(o.y)},
                        {"w", round1(o.w)}, {"h", round1(o.h)}, {"r", round1(o.rot)}});
    }
    auto grid = buildGridMap(srcs, b);
    return {
        {"request", opt.prompt},
        {"selection", {{"width", round1(b.w)}, {"height", round1(b.h)}, {"margin", 120}}},
        {"legend", "# solid, / slope, ^ hazard, = pad, o orb, P portal, d existing deco, t trigger, * collectible"},
        {"grid", {{"cell_units", grid.cell}, {"rows_top_to_bottom", grid.text}}},
        {"objects", std::move(objs)},
    };
}

// ---------------------------------------------------------------------------
// Response extraction. Real models wrap JSON in markdown fences, add prose,
// or return multi-part content. All of that is accepted; v1 rejected it,
// which made many otherwise-good answers fail.
// ---------------------------------------------------------------------------
inline std::string extractAssistantText(std::string const& responseText) {
    auto root = Json::parse(responseText);
    if (root.is_object() && root.contains("objects")) return responseText; // some gateways return the plan directly
    auto const& choices = root.at("choices");
    if (!choices.is_array() || choices.empty()) throw std::runtime_error("The API returned no choices.");
    auto const& msg = choices.at(0).at("message");
    auto const& content = msg.at("content");
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array()) { // multi-part content: [{"type":"text","text":"..."}]
        std::string joined;
        for (auto const& part : content)
            if (part.is_object() && part.contains("text") && part["text"].is_string())
                joined += part["text"].get<std::string>();
        if (!joined.empty()) return joined;
    }
    throw std::runtime_error("The API response had no text content.");
}

inline std::string extractJsonObject(std::string const& text) {
    auto first = text.find('{');
    auto last = text.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last <= first)
        throw std::runtime_error("The model did not return a JSON object.");
    return text.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// Plan parsing / validation / cleanup
// ---------------------------------------------------------------------------
struct Placement {
    std::string name;
    int objectId = 0;
    Shape shape = Shape::Panel;
    double nativeW = 30, nativeH = 30;
    int zBase = 0;
    double x = 0, y = 0, sx = 1, sy = 1, rot = 0;
    int channel = 1, detail = 0;
    bool flipX = false, flipY = false, front = false;
};

struct PlanResult {
    std::vector<Placement> placements;
    int droppedUnknown = 0;   // obj name not in the vocabulary
    int droppedInvalid = 0;   // out-of-range values / bad anchor / outside bounds
    int duplicatesRemoved = 0;
    std::string firstProblem; // human-readable first validation problem (for repair + UI)
};

namespace detail {
inline bool getNum(Json const& obj, char const* key, double& out) {
    if (!obj.contains(key) || !obj.at(key).is_number()) return false;
    out = obj.at(key).get<double>();
    return std::isfinite(out);
}
inline bool getInt(Json const& obj, char const* key, long long& out) {
    double d;
    if (!getNum(obj, key, d)) return false;
    if (std::abs(d - std::round(d)) > 1e-9) return false;
    out = static_cast<long long>(std::round(d));
    return true;
}
inline bool getBool(Json const& obj, char const* key, bool dflt) {
    if (!obj.contains(key)) return dflt;
    auto const& v = obj.at(key);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    return dflt;
}
}

// Parses the model's JSON text into placements. Individual bad items are
// dropped and counted (with the first problem recorded) instead of failing
// the whole plan; it throws only when nothing usable remains — the error
// message is specific enough to show the user and to feed back to the
// model in an automatic repair request.
inline PlanResult parsePlan(std::string const& text, std::vector<SrcObject> const& srcs,
                            Bounds b, std::vector<VocabEntry> const& vocab) {
    if (text.size() > 400000) throw std::runtime_error("The decoration response is too large.");
    Json root;
    try { root = Json::parse(extractJsonObject(text)); }
    catch (std::exception const&) { throw std::runtime_error("The model did not return valid JSON."); }
    if (!root.is_object() || !root.contains("objects") || !root["objects"].is_array())
        throw std::runtime_error("Expected a JSON object with an \"objects\" array.");
    auto const& arr = root["objects"];
    if (arr.empty()) throw std::runtime_error("The plan contains no objects.");
    if (arr.size() > 450) throw std::runtime_error("The plan has more than 450 objects.");

    PlanResult res;
    auto drop = [&](bool unknown, std::string why) {
        if (unknown) ++res.droppedUnknown; else ++res.droppedInvalid;
        if (res.firstProblem.empty()) res.firstProblem = std::move(why);
    };

    for (auto const& obj : arr) {
        if (!obj.is_object()) { drop(false, "an item is not a JSON object"); continue; }
        if (!obj.contains("obj") || !obj["obj"].is_string()) { drop(true, "an item is missing its \"obj\" name"); continue; }
        auto name = obj["obj"].get<std::string>();
        auto const* voc = findVocab(vocab, name);
        if (!voc) { drop(true, "unknown obj \"" + name + "\" (use only names from the provided list)"); continue; }

        Placement p;
        p.name = voc->name; p.objectId = voc->id; p.shape = voc->shape;
        p.nativeW = voc->w; p.nativeH = voc->h; p.zBase = voc->zBase;

        // Position: anchored to a selected object, or absolute x/y.
        long long anchor = -1;
        bool hasAnchor = detail::getInt(obj, "anchor", anchor);
        if (hasAnchor) {
            if (anchor < 0 || anchor >= static_cast<long long>(srcs.size())) {
                drop(false, "anchor index out of range"); continue;
            }
            double dx = 0, dy = 0;
            detail::getNum(obj, "dx", dx); detail::getNum(obj, "dy", dy);
            p.x = srcs[size_t(anchor)].x + dx;
            p.y = srcs[size_t(anchor)].y + dy;
        } else if (!detail::getNum(obj, "x", p.x) || !detail::getNum(obj, "y", p.y)) {
            drop(false, "an item has no position (needs x/y or anchor+dx/dy)"); continue;
        }

        if (!detail::getNum(obj, "sx", p.sx)) p.sx = 1;
        if (!detail::getNum(obj, "sy", p.sy)) p.sy = p.sx; // uniform if only sx given
        if (p.sx < 0.05 || p.sx > 10 || p.sy < 0.05 || p.sy > 10) {
            drop(false, "scale outside 0.05..10 for \"" + p.name + "\""); continue;
        }
        if (!detail::getNum(obj, "rotation", p.rot)) p.rot = 0;
        p.rot = std::fmod(p.rot, 360.0);
        if (p.rot < 0) p.rot += 360.0;

        long long ch = 1;
        if (obj.contains("channel") && !detail::getInt(obj, "channel", ch)) {
            drop(false, "channel must be an integer 1, 2 or 3"); continue;
        }
        if (ch < 1 || ch > 3) { drop(false, "channel outside 1..3"); continue; }
        p.channel = int(ch);
        long long det = 0;
        if (obj.contains("detail") && detail::getInt(obj, "detail", det) && det >= 1 && det <= 3)
            p.detail = int(det);

        p.flipX = detail::getBool(obj, "flipX", false);
        p.flipY = detail::getBool(obj, "flipY", false);
        p.front = detail::getBool(obj, "front", false);
        if (obj.contains("layer") && obj["layer"].is_string())
            p.front = lower(obj["layer"].get<std::string>()).starts_with('f');

        // Snap position/rotation to editor-friendly steps (models emit noise).
        p.x = std::round(p.x * 2) / 2;
        p.y = std::round(p.y * 2) / 2;
        p.rot = std::round(p.rot * 2) / 2;
        if (p.rot >= 360) p.rot -= 360;

        // Rotated extents of the actual piece must fit bounds + 120 margin.
        double r = p.rot * 3.141592653589793 / 180.0;
        double ex = (std::abs(std::cos(r)) * p.nativeW * p.sx + std::abs(std::sin(r)) * p.nativeH * p.sy) / 2;
        double ey = (std::abs(std::sin(r)) * p.nativeW * p.sx + std::abs(std::cos(r)) * p.nativeH * p.sy) / 2;
        if (p.x - ex < -120 || p.x + ex > b.w + 120 || p.y - ey < -120 || p.y + ey > b.h + 120) {
            drop(false, "\"" + p.name + "\" extends beyond the allowed margin"); continue;
        }
        res.placements.push_back(p);
    }

    // Dedupe near-identical pieces (models often repeat themselves).
    std::set<std::string> seen;
    std::vector<Placement> kept;
    kept.reserve(res.placements.size());
    for (auto const& p : res.placements) {
        std::string key = p.name + "|" + std::to_string(std::llround(p.x / 2)) + "|" +
                          std::to_string(std::llround(p.y / 2)) + "|" +
                          std::to_string(std::llround(p.sx * 20)) + "|" +
                          std::to_string(std::llround(p.sy * 20)) + "|" +
                          std::to_string(std::llround(p.rot / 5)) + "|" +
                          std::to_string(p.channel) + "|" + (p.front ? "f" : "b");
        if (seen.insert(key).second) kept.push_back(p);
        else ++res.duplicatesRemoved;
    }
    res.placements = std::move(kept);

    if (res.placements.empty()) {
        std::string msg = "No usable decoration in the model's answer";
        if (!res.firstProblem.empty()) msg += ": " + res.firstProblem;
        msg += " (" + std::to_string(res.droppedUnknown) + " unknown, " +
               std::to_string(res.droppedInvalid) + " invalid).";
        throw std::runtime_error(msg);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Plan comparison: lets "Regenerate" detect that the model returned (nearly)
// the same arrangement again, so it can automatically re-roll instead of
// showing the user the same design rotated.
// ---------------------------------------------------------------------------
inline std::set<std::string> fingerprint(std::vector<Placement> const& ps) {
    std::set<std::string> out;
    for (auto const& p : ps)
        out.insert(p.name + "|" + std::to_string(std::llround(p.x / 15)) + "|" +
                   std::to_string(std::llround(p.y / 15)) + "|" +
                   std::to_string(std::llround(p.sx * 2)) + "|" +
                   std::to_string(std::llround(p.sy * 2)) + "|" +
                   std::to_string(std::llround(p.rot / 15)) + "|" +
                   std::to_string(p.channel));
    return out;
}

inline double similarity(std::vector<Placement> const& a, std::vector<Placement> const& b) {
    if (a.empty() || b.empty()) return 0;
    auto fa = fingerprint(a), fb = fingerprint(b);
    size_t inter = 0;
    for (auto const& k : fa) if (fb.count(k)) ++inter;
    size_t uni = fa.size() + fb.size() - inter;
    return uni ? double(inter) / double(uni) : 0;
}

// Short "what did that plan mainly use" summary, fed back on regenerate so
// the model deliberately does something different.
inline std::string summarizePlan(std::vector<Placement> const& ps) {
    std::vector<std::pair<std::string, int>> counts;
    for (auto const& p : ps) {
        auto it = std::find_if(counts.begin(), counts.end(),
                               [&](auto const& kv) { return kv.first == p.name; });
        if (it == counts.end()) counts.push_back({p.name, 1}); else ++it->second;
    }
    std::sort(counts.begin(), counts.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    std::string out;
    for (size_t i = 0; i < counts.size() && i < 4; ++i) {
        if (i) out += ", ";
        out += counts[i].first + " x" + std::to_string(counts[i].second);
    }
    return out.empty() ? "(nothing)" : out;
}

// How many opaque front-layer pieces sit on gameplay centers (readability
// warning in the preview; glow/soft pieces behind gameplay are fine).
inline int frontCoverageWarnings(std::vector<Placement> const& ps, std::vector<SrcObject> const& srcs) {
    int n = 0;
    for (auto const& p : ps) {
        if (!p.front) continue;
        if (p.shape == Shape::Glow || p.shape == Shape::GlowLine || p.shape == Shape::Shine) continue;
        for (auto const& o : srcs) {
            if (o.kind != ObjKind::Hazard && o.kind != ObjKind::Orb &&
                o.kind != ObjKind::Pad && o.kind != ObjKind::Portal) continue;
            if (std::abs(p.x - o.x) <= o.w / 2 + 4 && std::abs(p.y - o.y) <= o.h / 2 + 4) { ++n; break; }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Serialization to a GD level-object string.
// Keys (gd.docs level-object): 1 id, 2 x, 3 y, 4 flipX, 5 flipY, 6 rotation,
// 20 editor layer, 21 main color channel, 22 secondary color channel,
// 24 Z layer, 25 Z order, 128/129 scale X/Y (2.2).
// Z layer values (Geode ZLayer enum): B1 = 3, T1 = 5.
// ---------------------------------------------------------------------------
inline std::string serialize(std::vector<Placement> const& ps, Bounds b, int editorLayer) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    for (auto const& p : ps) {
        out << "1," << p.objectId << ",2," << fmtNum(b.x + p.x) << ",3," << fmtNum(b.y + p.y);
        if (p.flipX) out << ",4,1";
        if (p.flipY) out << ",5,1";
        if (std::abs(p.rot) > 0.001) out << ",6," << fmtNum(p.rot);
        out << ",21," << p.channel;
        if (p.detail) out << ",22," << p.detail;
        out << ",20," << editorLayer << ",24," << (p.front ? 5 : 3) << ",25," << p.zBase
            << ",128," << fmtNum(p.sx) << ",129," << fmtNum(p.sy) << ';';
    }
    return out.str();
}

} // namespace deco
