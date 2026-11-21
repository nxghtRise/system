#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <fstream>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------- state ----

struct Input {
    int frame;
    int button;
    bool down;
    bool player1;
};

enum class Mode { Off, Record, Replay };

namespace sys {
    inline Mode mode = Mode::Off;
    inline std::vector<Input> macro;
    inline size_t idx = 0;          // next input to replay
    inline int frame = 0;           // current physics frame
    inline bool botInput = false;   // true while the bot itself presses a button
    inline float leftover = 0.f;    // for the fixed-step scheduler

    inline const char* name(Mode m) {
        switch (m) {
            case Mode::Record: return "Recording";
            case Mode::Replay: return "Replaying";
            default: return "Off";
        }
    }

    inline void notify(std::string const& text) {
        Notification::create(text, NotificationIcon::None, 1.f)->show();
    }

    inline void setMode(Mode m) {
        mode = m;
        idx = 0;
        if (m == Mode::Record) macro.clear();
        notify(fmt::format("System: {}", name(m)));
    }

    inline std::filesystem::path file() {
        return Mod::get()->getSaveDir() / "macro.txt";
    }

    inline void save() {
        std::ofstream out(file());
        if (!out) return notify("System: cannot write file");
        for (auto& i : macro)
            out << i.frame << ' ' << i.button << ' ' << i.down << ' ' << i.player1 << '\n';
        notify(fmt::format("System: saved {} inputs", macro.size()));
    }

    inline void load() {
        std::ifstream in(file());
        if (!in) return notify("System: no saved macro");
        macro.clear();
        Input i{};
        int d, p;
        while (in >> i.frame >> i.button >> d >> p) {
            i.down = d;
            i.player1 = p;
            macro.push_back(i);
        }
        idx = 0;
        notify(fmt::format("System: loaded {} inputs", macro.size()));
    }

    inline enumKeyCodes menuKey() {
        auto s = Mod::get()->getSettingValue<std::string>("menu-key");
        if (s == "Tab")      return KEY_Tab;
        if (s == "Home")     return KEY_Home;
        if (s == "End")      return KEY_End;
        if (s == "Delete")   return KEY_Delete;
        if (s == "PageUp")   return KEY_PageUp;
        if (s == "PageDown") return KEY_PageDown;
        if (s == "F6")       return KEY_F6;
        if (s == "F7")       return KEY_F7;
        if (s == "F8")       return KEY_F8;
        return KEY_Insert;
    }
}

// ------------------------------------------------------------------ menu ----

namespace ui {
    static const ccColor3B PANEL {14, 16, 21};
    static const ccColor3B SIDE  {10, 12, 16};
    static const ccColor3B CARD  {24, 27, 34};
    static const ccColor3B CHIP  {43, 48, 60};
    static const ccColor3B ACCENT{59, 110, 245};
    static const ccColor3B WHITE {255, 255, 255};
    static const ccColor3B GRAY  {150, 156, 168};

    inline CCScale9Sprite* rounded(CCSize size, ccColor3B color) {
        auto s = CCScale9Sprite::create("square02_001.png");
        s->setContentSize(size);
        s->setColor(color);
        s->setOpacity(255);
        return s;
    }
}

class SystemMenu : public Popup<> {
protected:
    int m_tab = 0;
    std::vector<Ref<CCNode>> m_dyn; // everything that gets rebuilt on change

    // design coordinates: (0,0) = bottom-left of the window -> offset from center
    CCPoint C(float x, float y) const {
        return {x - m_size.width / 2, y - m_size.height / 2};
    }

    void put(CCNode* n, float x, float y, bool inMenu = false) {
        CCNode* parent = inMenu ? static_cast<CCNode*>(m_buttonMenu) : static_cast<CCNode*>(m_mainLayer);
        parent->addChildAtPosition(n, Anchor::Center, C(x, y));
        if (!inMenu) n->setZOrder(-5); // below the buttons
        m_dyn.push_back(Ref<CCNode>(n));
    }

    void clearDyn() {
        for (auto& n : m_dyn) n->removeFromParent();
        m_dyn.clear();
    }

    CCLabelBMFont* text(std::string const& s, char const* font, float scale,
                        ccColor3B color, float anchorX, float x, float y) {
        auto l = CCLabelBMFont::create(s.c_str(), font);
        l->setScale(scale);
        l->setColor(color);
        put(l, x, y);
        l->setAnchorPoint({anchorX, 0.5f});
        return l;
    }

    // a rounded card with a title on the left and a "keybind" label on the right
    void card(float x, float y, float w, float h, char const* title, std::string const& right) {
        auto bg = ui::rounded({w, h}, ui::CARD);
        put(bg, x + w / 2, y + h / 2);
        text(title, "bigFont.fnt", 0.42f, ui::WHITE, 0.f, x + 10, y + h - 14);
        text(right, "chatFont.fnt", 0.8f, ui::GRAY, 1.f, x + w - 10, y + h - 14);
    }

    CCMenuItemSpriteExtra* chip(char const* label, bool selected, float w, float cx, float cy,
                                SEL_MenuHandler cb, int tag) {
        auto node = CCNode::create();
        node->setContentSize({w, 20});
        node->setAnchorPoint({0.5f, 0.5f});
        auto bg = ui::rounded({w, 20}, selected ? ui::ACCENT : ui::CHIP);
        bg->setPosition({w / 2, 10});
        node->addChild(bg);
        auto l = CCLabelBMFont::create(label, "bigFont.fnt");
        l->limitLabelWidth(w - 8, 0.38f, 0.1f);
        l->setPosition({w / 2, 10});
        node->addChild(l);
        auto item = CCMenuItemSpriteExtra::create(node, this, cb);
        item->setTag(tag);
        put(item, cx, cy, true);
        return item;
    }

    CCMenuItemSpriteExtra* toggle(bool on, float cx, float cy, SEL_MenuHandler cb) {
        auto node = CCNode::create();
        node->setContentSize({34, 16});
        node->setAnchorPoint({0.5f, 0.5f});
        auto track = ui::rounded({34, 16}, on ? ui::ACCENT : ui::CHIP);
        track->setPosition({17, 8});
        node->addChild(track);
        auto knob = CCDrawNode::create();
        knob->drawDot(ccp(on ? 26.f : 8.f, 8.f), 6.f, ccc4f(1, 1, 1, 1));
        node->addChild(knob);
        auto item = CCMenuItemSpriteExtra::create(node, this, cb);
        put(item, cx, cy, true);
        return item;
    }

    bool setup() override {
        this->setID("system-menu");
        m_bgSprite->setVisible(false);

        auto win = ui::rounded({m_size.width, m_size.height}, ui::PANEL);
        m_mainLayer->addChildAtPosition(win, Anchor::Center);
        win->setZOrder(-10);

        auto side = ui::rounded({54, 244}, ui::SIDE);
        m_mainLayer->addChildAtPosition(side, Anchor::Center, C(35, 130));
        side->setZOrder(-9);

        rebuild();
        return true;
    }

    void rebuild() {
        clearDyn();

        bool hk = Mod::get()->getSettingValue<bool>("hotkeys");
        int64_t tps = Mod::get()->getSettingValue<int64_t>("tps");

        // sidebar
        chip("MACRO",   m_tab == 0, 46, 35, 215, menu_selector(SystemMenu::onTab), 0);
        chip("OPTIONS", m_tab == 1, 46, 35, 187, menu_selector(SystemMenu::onTab), 1);
        text("v1.1.0", "chatFont.fnt", 0.7f, ui::GRAY, 0.5f, 35, 20);

        if (m_tab == 0) {
            // ---- Macro (mode chips) ----
            card(70, 133, 176, 119, "Macro", hk ? "F1 / F2 / F3" : "None");
            chip("Off",    sys::mode == Mode::Off,    50, 102, 204, menu_selector(SystemMenu::onMode), 0);
            chip("Record", sys::mode == Mode::Record, 50, 158, 204, menu_selector(SystemMenu::onMode), 1);
            chip("Replay", sys::mode == Mode::Replay, 50, 214, 204, menu_selector(SystemMenu::onMode), 2);
            text(fmt::format("Status: {}", sys::name(sys::mode)), "chatFont.fnt", 0.85f, ui::WHITE, 0.5f, 158, 174);
            text("Restart the level (R) after switching", "chatFont.fnt", 0.6f, ui::GRAY, 0.5f, 158, 152);

            // ---- Recorder (info) ----
            card(256, 133, 176, 119, "Recorder", "None");
            text("Inputs", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 208);
            text(std::to_string(sys::macro.size()), "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 208);
            text("Last frame", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 182);
            text(std::to_string(sys::macro.empty() ? 0 : sys::macro.back().frame),
                 "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 182);
            text("Mode", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 156);
            text(sys::name(sys::mode), "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 156);

            // ---- File ----
            card(70, 8, 176, 119, "File", hk ? "F4 / F5" : "None");
            chip("Save", false, 70, 118, 81, menu_selector(SystemMenu::onSave), 0);
            chip("Load", false, 70, 198, 81, menu_selector(SystemMenu::onLoad), 0);
            text("Stored in the mod save folder", "chatFont.fnt", 0.6f, ui::GRAY, 0.5f, 158, 51);

            // ---- Timer (TPS presets) ----
            card(256, 8, 176, 119, "Timer", "None");
            int presets[5] = {60, 120, 240, 480, 960};
            for (int i = 0; i < 5; i++) {
                chip(std::to_string(presets[i]).c_str(), tps == presets[i], 30,
                     277 + i * 34.f, 81, menu_selector(SystemMenu::onTps), presets[i]);
            }
            text(fmt::format("TPS {}", tps), "chatFont.fnt", 0.85f, ui::WHITE, 0.5f, 344, 51);
        }
        else {
            // ---- Hotkeys ----
            card(70, 133, 176, 119, "Hotkeys", hk ? "On" : "Off");
            text("Enable F1-F5", "chatFont.fnt", 0.85f, ui::WHITE, 0.f, 80, 208);
            toggle(hk, 224, 208, menu_selector(SystemMenu::onHotkeys));
            text("F1 record   F2 replay", "chatFont.fnt", 0.75f, ui::GRAY, 0.f, 80, 180);
            text("F3 stop   F4 save   F5 load", "chatFont.fnt", 0.75f, ui::GRAY, 0.f, 80, 160);

            // ---- Menu key ----
            card(256, 133, 176, 119, "Menu", Mod::get()->getSettingValue<std::string>("menu-key"));
            text("Open / close key", "chatFont.fnt", 0.85f, ui::WHITE, 0.f, 266, 208);
            text("Change it in the mod settings", "chatFont.fnt", 0.7f, ui::GRAY, 0.f, 266, 180);
            text("(Geode > System > Menu key)", "chatFont.fnt", 0.7f, ui::GRAY, 0.f, 266, 160);

            // ---- About ----
            card(70, 8, 362, 119, "System", "v1.1.0");
            text("Macro bot for GD 2.2074 (Geode 4.10.2)", "chatFont.fnt", 0.85f, ui::WHITE, 0.f, 80, 80);
            text("Records inputs frame by frame and replays them.", "chatFont.fnt", 0.75f, ui::GRAY, 0.f, 80, 56);
            text("Do not submit bot runs as legit records.", "chatFont.fnt", 0.75f, ui::GRAY, 0.f, 80, 36);
        }
    }

    // rebuilding inside a button callback would delete the pressed button,
    // so do it on the next frame
    void scheduleRebuild() {
        Loader::get()->queueInMainThread([self = Ref<SystemMenu>(this)] {
            self->rebuild();
        });
    }

    void onTab(CCObject* s) {
        m_tab = static_cast<CCNode*>(s)->getTag();
        scheduleRebuild();
    }

    void onMode(CCObject* s) {
        int t = static_cast<CCNode*>(s)->getTag();
        if (t == 1) sys::setMode(Mode::Record);
        else if (t == 2) {
            if (sys::macro.empty()) sys::notify("System: macro is empty");
            else sys::setMode(Mode::Replay);
        }
        else sys::setMode(Mode::Off);
        scheduleRebuild();
    }

    void onSave(CCObject*) { sys::save(); }
    void onLoad(CCObject*) { sys::load(); scheduleRebuild(); }

    void onTps(CCObject* s) {
        if (sys::mode != Mode::Off) return sys::notify("System: stop first to change TPS");
        int v = static_cast<CCNode*>(s)->getTag();
        Mod::get()->setSettingValue<int64_t>("tps", v);
        scheduleRebuild();
    }

    void onHotkeys(CCObject*) {
        bool cur = Mod::get()->getSettingValue<bool>("hotkeys");
        Mod::get()->setSettingValue<bool>("hotkeys", !cur);
        scheduleRebuild();
    }

public:
    static SystemMenu* create() {
        auto ret = new SystemMenu();
        if (ret->initAnchored(440.f, 260.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    static void toggle() {
        auto scene = CCDirector::get()->getRunningScene();
        if (scene) {
            if (auto old = typeinfo_cast<SystemMenu*>(scene->getChildByID("system-menu"))) {
                old->keyBackClicked(); // closes the popup
                return;
            }
        }
        if (auto menu = SystemMenu::create()) menu->show();
    }
};

// ------------------------------------------------------- game layer hooks ----

class $modify(SysBase, GJBaseGameLayer) {
    void processCommands(float dt) {
        // current physics frame comes from the game itself, so that practice-mode
        // checkpoints restore it correctly
        sys::frame = m_gameState.m_currentProgress;

        if (sys::mode == Mode::Replay) {
            while (sys::idx < sys::macro.size() && sys::macro[sys::idx].frame <= sys::frame) {
                auto& in = sys::macro[sys::idx++];
                sys::botInput = true;
                GJBaseGameLayer::handleButton(in.down, in.button, in.player1);
                sys::botInput = false;
            }
        }

        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (sys::mode == Mode::Replay && !sys::botInput) {
            return; // ignore real player input while a macro is playing
        }
        if (sys::mode == Mode::Record) {
            sys::macro.push_back({sys::frame, button, down, isPlayer1});
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SysPlay, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();

        // after a respawn the game state is back at the checkpoint (or frame 0)
        int f = m_gameState.m_currentProgress;
        sys::frame = f;

        if (sys::mode == Mode::Record) {
            // drop every input recorded after the point we respawned at
            sys::macro.erase(
                std::remove_if(sys::macro.begin(), sys::macro.end(),
                    [f](Input const& i) { return i.frame >= f; }),
                sys::macro.end());
        }
        else if (sys::mode == Mode::Replay) {
            sys::idx = 0;
            while (sys::idx < sys::macro.size() && sys::macro[sys::idx].frame < f)
                sys::idx++;
        }
    }

    void onQuit() {
        if (sys::mode != Mode::Off) sys::setMode(Mode::Off);
        PlayLayer::onQuit();
    }
};

// ----------------------------------------------- fixed physics step (TPS) ----

class $modify(SysScheduler, CCScheduler) {
    void update(float dt) {
        if (sys::mode == Mode::Off) {
            sys::leftover = 0.f;
            return CCScheduler::update(dt);
        }

        float step = 1.f / static_cast<float>(Mod::get()->getSettingValue<int64_t>("tps"));
        sys::leftover += dt;
        int steps = static_cast<int>(sys::leftover / step);
        sys::leftover -= steps * step;

        for (int i = 0; i < steps; i++)
            CCScheduler::update(step);
    }
};

// ------------------------------------------------------------- hotkeys ----

class $modify(SysKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            if (key == sys::menuKey()) {
                SystemMenu::toggle();
                return true;
            }
            if (PlayLayer::get() && Mod::get()->getSettingValue<bool>("hotkeys")) {
                switch (key) {
                    case KEY_F1: sys::setMode(Mode::Record); return true;
                    case KEY_F2: sys::setMode(Mode::Replay); return true;
                    case KEY_F3: sys::setMode(Mode::Off);    return true;
                    case KEY_F4: sys::save();                return true;
                    case KEY_F5: sys::load();                return true;
                    default: break;
                }
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
