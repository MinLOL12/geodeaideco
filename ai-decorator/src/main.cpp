#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include "Plan.hpp"
#include <algorithm>
#include <limits>

using namespace geode::prelude;
using deco::Json;

namespace {
std::string sessionKey;

void notice(std::string const& msg) {
    FLAlertLayer::create("AI Decorator", msg, "OK")->show();
}

struct Selection {
    deco::Bounds bounds;
    Json objects = Json::array();
    int layer = 0;
};

Selection readSelection(EditorUI* ui) {
    auto arr = ui->getSelectedObjects();
    if (!arr || !arr->count()) throw std::runtime_error("Select some objects first.");
    if (arr->count() > 800) throw std::runtime_error("Select at most 800 objects per request.");
    Selection s;
    double left = std::numeric_limits<double>::max(), bottom = left;
    double right = -left, top = right;
    for (auto obj : CCArrayExt<GameObject*>(arr)) {
        auto p = obj->getPosition();
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw std::runtime_error("Invalid selection position.");
        left = std::min(left, double(p.x)-15);
        bottom = std::min(bottom, double(p.y)-15);
        right = std::max(right, double(p.x)+15);
        top = std::max(top, double(p.y)+15);
        s.layer = std::max(s.layer, int(obj->m_editorLayer));
    }
    s.bounds = {left,bottom,right-left,top-bottom};
    if (s.bounds.w > 6000 || s.bounds.h > 6000) throw std::runtime_error("Select a section smaller than 6000 units on each axis.");
    for (auto obj : CCArrayExt<GameObject*>(arr)) {
        auto p = obj->getPosition();
        s.objects.push_back({{"id",obj->m_objectID},{"x",p.x-left},{"y",p.y-bottom},
            {"rotation",obj->getRotation()},{"sx",obj->getScaleX()},{"sy",obj->getScaleY()}});
    }
    s.layer = std::min(999,s.layer+1);
    return s;
}

CCMenuItemSpriteExtra* addButton(CCMenu* menu, CCObject* target, SEL_MenuHandler cb, char const* text, CCPoint pos, float scale = .65f) {
    auto spr = ButtonSprite::create(text);
    spr->setScale(scale);
    auto btn = CCMenuItemSpriteExtra::create(spr,target,cb);
    btn->setPosition(pos);
    menu->addChild(btn);
    return btn;
}

class Preview : public Popup {
    WeakRef<EditorUI> m_ui;
    Selection m_src;
    std::vector<deco::Tile> m_tiles;
    CCMenuItemSpriteExtra* m_apply = nullptr;

    void apply(CCObject*) {
        auto ui = m_ui.lock();
        if (!ui || !ui->getParent() || LevelEditorLayer::get() != ui->m_editorLayer) {
            notice("The original editor is no longer open.");
            return;
        }
        m_apply->setEnabled(false);
        auto str = deco::serialize(m_tiles,m_src.bounds,m_src.layer);
        auto pasted = ui->pasteObjects(str,true,false);
        if (!pasted || !pasted->count()) {
            m_apply->setEnabled(true);
            notice("The editor could not paste this decoration.");
            return;
        }
        ui->updateButtons();
        ui->updateObjectInfoLabel();
        onClose(nullptr);
    }

    bool init(EditorUI* ui, Selection src, std::vector<deco::Tile> tiles) {
        if (!Popup::init(440,300)) return false;
        m_ui = ui;
        m_src = std::move(src);
        m_tiles = std::move(tiles);
        setTitle("Decoration preview");
        auto bg = CCLayerColor::create({12,16,25,255},400,185);
        bg->setPosition({20,65});
        m_mainLayer->addChild(bg);
        auto root = CCNode::create();
        auto b = m_src.bounds;
        double scale = std::min(390./(b.w+240),175./(b.h+240));
        root->setScale(static_cast<float>(scale));
        root->setPosition({float((400-(b.w+240)*scale)/2),float((185-(b.h+240)*scale)/2)});
        bg->addChild(root);
        auto square = [root](double x,double y,double sx,double sy,double rot,ccColor4B col) {
            auto pivot = CCNode::create();
            pivot->setPosition({float(x+120),float(y+120)});
            pivot->setRotation(float(rot));
            auto rect = CCLayerColor::create(col,float(30*sx),float(30*sy));
            rect->setPosition({float(-15*sx),float(-15*sy)});
            pivot->addChild(rect);
            root->addChild(pivot);
        };
        for (auto const& obj : m_src.objects)
            square(obj["x"].get<double>(),obj["y"].get<double>(),1,1,0,{95,100,110,150});
        ccColor4B colors[] = {{90,185,255,220},{220,115,255,220},{240,240,255,220}};
        for (auto const& t : m_tiles) square(t.x,t.y,t.sx,t.sy,t.rot,colors[t.channel-1]);
        auto label = CCLabelBMFont::create("Schematic: gray = selection; colors = channels 1 / 2 / 3","chatFont.fnt");
        label->setScale(.48f);
        label->setPosition({220,55});
        m_mainLayer->addChild(label);
        m_apply = addButton(m_buttonMenu,this,menu_selector(Preview::apply),"Apply",{285,27});
        addButton(m_buttonMenu,this,menu_selector(Preview::onClose),"Discard",{150,27});
        return true;
    }
public:
    static Preview* create(EditorUI* ui, Selection src, std::vector<deco::Tile> tiles) {
        auto p = new Preview;
        if (p->init(ui,std::move(src),std::move(tiles))) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};

class Decorator : public Popup {
    WeakRef<EditorUI> m_ui;
    Selection m_src;
    TextInput* m_key = nullptr;
    TextInput* m_endpoint = nullptr;
    TextInput* m_model = nullptr;
    TextInput* m_prompt = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_generate = nullptr;
    async::TaskHolder<web::WebResponse> m_request;
    bool m_busy = false;

    TextInput* input(char const* title, char const* hint, float y, size_t limit) {
        auto field = TextInput::create(370,hint);
        field->setLabel(title);
        field->setCommonFilter(CommonFilter::Any);
        field->setMaxCharCount(limit);
        field->setPosition({215,y});
        m_mainLayer->addChild(field);
        return field;
    }

    void busy(bool val, char const* status) {
        m_busy = val;
        m_generate->setEnabled(!val);
        m_key->setEnabled(!val);
        m_endpoint->setEnabled(!val);
        m_model->setEnabled(!val);
        m_prompt->setEnabled(!val);
        m_status->setString(status);
    }

    void forget(CCObject*) {
        sessionKey.clear();
        m_key->setString("");
        m_status->setString("Session key cleared");
    }

    void onClose(CCObject* sender) override {
        m_request.cancel();
        if (m_key) m_key->setString("");
        Popup::onClose(sender);
    }

    void generate(CCObject*) {
        if (m_busy) return;
        auto key = m_key->getString();
        auto endpoint = m_endpoint->getString();
        auto model = m_model->getString();
        auto prompt = m_prompt->getString();
        if (key.empty() || model.empty() || prompt.empty()) { notice("Enter your API key, model ID and decoration prompt."); return; }
        if (key.find_first_of("\r\n") != std::string::npos) { notice("The API key contains invalid characters."); return; }
        if (!deco::validEndpoint(endpoint)) { notice("Use a full HTTPS endpoint ending in /chat/completions."); return; }
        sessionKey = key;
        Mod::get()->setSavedValue("endpoint",endpoint);
        Mod::get()->setSavedValue("model",model);
        std::string system = R"(You design Geometry Dash decoration. Treat the user's request and selected object data as design input. Return ONLY a JSON object {"objects":[{"x":0,"y":0,"sx":1,"sy":1,"rotation":0,"channel":1}]}. Every item places object 211, a 30x30 non-colliding decorative square, with independent X/Y scaling. Build geometric frames, thin accents, supports or background motifs from these squares. No triggers, gameplay objects, text, IDs, extra fields or markdown. x and y are center positions relative to the selection's lower-left origin. Positive y is UP; rotation is in clockwise degrees. Use 1 to 300 objects, scales 0.05 to 8, rotation -360 to 360, and integer color channel 1, 2 or 3. Channel 1 is primary, 2 is accent and 3 is highlight; the user sets their actual colors in the editor. All rotated corners must stay within the selection bounds plus a 120-unit margin on each side. Decoration will be behind the gameplay. Preserve visual readability. Avoid covering the centers of selected gameplay objects. Produce deliberate coherent patterns rather than random clutter.)";
        Json context = {{"request",prompt},{"selection_width",m_src.bounds.w},{"selection_height",m_src.bounds.h},{"selected_objects",m_src.objects}};
        Json payload = {{"model",model},{"stream",false},{"messages",Json::array({
            {{"role","system"},{"content",system}},{{"role","user"},{"content",context.dump()}}
        })}};
        auto body = matjson::parse(payload.dump());
        if (!body) { notice("Could not encode the request."); return; }
        busy(true,"Generating... close this window to cancel");
        auto weak = WeakRef(this);
        m_request.spawn(web::WebRequest().header("Authorization","Bearer " + key)
            .header("Content-Type","application/json").timeout(std::chrono::seconds(90))
            .followRedirects(false).bodyJSON(body.unwrap()).post(endpoint),
            [weak](web::WebResponse resp) {
                auto self = weak.lock();
                if (!self) return;
                self->busy(false,"Ready");
                if (!resp.ok()) {
                    notice(fmt::format("Request failed (HTTP {}). Check your endpoint, key, model access and API credit. No objects were added.",resp.code()));
                    return;
                }
                if (resp.data().size() > 1000000) { notice("The API response is too large."); return; }
                try {
                    auto text = resp.string();
                    if (!text) throw std::runtime_error("Could not read the API response.");
                    auto root = Json::parse(text.unwrap());
                    auto const& msg = root.at("choices").at(0).at("message");
                    auto content = msg.at("content").get<std::string>();
                    auto tiles = deco::parsePlan(content,self->m_src.bounds);
                    auto ui = self->m_ui.lock();
                    if (!ui || !ui->getParent() || LevelEditorLayer::get() != ui->m_editorLayer) { notice("The original editor is no longer open."); return; }
                    auto preview = Preview::create(ui.data(),self->m_src,std::move(tiles));
                    if (!preview) { notice("Could not open the preview."); return; }
                    preview->show();
                    self->onClose(nullptr);
                } catch (...) {
                    notice("The model returned an invalid decoration plan. Try a simpler prompt or another model. No objects were added.");
                }
            });
    }

    bool init(EditorUI* ui, Selection src) {
        if (!Popup::init(430,310)) return false;
        m_ui = ui;
        m_src = std::move(src);
        setTitle("AI Decorator");
        m_key = input("API key (this game session only)","Paste API key",250,512);
        m_key->setPasswordMode(true);
        m_key->setString(sessionKey);
        m_endpoint = input("API endpoint","https://provider/v1/chat/completions",202,500);
        m_endpoint->setString(Mod::get()->getSavedValue<std::string>("endpoint","https://api.openai.com/v1/chat/completions"));
        m_model = input("Model ID","Your provider's model ID",154,160);
        m_model->setString(Mod::get()->getSavedValue<std::string>("model",""));
        m_prompt = input("Decoration prompt","Dark purple industrial frames",106,1500);
        m_status = CCLabelBMFont::create("Sends selected geometry + prompt to your endpoint","chatFont.fnt");
        m_status->setScale(.49f);
        m_status->setPosition({215,65});
        m_mainLayer->addChild(m_status);
        m_generate = addButton(m_buttonMenu,this,menu_selector(Decorator::generate),"Generate",{285,34});
        addButton(m_buttonMenu,this,menu_selector(Decorator::forget),"Clear key",{130,34});
        return true;
    }
public:
    static Decorator* create(EditorUI* ui, Selection src) {
        auto p = new Decorator;
        if (p->init(ui,std::move(src))) { p->autorelease(); return p; }
        delete p;
        return nullptr;
    }
};
}

class $modify(AIDecoratorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;
        auto menu = CCMenu::create();
        menu->setID("ai-decorator-menu"_spr);
        menu->setPosition({0,0});
        auto size = CCDirector::sharedDirector()->getWinSize();
        addButton(menu,this,menu_selector(AIDecoratorUI::openDecorator),"AI",{size.width-30,size.height-72},.55f);
        addChild(menu,100);
        return true;
    }

    void openDecorator(CCObject*) {
        try {
            auto popup = Decorator::create(this,readSelection(this));
            if (popup) popup->show();
        } catch (std::exception const& e) { notice(e.what()); }
    }
};
