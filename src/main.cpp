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

class SystemMenu : public Popup<> {
protected:
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_tps = nullptr;

    void addButton(char const* text, CCPoint pos, SEL_MenuHandler cb, float scale = 0.7f) {
        auto spr = ButtonSprite::create(text);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::Center, pos);
    }

    bool setup() override {
        this->setTitle("System");
        this->setID("system-menu");

        m_status = CCLabelBMFont::create("", "bigFont.fnt");
        m_status->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0, 62});

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.8f);
        m_mainLayer->addChildAtPosition(m_info, Anchor::Center, {0, 40});

        addButton("Record", {-100, 5}, menu_selector(SystemMenu::onRecord));
        addButton("Replay", {0, 5},    menu_selector(SystemMenu::onReplay));
        addButton("Stop",   {100, 5},  menu_selector(SystemMenu::onStop));

        addButton("Save", {-55, -35}, menu_selector(SystemMenu::onSave));
        addButton("Load", {55, -35},  menu_selector(SystemMenu::onLoad));

        addButton("-", {-70, -75}, menu_selector(SystemMenu::onTpsDown), 0.6f);
        addButton("+", {70, -75},  menu_selector(SystemMenu::onTpsUp),   0.6f);
        m_tps = CCLabelBMFont::create("", "bigFont.fnt");
        m_tps->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_tps, Anchor::Center, {0, -75});

        auto hint = CCLabelBMFont::create("Restart the level (R) after Record / Replay", "chatFont.fnt");
        hint->setScale(0.6f);
        hint->setOpacity(160);
        m_mainLayer->addChildAtPosition(hint, Anchor::Center, {0, -102});

        refresh();
        return true;
    }

    void refresh() {
        m_status->setString(fmt::format("Mode: {}", sys::name(sys::mode)).c_str());
        m_info->setString(fmt::format("Inputs in macro: {}", sys::macro.size()).c_str());
        m_tps->setString(fmt::format("TPS {}", Mod::get()->getSettingValue<int64_t>("tps")).c_str());
    }

    void onRecord(CCObject*) { sys::setMode(Mode::Record); refresh(); }
    void onReplay(CCObject*) {
        if (sys::macro.empty()) return sys::notify("System: macro is empty");
        sys::setMode(Mode::Replay);
        refresh();
    }
    void onStop(CCObject*) { sys::setMode(Mode::Off); refresh(); }
    void onSave(CCObject*) { sys::save(); }
    void onLoad(CCObject*) { sys::load(); refresh(); }

    void changeTps(int64_t delta) {
        if (sys::mode != Mode::Off) return sys::notify("System: stop first to change TPS");
        int64_t v = Mod::get()->getSettingValue<int64_t>("tps") + delta;
        v = std::clamp<int64_t>(v, 60, 960);
        Mod::get()->setSettingValue<int64_t>("tps", v);
        refresh();
    }
    void onTpsDown(CCObject*) { changeTps(-60); }
    void onTpsUp(CCObject*)   { changeTps(60); }

public:
    static SystemMenu* create() {
        auto ret = new SystemMenu();
        if (ret->initAnchored(320.f, 230.f)) {
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
