#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
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

    inline std::string currentName;   // name of the loaded / last saved replay
    inline std::string nameField;     // text typed into the "File name" box (shared by all tabs)

    inline std::string u8(std::filesystem::path const& p) {
        auto s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    // all replays live here (created automatically)
    inline std::filesystem::path dir() {
        auto d = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }

    inline std::string sanitize(std::string const& s) {
        std::string out;
        for (char c : s) {
            if (static_cast<unsigned char>(c) < 32) continue;
            if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) continue;
            out += c;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        return out;
    }

    inline std::filesystem::path pathFor(std::string const& name) {
        std::string full = name + ".macro";
        return dir() / std::filesystem::path(std::u8string(full.begin(), full.end()));
    }

    inline bool saveAs(std::string name) {
        name = sanitize(name);
        if (name.empty()) return false;
        std::ofstream out(pathFor(name));
        if (!out) return false;
        for (auto& i : macro)
            out << i.frame << ' ' << i.button << ' ' << i.down << ' ' << i.player1 << '\n';
        currentName = name;
        return true;
    }

    inline bool loadFrom(std::string const& name) {
        std::ifstream in(pathFor(name));
        if (!in) return false;
        std::vector<Input> loaded;
        Input i{};
        int d, p;
        while (in >> i.frame >> i.button >> d >> p) {
            i.down = d;
            i.player1 = p;
            loaded.push_back(i);
        }
        macro = std::move(loaded);
        idx = 0;
        currentName = name;
        return true;
    }

    inline bool removeFile(std::string const& name) {
        std::error_code ec;
        return std::filesystem::remove(pathFor(name), ec);
    }

    inline std::vector<std::string> list() {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir(), ec)) {
            if (e.path().extension() == ".macro") names.push_back(u8(e.path().stem()));
        }
        std::sort(names.begin(), names.end(), [](std::string const& a, std::string const& b) {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
        });
        return names;
    }

    // name used by Save / Load / F4 / F5: what the user typed, else the selected replay, else "quicksave"
    inline std::string activeName() {
        std::string n = sanitize(nameField);
        if (!n.empty()) return n;
        return currentName.empty() ? std::string("quicksave") : currentName;
    }

    inline void autoSave() {
        std::string name = sanitize(nameField);
        if (name.empty()) {
            char buf[40];
            std::time_t t = std::time(nullptr);
            std::strftime(buf, sizeof buf, "auto_%Y%m%d_%H%M%S", std::localtime(&t));
            name = buf;
        }
        if (saveAs(name)) notify(fmt::format("System: auto-saved '{}'", name));
    }

    inline void setMode(Mode m) {
        // leaving Record mode: optionally save the fresh recording
        if (mode == Mode::Record && m != Mode::Record && !macro.empty()
            && Mod::get()->getSettingValue<bool>("autosave")) {
            autoSave();
        }
        mode = m;
        idx = 0;
        if (m == Mode::Record) {
            macro.clear();
            currentName.clear(); // a new recording must never overwrite a loaded replay
        }
        notify(fmt::format("System: {}", name(m)));
    }

    // F4 / F5
    inline void save() {
        std::string n = activeName();
        if (saveAs(n)) notify(fmt::format("System: saved '{}' ({} inputs)", n, macro.size()));
        else notify("System: cannot write file");
    }

    inline void load() {
        std::string n = activeName();
        if (loadFrom(n)) notify(fmt::format("System: loaded '{}' ({} inputs)", n, macro.size()));
        else notify(fmt::format("System: no replay '{}'", n));
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

    // applies the FPS limit (or restores the one the game had before we touched it)
    inline void applyFps() {
        static double orig = 0.0;
        auto dir = CCDirector::get();
        if (orig <= 0.0) orig = dir->getAnimationInterval();

        double interval = orig;
        if (Mod::get()->getSettingValue<bool>("fps-unlock")) {
            int64_t fps = std::max<int64_t>(Mod::get()->getSettingValue<int64_t>("fps"), 10);
            interval = 1.0 / static_cast<double>(fps);
        }
        dir->setAnimationInterval(interval);
        CCApplication::sharedApplication()->setAnimationInterval(interval);
    }
}

// ------------------------------------------------------------------ menu ----

namespace ui {
    static const ccColor3B PANEL   {16, 18, 24};
    static const ccColor3B SIDE    {11, 13, 17};
    static const ccColor3B CARD    {27, 31, 40};
    static const ccColor3B BORDER  {44, 49, 62};
    static const ccColor3B CHIP    {45, 50, 64};
    static const ccColor3B CHIP_B  {66, 73, 92};
    static const ccColor3B ACCENT  {59, 110, 245};
    static const ccColor3B ACCENT_B{110, 150, 255};
    static const ccColor3B WHITE   {255, 255, 255};
    static const ccColor3B GRAY    {150, 156, 168};

    inline ccColor4F f4(ccColor3B c) {
        return ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
    }

    // filled rounded rectangle centered on (0,0), drawn as a polygon
    inline CCDrawNode* shape(CCSize size, float radius, ccColor3B color) {
        auto d = CCDrawNode::create();
        float hw = size.width / 2, hh = size.height / 2;
        float r = std::min(radius, std::min(hw, hh));
        std::vector<CCPoint> pts;
        auto arc = [&](float cx, float cy, float a0) {
            for (int i = 0; i <= 6; i++) {
                float a = (a0 + 90.f * i / 6.f) * 3.14159265f / 180.f;
                pts.push_back(ccp(cx + r * cosf(a), cy + r * sinf(a)));
            }
        };
        arc(hw - r, hh - r, 0);
        arc(-hw + r, hh - r, 90);
        arc(-hw + r, -hh + r, 180);
        arc(hw - r, -hh + r, 270);
        d->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), f4(color), 0.f, ccc4f(0, 0, 0, 0));
        return d;
    }

    // rounded rectangle with a thin border
    inline CCNode* rounded(CCSize size, ccColor3B fill, ccColor3B border, float radius = 6.f) {
        auto n = CCNode::create();
        n->addChild(shape(size, radius, border));
        n->addChild(shape({size.width - 2, size.height - 2}, std::max(radius - 1.f, 1.f), fill));
        return n;
    }

    inline CCDrawNode* circle(float radius, ccColor3B color) {
        auto d = CCDrawNode::create();
        std::vector<CCPoint> pts;
        for (int i = 0; i < 24; i++) {
            float a = 2.f * 3.14159265f * i / 24.f;
            pts.push_back(ccp(radius * cosf(a), radius * sinf(a)));
        }
        d->drawPolygon(pts.data(), 24, f4(color), 0.f, ccc4f(0, 0, 0, 0));
        return d;
    }
}

class SystemMenu : public Popup<> {
protected:
    int m_tab = 0;
    std::vector<Ref<CCNode>> m_dyn; // everything that gets rebuilt on change
    TextInput* m_fpsInput = nullptr;
    TextInput* m_tpsInput = nullptr;
    TextInput* m_nameInput = nullptr;
    std::vector<std::string> m_files;
    int m_page = 0;

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
        m_fpsInput = nullptr;
        m_tpsInput = nullptr;
        m_nameInput = nullptr;
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
        auto bg = ui::rounded({w, h}, ui::CARD, ui::BORDER, 8.f);
        put(bg, x + w / 2, y + h / 2);
        text(title, "bigFont.fnt", 0.42f, ui::WHITE, 0.f, x + 10, y + h - 14);
        if (!right.empty()) text(right, "chatFont.fnt", 0.8f, ui::GRAY, 1.f, x + w - 10, y + h - 14);
    }

    TextInput* makeInput(std::string const& value, char const* placeholder, float x, float y) {
        auto in = TextInput::create(80.f, placeholder, "bigFont.fnt");
        in->setScale(0.7f);
        in->setFilter("0123456789");
        in->setMaxCharCount(4);
        in->setString(value);
        put(in, x, y);
        return in;
    }

    CCMenuItemSpriteExtra* chip(char const* label, bool selected, float w, float cx, float cy,
                                SEL_MenuHandler cb, int tag) {
        const float h = 22.f;
        auto node = CCNode::create();
        node->setContentSize({w, h});
        node->setAnchorPoint({0.5f, 0.5f});
        auto bg = ui::rounded({w, h}, selected ? ui::ACCENT : ui::CHIP,
                              selected ? ui::ACCENT_B : ui::CHIP_B, 6.f);
        bg->setPosition({w / 2, h / 2});
        node->addChild(bg);
        auto l = CCLabelBMFont::create(label, "bigFont.fnt");
        l->limitLabelWidth(w - 8, 0.38f, 0.1f);
        l->setPosition({w / 2, h / 2 + 1});
        node->addChild(l);
        auto item = CCMenuItemSpriteExtra::create(node, this, cb);
        item->setTag(tag);
        put(item, cx, cy, true);
        return item;
    }

    CCMenuItemSpriteExtra* toggle(bool on, float cx, float cy, SEL_MenuHandler cb) {
        auto node = CCNode::create();
        node->setContentSize({36, 18});
        node->setAnchorPoint({0.5f, 0.5f});
        auto track = ui::rounded({36, 18}, on ? ui::ACCENT : ui::CHIP,
                                 on ? ui::ACCENT_B : ui::CHIP_B, 9.f);
        track->setPosition({18, 9});
        node->addChild(track);
        auto knob = ui::circle(6.5f, ui::WHITE);
        knob->setPosition({on ? 27.f : 9.f, 9.f});
        node->addChild(knob);
        auto item = CCMenuItemSpriteExtra::create(node, this, cb);
        put(item, cx, cy, true);
        return item;
    }

    bool setup() override {
        this->setID("system-menu");
        m_bgSprite->setVisible(false);

        auto win = ui::rounded({m_size.width, m_size.height}, ui::PANEL, ui::BORDER, 10.f);
        m_mainLayer->addChildAtPosition(win, Anchor::Center);
        win->setZOrder(-10);

        auto side = ui::rounded({58, 244}, ui::SIDE, ui::BORDER, 8.f);
        m_mainLayer->addChildAtPosition(side, Anchor::Center, C(35, 130));
        side->setZOrder(-9);

        rebuild();
        return true;
    }

    void rebuild() {
        clearDyn();
        if (m_tab == 1) m_files = sys::list();

        bool hk = Mod::get()->getSettingValue<bool>("hotkeys");
        int64_t tps = Mod::get()->getSettingValue<int64_t>("tps");

        // sidebar
        chip("MACRO",   m_tab == 0, 50, 35, 215, menu_selector(SystemMenu::onTab), 0);
        chip("REPLAYS", m_tab == 1, 50, 35, 187, menu_selector(SystemMenu::onTab), 1);
        chip("OPTIONS", m_tab == 2, 50, 35, 159, menu_selector(SystemMenu::onTab), 2);
        text("v1.3.1", "chatFont.fnt", 0.7f, ui::GRAY, 0.5f, 35, 20);

        if (m_tab == 0) {
            // ---- Macro (mode chips) ----
            card(70, 133, 176, 119, "Macro", hk ? "F1 / F2 / F3" : "");
            chip("Off",    sys::mode == Mode::Off,    50, 102, 204, menu_selector(SystemMenu::onMode), 0);
            chip("Record", sys::mode == Mode::Record, 50, 158, 204, menu_selector(SystemMenu::onMode), 1);
            chip("Replay", sys::mode == Mode::Replay, 50, 214, 204, menu_selector(SystemMenu::onMode), 2);
            text(fmt::format("Status: {}", sys::name(sys::mode)), "chatFont.fnt", 0.85f, ui::WHITE, 0.5f, 158, 174);
            text("Restart the level (R) after switching", "chatFont.fnt", 0.6f, ui::GRAY, 0.5f, 158, 152);

            // ---- Recorder (info) ----
            card(256, 133, 176, 119, "Recorder", "");
            text("Inputs", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 208);
            text(std::to_string(sys::macro.size()), "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 208);
            text("Last frame", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 182);
            text(std::to_string(sys::macro.empty() ? 0 : sys::macro.back().frame),
                 "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 182);
            text("Mode", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 266, 156);
            text(sys::name(sys::mode), "chatFont.fnt", 0.85f, ui::WHITE, 1.f, 422, 156);

            // ---- File (save / load + file name) ----
            card(70, 8, 176, 119, "File", hk ? "F4 / F5" : "");
            chip("Save", false, 70, 118, 90, menu_selector(SystemMenu::onSave), 0);
            chip("Load", false, 70, 198, 90, menu_selector(SystemMenu::onLoad), 0);
            m_nameInput = TextInput::create(150.f, "File name...", "bigFont.fnt");
            m_nameInput->setScale(0.9f);
            m_nameInput->setMaxCharCount(24);
            m_nameInput->setString(sys::nameField);
            m_nameInput->setCallback([](std::string const& t) { sys::nameField = t; });
            put(m_nameInput, 158, 50);

            // ---- Display (FPS / TPS) ----
            card(256, 8, 176, 119, "Display", "");
            bool fpsU = Mod::get()->getSettingValue<bool>("fps-unlock");
            bool tpsU = Mod::get()->getSettingValue<bool>("tps-unlock");
            int64_t fps = Mod::get()->getSettingValue<int64_t>("fps");

            m_fpsInput = makeInput(std::to_string(fps), "FPS", 288, 88);
            text("FPS", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 320, 88);
            chip("Unlock FPS", fpsU, 68, 387, 88, menu_selector(SystemMenu::onUnlock), 0);

            m_tpsInput = makeInput(std::to_string(tps), "TPS", 288, 58);
            text("TPS", "chatFont.fnt", 0.85f, ui::GRAY, 0.f, 320, 58);
            chip("Unlock TPS", tpsU, 68, 387, 58, menu_selector(SystemMenu::onUnlock), 1);

            chip("Apply", false, 80, 344, 28, menu_selector(SystemMenu::onApply), 0);
        }
        else if (m_tab == 1) {
            bool autosave = Mod::get()->getSettingValue<bool>("autosave");

            // ---- Replay controls ----
            card(70, 8, 176, 244, "Replay", "");
            chip("Play", sys::mode == Mode::Replay, 150, 158, 206, menu_selector(SystemMenu::onMode), 2);
            chip("Delete",    false,    70, 118, 172, menu_selector(SystemMenu::onDelete), 0);
            chip("Clear",     false,    70, 198, 172, menu_selector(SystemMenu::onClear), 0);
            chip("Auto-save", autosave, 70, 118, 144, menu_selector(SystemMenu::onAutosave), 0);
            chip("Folder",    false,    70, 198, 144, menu_selector(SystemMenu::onOpenFolder), 0);

            auto sel = text(fmt::format("Selected: {}", sys::currentName.empty() ? "-" : sys::currentName),
                            "chatFont.fnt", 0.8f, ui::WHITE, 0.5f, 158, 104);
            sel->limitLabelWidth(160.f, 0.8f, 0.3f);
            text(fmt::format("Inputs: {}", sys::macro.size()), "chatFont.fnt", 0.8f, ui::GRAY, 0.5f, 158, 82);
            text("Click a replay on the right to select it,", "chatFont.fnt", 0.6f, ui::GRAY, 0.5f, 158, 52);
            text("then press Play", "chatFont.fnt", 0.6f, ui::GRAY, 0.5f, 158, 38);

            // ---- Saved replays (paged list) ----
            const int perPage = 7;
            int total = static_cast<int>(m_files.size());
            int pages = std::max(1, (total + perPage - 1) / perPage);
            m_page = std::clamp(m_page, 0, pages - 1);

            card(256, 8, 176, 244, "Saved replays", fmt::format("{} / {}", m_page + 1, pages));
            if (total == 0) {
                text("No saved replays yet", "chatFont.fnt", 0.85f, ui::GRAY, 0.5f, 344, 130);
            }
            for (int k = 0; k < perPage; k++) {
                int i = m_page * perPage + k;
                if (i >= total) break;
                chip(m_files[i].c_str(), m_files[i] == sys::currentName, 156, 344, 208 - k * 25.f,
                     menu_selector(SystemMenu::onPick), i);
            }
            chip("<", false, 40, 290, 24, menu_selector(SystemMenu::onPage), -1);
            chip(">", false, 40, 398, 24, menu_selector(SystemMenu::onPage), 1);
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
            card(70, 8, 362, 119, "System", "v1.3.1");
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

    void onSave(CCObject*) {
        std::string name = sys::sanitize(sys::nameField);
        if (name.empty()) return sys::notify("System: enter a file name first");
        if (sys::macro.empty()) return sys::notify("System: nothing recorded yet");
        if (sys::saveAs(name)) sys::notify(fmt::format("System: saved '{}' ({} inputs)", name, sys::macro.size()));
        else sys::notify("System: cannot write file");
        scheduleRebuild();
    }

    void onLoad(CCObject*) {
        if (sys::mode != Mode::Off) return sys::notify("System: stop first");
        std::string name = sys::sanitize(sys::nameField);
        if (name.empty()) return sys::notify("System: enter a file name first");
        if (sys::loadFrom(name)) sys::notify(fmt::format("System: loaded '{}' ({} inputs)", name, sys::macro.size()));
        else sys::notify(fmt::format("System: no replay '{}'", name));
        scheduleRebuild();
    }

    void onUnlock(CCObject* s) {
        int t = static_cast<CCNode*>(s)->getTag();
        if (t == 0) {
            bool cur = Mod::get()->getSettingValue<bool>("fps-unlock");
            Mod::get()->setSettingValue<bool>("fps-unlock", !cur);
            sys::applyFps();
        } else {
            if (sys::mode != Mode::Off) return sys::notify("System: stop first to change TPS");
            bool cur = Mod::get()->getSettingValue<bool>("tps-unlock");
            Mod::get()->setSettingValue<bool>("tps-unlock", !cur);
        }
        scheduleRebuild();
    }

    void onApply(CCObject*) {
        if (m_fpsInput) {
            int v = std::clamp(std::atoi(m_fpsInput->getString().c_str()), 10, 1000);
            Mod::get()->setSettingValue<int64_t>("fps", v);
        }
        if (m_tpsInput) {
            int v = std::clamp(std::atoi(m_tpsInput->getString().c_str()), 10, 2000);
            if (sys::mode != Mode::Off) sys::notify("System: stop first to change TPS");
            else Mod::get()->setSettingValue<int64_t>("tps", v);
        }
        sys::applyFps();
        sys::notify("System: display settings applied");
        scheduleRebuild();
    }

    void onDelete(CCObject*) {
        std::string name = sys::currentName;
        if (name.empty()) return sys::notify("System: select a replay first");
        if (sys::removeFile(name)) {
            sys::notify(fmt::format("System: deleted '{}'", name));
            sys::currentName.clear();
            if (sys::sanitize(sys::nameField) == name) sys::nameField.clear();
        } else {
            sys::notify("System: replay not found");
        }
        scheduleRebuild();
    }

    void onClear(CCObject*) {
        if (sys::mode != Mode::Off) return sys::notify("System: stop first");
        sys::macro.clear();
        sys::idx = 0;
        sys::currentName.clear();
        sys::notify("System: macro cleared");
        scheduleRebuild();
    }

    void onAutosave(CCObject*) {
        bool cur = Mod::get()->getSettingValue<bool>("autosave");
        Mod::get()->setSettingValue<bool>("autosave", !cur);
        scheduleRebuild();
    }

    void onOpenFolder(CCObject*) {
        geode::utils::file::openFolder(sys::dir());
    }

    void onPick(CCObject* s) {
        int i = static_cast<CCNode*>(s)->getTag();
        if (sys::mode != Mode::Off) return sys::notify("System: stop first");
        if (i < 0 || i >= static_cast<int>(m_files.size())) return;
        if (sys::loadFrom(m_files[i])) {
            sys::nameField = m_files[i];
            sys::notify(fmt::format("System: selected '{}' ({} inputs)", m_files[i], sys::macro.size()));
        }
        else {
            sys::notify("System: cannot read replay");
        }
        scheduleRebuild();
    }

    void onPage(CCObject* s) {
        m_page += static_cast<CCNode*>(s)->getTag();
        if (m_page < 0) m_page = 0;
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
        bool unlocked = Mod::get()->getSettingValue<bool>("tps-unlock");

        // locked + not recording/replaying -> leave the game alone
        if (!unlocked && sys::mode == Mode::Off) {
            sys::leftover = 0.f;
            return CCScheduler::update(dt);
        }

        int64_t tps = unlocked ? Mod::get()->getSettingValue<int64_t>("tps") : 240;
        tps = std::clamp<int64_t>(tps, 10, 2000);

        float step = 1.f / static_cast<float>(tps);
        sys::leftover += dt;
        int steps = static_cast<int>(sys::leftover / step);
        sys::leftover -= steps * step;

        for (int i = 0; i < steps; i++)
            CCScheduler::update(step);
    }
};

// re-apply the saved FPS limit once when the game starts
class $modify(SysMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool once = false;
        if (!once) {
            once = true;
            sys::applyFps();
        }
        return true;
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
