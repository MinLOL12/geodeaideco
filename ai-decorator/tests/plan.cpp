// Platform-independent tests for the AI Decorator core (src/Plan.hpp).
// Build & run:  g++ -std=c++23 -Wall -Wextra -Werror -Ivendor -Isrc tests/plan.cpp -o tests/plan-test && ./tests/plan-test
#include "Plan.hpp"
#include <cassert>
#include <iostream>

using namespace deco;

static std::vector<SrcObject> sampleSrcs() {
    // A little platform: three solids in a row, a spike on top, a floating orb.
    return {
        {ObjKind::Solid, 15, 15, 30, 30, 0},
        {ObjKind::Solid, 45, 15, 30, 30, 0},
        {ObjKind::Solid, 75, 15, 30, 30, 0},
        {ObjKind::Hazard, 45, 45, 30, 30, 0},
        {ObjKind::Orb, 105, 75, 30, 30, 0},
    };
}

static bool throwsWith(std::function<void()> fn, std::string const& needle) {
    try { fn(); } catch (std::exception const& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

int main() {
    Bounds b{100, 200, 300, 120};
    auto srcs = sampleSrcs();
    auto const& vocab = vocabulary();

    // ---- vocabulary sanity: unique names/ids, positive sizes ----
    {
        std::set<std::string> names; std::set<int> ids;
        for (auto const& e : vocab) {
            assert(names.insert(e.name).second);
            assert(ids.insert(e.id).second);
            assert(e.w > 0 && e.h > 0);
            assert(findVocab(vocab, e.name)->id == e.id);
        }
        assert(findVocab(vocab, "GLOW")->id == 503);            // case-insensitive
        assert(findVocab(vocab, "not_a_piece") == nullptr);
        assert(vocab.size() >= 20);
    }

    // ---- basic parse + serialize round trip ----
    {
        auto plan = parsePlan(
            R"({"objects":[{"obj":"panel","x":30,"y":30,"sx":2,"sy":0.2,"rotation":45,"channel":2}]})",
            srcs, b, vocab);
        assert(plan.placements.size() == 1);
        auto const& p = plan.placements[0];
        assert(p.objectId == 211 && p.channel == 2 && p.rot == 45 && !p.front);
        auto str = serialize(plan.placements, b, 7);
        assert(str == "1,211,2,130,3,230,6,45,21,2,20,7,24,3,25,-4,128,2,129,0.2;");
    }

    // ---- defaults: only obj + position is enough; sy defaults to sx ----
    {
        auto plan = parsePlan(R"({"objects":[{"obj":"glow","x":60,"y":60,"sx":3}]})", srcs, b, vocab);
        auto const& p = plan.placements[0];
        assert(p.sx == 3 && p.sy == 3 && p.channel == 1 && p.rot == 0);
        // rotation 0 is omitted from the level string
        assert(serialize(plan.placements, b, 0).find(",6,") == std::string::npos);
    }

    // ---- anchored placement resolves against the real selected object ----
    {
        auto plan = parsePlan(R"({"objects":[{"obj":"grass","anchor":1,"dx":0,"dy":15}]})", srcs, b, vocab);
        auto const& p = plan.placements[0];
        assert(p.x == 45 && p.y == 30); // solid #1 center (45,15) + (0,15) = its top face
        // bad anchor is dropped, not fatal, when other items are fine
        auto plan2 = parsePlan(
            R"({"objects":[{"obj":"grass","anchor":99},{"obj":"dot","x":10,"y":10}]})", srcs, b, vocab);
        assert(plan2.placements.size() == 1 && plan2.droppedInvalid == 1);
    }

    // ---- tolerant extraction: markdown fences + prose are accepted (v1 rejected these) ----
    {
        std::string fenced = "Here is your decoration:\n```json\n{\"objects\":[{\"obj\":\"shine\",\"x\":45,\"y\":75,\"channel\":3}]}\n```\nHope you like it!";
        auto plan = parsePlan(fenced, srcs, b, vocab);
        assert(plan.placements.size() == 1 && plan.placements[0].name == "shine");
    }

    // ---- extra/unknown fields are ignored; unknown pieces dropped + reported ----
    {
        auto plan = parsePlan(
            R"({"objects":[{"obj":"panel","x":30,"y":30,"id":9999,"comment":"hi"},{"obj":"trigger_thing","x":1,"y":1}]})",
            srcs, b, vocab);
        assert(plan.placements.size() == 1 && plan.placements[0].objectId == 211); // model-supplied id ignored
        assert(plan.droppedUnknown == 1);
        assert(plan.firstProblem.find("trigger_thing") != std::string::npos);
    }

    // ---- hard failures carry specific messages ----
    assert(throwsWith([&] { parsePlan("{}", srcs, b, vocab); }, "objects"));
    assert(throwsWith([&] { parsePlan(R"({"objects":[]})", srcs, b, vocab); }, "no objects"));
    assert(throwsWith([&] { parsePlan("not json", srcs, b, vocab); }, "JSON"));
    assert(throwsWith([&] { parsePlan(std::string(400001, 'x'), srcs, b, vocab); }, "too large"));
    assert(throwsWith([&] { parsePlan(R"({"objects":[{"obj":"nope","x":1,"y":1}]})", srcs, b, vocab); }, "No usable"));
    assert(throwsWith([&] { parsePlan(R"({"objects":[{"x":1,"y":1}]})", srcs, b, vocab); }, "obj"));

    // ---- value validation ----
    {
        // out-of-range channel / scale / outside margin are dropped per-item
        auto plan = parsePlan(
            R"({"objects":[
                {"obj":"panel","x":30,"y":30,"channel":7},
                {"obj":"panel","x":30,"y":30,"sx":99},
                {"obj":"panel","x":-500,"y":30},
                {"obj":"panel","x":30,"y":30}
            ]})", srcs, b, vocab);
        assert(plan.placements.size() == 1 && plan.droppedInvalid == 3);
    }

    // ---- bounds use the piece's real native size and rotation ----
    {
        // glow_large is 60x60: centered at x=-100 with scale 1 its left edge is -130 < -120
        auto plan = parsePlan(
            R"({"objects":[{"obj":"glow_large","x":-100,"y":60},{"obj":"glow_large","x":-80,"y":60}]})",
            srcs, b, vocab);
        assert(plan.placements.size() == 1 && plan.placements[0].x == -80);
    }

    // ---- rotation normalization + snapping ----
    {
        auto plan = parsePlan(
            R"({"objects":[{"obj":"panel","x":30.24,"y":30.26,"rotation":-90.3}]})", srcs, b, vocab);
        auto const& p = plan.placements[0];
        assert(p.x == 30.0 && p.y == 30.5);
        assert(p.rot == 269.5); // -90.3 wrapped to 269.7, snapped to 269.5
    }

    // ---- flip / detail / front layer serialization ----
    {
        auto plan = parsePlan(
            R"({"objects":[{"obj":"pipe_corner","x":60,"y":60,"flipX":true,"detail":3,"layer":"front"}]})",
            srcs, b, vocab);
        auto str = serialize(plan.placements, b, 2);
        assert(str.find("1,238,") == 0);
        assert(str.find(",4,1") != std::string::npos);   // flipX
        assert(str.find(",22,3") != std::string::npos);  // detail channel
        assert(str.find(",24,5") != std::string::npos);  // front Z layer T1
    }

    // ---- dedupe ----
    {
        auto plan = parsePlan(
            R"({"objects":[
                {"obj":"dot","x":10,"y":10},{"obj":"dot","x":10.4,"y":9.6},
                {"obj":"dot","x":40,"y":10}
            ]})", srcs, b, vocab);
        assert(plan.placements.size() == 2 && plan.duplicatesRemoved == 1);
    }

    // ---- count limits ----
    {
        Json many = Json::array();
        for (int i = 0; i < 451; ++i)
            many.push_back({{"obj", "dot"}, {"x", 10 + i % 200}, {"y", 10 + (i / 200) * 10}});
        assert(throwsWith([&] { parsePlan(Json{{"objects", many}}.dump(), srcs, b, vocab); }, "450"));
    }

    // ---- assistant-text extraction: string content, multipart content, direct plan ----
    {
        auto t1 = extractAssistantText(R"({"choices":[{"message":{"content":"{\"objects\":[]}"}}]})");
        assert(t1 == "{\"objects\":[]}");
        auto t2 = extractAssistantText(R"({"choices":[{"message":{"content":[{"type":"text","text":"abc"},{"type":"text","text":"def"}]}}]})");
        assert(t2 == "abcdef");
        auto t3 = extractAssistantText(R"({"objects":[{"obj":"dot","x":1,"y":1}]})");
        assert(t3.find("dot") != std::string::npos);
        assert(throwsWith([&] { extractAssistantText(R"({"choices":[]})"); }, "no choices"));
    }

    // ---- similarity: identical plans ~1, different plans low ----
    {
        auto a = parsePlan(R"({"objects":[{"obj":"panel","x":30,"y":30},{"obj":"glow","x":60,"y":30}]})", srcs, b, vocab).placements;
        auto same = a;
        assert(similarity(a, same) > 0.99);
        auto moved = parsePlan(R"({"objects":[{"obj":"chain","x":200,"y":90},{"obj":"gear","x":240,"y":60}]})", srcs, b, vocab).placements;
        assert(similarity(a, moved) < 0.1);
        // "same but rotated" (the v1 complaint) must NOT look identical
        auto rotated = parsePlan(R"({"objects":[{"obj":"panel","x":30,"y":30,"rotation":90},{"obj":"glow","x":60,"y":30,"rotation":180}]})", srcs, b, vocab).placements;
        assert(similarity(a, rotated) < 0.85);
        assert(summarizePlan(a).find("panel x1") != std::string::npos);
    }

    // ---- front-layer gameplay coverage warnings ----
    {
        auto front = parsePlan(
            R"({"objects":[{"obj":"panel","anchor":3,"layer":"front"},{"obj":"glow","anchor":3,"layer":"front"},{"obj":"panel","anchor":3}]})",
            srcs, b, vocab).placements;
        // only the opaque front panel on the hazard counts (front glow and back panel are fine)
        assert(frontCoverageWarnings(front, srcs) == 1);
    }

    // ---- grid map shows the structure ----
    {
        auto g = buildGridMap(srcs, Bounds{0, 0, 120, 90});
        assert(g.cols == 4 && g.rows == 3 && g.cell == 30);
        assert(g.text.find('#') != std::string::npos);
        assert(g.text.find('^') != std::string::npos);
        assert(g.text.find('o') != std::string::npos);
        // top row comes first: the orb (y=75) is above the solids
        assert(g.text.find('o') < g.text.find('#'));
        // huge selections downsample instead of exploding
        auto big = buildGridMap(srcs, Bounds{0, 0, 6000, 3000});
        assert(big.cols <= 96 && big.rows <= 48);
    }

    // ---- prompt/context construction carries the real information ----
    {
        GenOptions opt;
        opt.prompt = "industrial frames";
        opt.variation = 4242;
        densityTargets(1, srcs.size(), opt.targetMin, opt.targetMax);
        auto sys = buildSystemPrompt(vocab, opt);
        assert(sys.find("panel") != std::string::npos);
        assert(sys.find("glow") != std::string::npos);
        assert(sys.find("anchor") != std::string::npos);
        assert(sys.find("4242") != std::string::npos);
        assert(sys.find(std::to_string(opt.targetMin)) != std::string::npos);
        auto ctx = buildContext(srcs, Bounds{0, 0, 120, 90}, opt);
        assert(ctx["objects"].size() == srcs.size());
        assert(ctx["objects"][3]["k"] == "hazard");
        assert(ctx["grid"]["rows_top_to_bottom"].get<std::string>().find('^') != std::string::npos);
        assert(ctx["request"] == "industrial frames");
        // previous-plan feedback shows up on regenerate
        opt.previousSummary = "panel x30, glow x12";
        assert(buildSystemPrompt(vocab, opt).find("DIFFERENT") != std::string::npos);
        // density ranges scale and clamp
        int lo, hi;
        densityTargets(0, 10, lo, hi); assert(lo >= 6 && hi > lo);
        densityTargets(2, 400, lo, hi); assert(hi <= 400);
    }

    // ---- endpoint validation (incl. local model servers) ----
    assert(validEndpoint("https://api.openai.com/v1/chat/completions"));
    assert(validEndpoint("https://openrouter.ai/api/v1/chat/completions"));
    assert(validEndpoint("https://example.org:443/api/v1/chat/completions"));
    assert(validEndpoint("http://127.0.0.1:11434/v1/chat/completions"));   // Ollama
    assert(validEndpoint("http://localhost:1234/v1/chat/completions"));    // LM Studio
    for (auto url : {
            "http://example.org/v1/chat/completions",              // plain http only allowed locally
            "http://192.168.1.5:11434/v1/chat/completions",
            "https://key@example.org/v1/chat/completions",
            "https://example.org/v1/chat/completions?key=x",
            "https://example.org/v1/chat/completions#x",
            "https://example.org/v1/responses",
            "https://example.org/v1/chat/completions\r\nX: y"})
        assert(!validEndpoint(url));

    std::cout << "all plan tests passed\n";
    return 0;
}
