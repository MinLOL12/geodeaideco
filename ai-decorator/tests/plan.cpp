#include "Plan.hpp"
#include <cassert>
#include <functional>

int main() {
    using namespace deco;
    Bounds b{100,200,300,120};
    Json item = {{"x",30},{"y",30},{"sx",2},{"sy",.2},{"rotation",45},{"channel",2}};
    auto encoded = [&](Json obj) { return Json{{"objects",Json::array({obj})}}.dump(); };
    auto tiles = parsePlan(encoded(item),b);
    assert(tiles.size()==1 && tiles[0].channel==2 && tiles[0].rot==45);
    auto str = serialize(tiles,b,7);
    assert(str=="1,211,2,130,3,230,6,45,21,2,20,7,24,-1,25,-5,128,2,129,0.2;");
    auto rejects = [&](std::string const& s) {
        bool bad = false;
        try { parsePlan(s,b); } catch (...) { bad = true; }
        assert(bad);
    };
    rejects("{}");
    rejects("{\"objects\":[]}");
    rejects("not json");
    rejects("```json\n"+encoded(item)+"\n```");
    rejects(std::string(200001,'x'));
    rejects(Json{{"objects",Json::array({1})}}.dump());
    for (auto const& key : {"x","y","sx","sy","rotation","channel"}) {
        auto tmp = item;
        tmp.erase(key);
        rejects(encoded(tmp));
        tmp = item;
        tmp[key] = "1";
        rejects(encoded(tmp));
    }
    for (double v : {-1.,0.,4.,1.5,1e18}) {
        auto tmp = item;
        tmp["channel"] = v;
        rejects(encoded(tmp));
    }
    auto tmp = item;
    tmp["id"] = 1;
    rejects(encoded(tmp));
    tmp = item;
    tmp["sx"] = 100;
    rejects(encoded(tmp));
    tmp = item;
    tmp["x"] = -120;
    rejects(encoded(tmp));
    tmp["x"] = 420;
    rejects(encoded(tmp));
    tmp = item;
    tmp["y"] = 240;
    rejects(encoded(tmp));
    tmp = item;
    tmp["rotation"] = 361;
    rejects(encoded(tmp));
    Json many = Json::array();
    for (int i=0;i<300;++i) many.push_back(item);
    assert(parsePlan(Json{{"objects",many}}.dump(),b).size()==300);
    many.push_back(item);
    rejects(Json{{"objects",many}}.dump());
    assert(validEndpoint("https://api.openai.com/v1/chat/completions"));
    assert(validEndpoint("https://example.org:443/api/v1/chat/completions"));
    assert(validEndpoint("https://example.org/chat/completions"));
    for (auto url : {"http://example.org/v1/chat/completions","https://key@example.org/v1/chat/completions", "https://example.org/v1/chat/completions?key=x","https://example.org/v1/chat/completions#x","https://example.org/v1/responses", "https://example.org/v1/chat/completions\r\nX: y"})
        assert(!validEndpoint(url));
}
