#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
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
    inline float leftover = 0.f;    // for fixed-step scheduler

    const char* name(Mode m) {
        switch (m) {
            case Mode::Record: return "Recording";
            case Mode::Replay: return "Replaying";
            default: return "Off";
        }
    }

    void notify(std::string const& text) {
        Notification::create(text, NotificationIcon::None, 1.f)->show();
    }

    void setMode(Mode m) {
        mode = m;
        idx = 0;
        if (m == Mode::Record) macro.clear();
        notify(fmt::format("System: {}", name(m)));
    }

    std::filesystem::path file() {
        return Mod::get()->getSaveDir() / "macro.txt";
    }

    void save() {
        std::ofstream out(file());
        if (!out) return notify("System: cannot write file");
        for (auto& i : macro)
            out << i.frame << ' ' << i.button << ' ' << i.down << ' ' << i.player1 << '\n';
        notify(fmt::format("System: saved {} inputs", macro.size()));
    }

    void load() {
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
}

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
// F1 = record, F2 = replay, F3 = off, F4 = save to file, F5 = load from file

class $modify(SysKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat && PlayLayer::get()) {
            switch (key) {
                case KEY_F1: sys::setMode(Mode::Record); return true;
                case KEY_F2: sys::setMode(Mode::Replay); return true;
                case KEY_F3: sys::setMode(Mode::Off);    return true;
                case KEY_F4: sys::save();                return true;
                case KEY_F5: sys::load();                return true;
                default: break;
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
