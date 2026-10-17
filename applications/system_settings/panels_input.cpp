// Mouse & Touchpad, Keyboard (input sources, typing, special keys, shortcuts) and Accessibility.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "glib_support.hpp"
#include "kconfig.hpp"
#include "panels.hpp"
#include "panels_shared.hpp"
#include "ui.hpp"

using namespace mayo;

namespace {

constexpr const char* kMouse = "org.gnome.desktop.peripherals.mouse";
constexpr const char* kTouchpad = "org.gnome.desktop.peripherals.touchpad";
constexpr const char* kKeyboard = "org.gnome.desktop.peripherals.keyboard";
constexpr const char* kInputSources = "org.gnome.desktop.input-sources";
constexpr const char* kInterface = "org.gnome.desktop.interface";

std::string ms(float v) { return std::to_string(static_cast<int>(std::lround(v))) + " ms"; }

/// Pointer acceleration as an on/off switch: "flat" is off, anything else on.
FormRow accelerationRow(const std::string& title, const gx::Key& key) {
    auto t = Toggle("", key.getString() != "flat").onChange([key](bool on) { key.setString(on ? "default" : "flat"); });
    key.watch([key, t]() mutable { t.setOn(key.getString() != "flat"); });
    if (!key) t.disabled();
    return FormRow(title, t, "Faster movements travel further");
}

/// Whether a touchpad is connected (libinput names them "... Touchpad").
bool hasTouchpad() {
    std::ifstream in("/proc/bus/input/devices");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("N: Name=", 0) == 0 && ui::lower(line).find("touchpad") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// Mouse & Touchpad ------------------------------------------------------------------------------------------

namespace shared {
bool hasTouchpad() { return ::hasTouchpad(); }
std::string ms(float v) { return ::ms(v); }
}  // namespace shared

Element mousePanel(Context& ctx) {
    if (desk::kde()) return kde::mousePanel(ctx);
    std::vector<Element> sections{
        ui::section("General", {ui::pickerRow("Primary Button", gx::Key(kMouse, "left-handed"), {{"Left", "false"}, {"Right", "true"}},
                                              "Order of the physical buttons on mice and touchpads"),
                                ui::sliderValueRow("Double-Click Delay", gx::Key(kMouse, "double-click"), 100, 1000, 50, ms)}),
        ui::section("Mouse", {ui::sliderRow("Pointer Speed", gx::Key(kMouse, "speed"), -1, 1, 0, "Slow to fast"),
                              accelerationRow("Mouse Acceleration", gx::Key(kMouse, "accel-profile")),
                              ui::toggleRow("Natural Scrolling", gx::Key(kMouse, "natural-scroll"), "Scrolling moves the content, not the view"),
                              ui::toggleRow("Middle Click Emulation", gx::Key(kMouse, "middle-click-emulation"),
                                            "Pressing both buttons together pastes")}),
    };
    if (hasTouchpad()) {
        gx::Key sendEvents(kTouchpad, "send-events");
        sections.push_back(ui::section(
            "Touchpad",
            {ui::pickerRow("Touchpad", sendEvents,
                           {{"On", "enabled"}, {"Off", "disabled"}, {"Off With a Mouse", "disabled-on-external-mouse"}}),
             ui::toggleRow("Disable While Typing", gx::Key(kTouchpad, "disable-while-typing")),
             ui::sliderRow("Pointer Speed", gx::Key(kTouchpad, "speed"), -1, 1, 0, "Slow to fast"),
             accelerationRow("Touchpad Acceleration", gx::Key(kTouchpad, "accel-profile"))}));
        sections.push_back(ui::section(
            "Clicking",
            {ui::pickerRow("Secondary Click", gx::Key(kTouchpad, "click-method"),
                           {{"Two Finger Push", "fingers"}, {"Corner Push", "areas"}, {"Hardware Default", "default"}, {"Off", "none"}}),
             ui::toggleRow("Tap to Click", gx::Key(kTouchpad, "tap-to-click"), "Quickly touch the touchpad to click"),
             ui::toggleRow("Tap and Drag", gx::Key(kTouchpad, "tap-and-drag"), "Double-tap and hold to drag"),
             ui::toggleRow("Drag Lock", gx::Key(kTouchpad, "tap-and-drag-lock"), "Lifting the finger briefly doesn't end a drag"),
             ui::pickerRow("Tap Buttons", gx::Key(kTouchpad, "tap-button-map"),
                           {{"Two Fingers Right, Three Middle", "lrm"}, {"Two Fingers Middle, Three Right", "lmr"}, {"Default", "default"}})}));
        sections.push_back(ui::section(
            "Scrolling",
            {ui::toggleRow("Two-Finger Scrolling", gx::Key(kTouchpad, "two-finger-scrolling-enabled")),
             ui::toggleRow("Edge Scrolling", gx::Key(kTouchpad, "edge-scrolling-enabled"), "Scroll along the right edge"),
             ui::toggleRow("Natural Scrolling", gx::Key(kTouchpad, "natural-scroll"), "Scrolling moves the content, not the view")}));
    }
    return ui::page(sections);
}

// Keyboard -----------------------------------------------------------------------------------------------------

namespace {

struct Layouts {
    std::map<std::string, std::string> names;  ///< "us", "tr+f" -> description
};

/// Layout and variant descriptions from XKB's rules (evdev.lst).
const Layouts& xkbLayouts() {
    static Layouts layouts = [] {
        Layouts l;
        for (const char* path : {"/usr/share/X11/xkb/rules/evdev.lst", "/usr/share/X11/xkb/rules/base.lst"}) {
            std::ifstream in(path);
            if (!in) continue;
            std::string line;
            std::string section;
            while (std::getline(in, line)) {
                if (line.rfind("! ", 0) == 0) {
                    section = line.substr(2);
                    continue;
                }
                const std::string t = ui::trim(line);
                if (t.empty()) continue;
                const size_t sp = t.find_first_of(" \t");
                if (sp == std::string::npos) continue;
                const std::string name = t.substr(0, sp);
                std::string desc = ui::trim(t.substr(sp));
                if (section == "layout") {
                    l.names[name] = desc;
                } else if (section == "variant") {
                    // "  f               tr: Turkish (F)"
                    const size_t colon = desc.find(':');
                    if (colon == std::string::npos) continue;
                    l.names[desc.substr(0, colon) + "+" + name] = ui::trim(desc.substr(colon + 1));
                }
            }
            break;
        }
        return l;
    }();
    return layouts;
}

std::string sourceName(const std::string& type, const std::string& id) {
    if (type == "xkb") {
        auto it = xkbLayouts().names.find(id);
        return it != xkbLayouts().names.end() ? it->second : id;
    }
    return id + " (IBus)";
}

using Sources = std::vector<std::pair<std::string, std::string>>;

Sources readSources(const gx::Key& key) {
    Sources out;
    auto v = key.value();
    if (!v || !g_variant_is_of_type(v.get(), G_VARIANT_TYPE("a(ss)"))) return out;
    GVariantIter it;
    g_variant_iter_init(&it, v.get());
    const char* type = nullptr;
    const char* id = nullptr;
    while (g_variant_iter_next(&it, "(&s&s)", &type, &id)) out.emplace_back(type, id);
    return out;
}

void writeSources(const gx::Key& key, const Sources& sources) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ss)"));
    for (const auto& [type, id] : sources) g_variant_builder_add(&b, "(ss)", type.c_str(), id.c_str());
    key.setValue(g_variant_builder_end(&b));
}

/// A choice among XKB options that share a prefix ("compose:", "caps:"); writes xkb-options.
FormRow xkbOptionRow(const std::string& title, const std::vector<std::string>& prefixes, std::vector<ui::Option> options,
                     const std::string& subtitle = {}) {
    gx::Key key(kInputSources, "xkb-options");
    std::vector<std::string> labels;
    for (const auto& o : options) labels.push_back(o.label);
    auto picker = Picker(labels, 0);
    auto belongs = [prefixes](const std::string& opt) {
        return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return opt.rfind(p, 0) == 0; });
    };
    auto sync = [key, picker, options, belongs]() mutable {
        int sel = 0;
        for (const auto& opt : key.getStrv()) {
            if (!belongs(opt)) continue;
            for (size_t i = 0; i < options.size(); ++i) {
                if (options[i].value == opt) sel = static_cast<int>(i);
            }
        }
        picker->setSelected(sel);
    };
    sync();
    picker.onChange([key, options, belongs](int i) {
        auto v = key.getStrv();
        v.erase(std::remove_if(v.begin(), v.end(), belongs), v.end());
        if (!options[static_cast<size_t>(i)].value.empty()) v.push_back(options[static_cast<size_t>(i)].value);
        key.setStrv(v);
    });
    key.watch(sync);
    if (!key) picker.disabled();
    return FormRow(title, picker, subtitle);
}

// Shortcuts ------------------------------------------------------------------------------------------------

constexpr const char* kWmKeys = "org.gnome.desktop.wm.keybindings";
constexpr const char* kShellKeys = "org.gnome.shell.keybindings";
constexpr const char* kMediaKeys = "org.gnome.settings-daemon.plugins.media-keys";
constexpr const char* kMutterKeys = "org.gnome.mutter.keybindings";

struct ShortcutDef {
    const char* group;
    const char* schema;
    const char* key;
    const char* title;
};

const ShortcutDef kShortcuts[] = {
    {"Navigation", kShellKeys, "toggle-overview", "Show the Overview"},
    {"Navigation", kShellKeys, "toggle-application-view", "Show All Apps"},
    {"Navigation", kWmKeys, "switch-applications", "Switch Applications"},
    {"Navigation", kWmKeys, "switch-windows", "Switch Windows"},
    {"Navigation", kWmKeys, "switch-group", "Switch Windows of an Application"},
    {"Navigation", kWmKeys, "switch-to-workspace-left", "Switch to Workspace on the Left"},
    {"Navigation", kWmKeys, "switch-to-workspace-right", "Switch to Workspace on the Right"},
    {"Navigation", kWmKeys, "move-to-workspace-left", "Move Window One Workspace to the Left"},
    {"Navigation", kWmKeys, "move-to-workspace-right", "Move Window One Workspace to the Right"},
    {"Navigation", kWmKeys, "switch-input-source", "Switch to Next Input Source"},
    {"Windows", kWmKeys, "close", "Close Window"},
    {"Windows", kWmKeys, "minimize", "Hide Window"},
    {"Windows", kWmKeys, "toggle-maximized", "Toggle Maximization"},
    {"Windows", kWmKeys, "toggle-fullscreen", "Toggle Fullscreen"},
    {"Windows", kMutterKeys, "toggle-tiled-left", "View Split on Left"},
    {"Windows", kMutterKeys, "toggle-tiled-right", "View Split on Right"},
    {"Windows", kWmKeys, "begin-move", "Move Window"},
    {"Windows", kWmKeys, "begin-resize", "Resize Window"},
    {"System", kMediaKeys, "screensaver", "Lock Screen"},
    {"System", kMediaKeys, "logout", "Log Out"},
    {"System", kShellKeys, "toggle-message-tray", "Show the Notification List"},
    {"System", kShellKeys, "toggle-quick-settings", "Open the Quick Settings Menu"},
    {"System", kMediaKeys, "control-center", "Settings"},
    {"Screenshots", kShellKeys, "show-screenshot-ui", "Take a Screenshot Interactively"},
    {"Screenshots", kShellKeys, "screenshot", "Take a Screenshot"},
    {"Screenshots", kShellKeys, "screenshot-window", "Take a Screenshot of a Window"},
    {"Screenshots", kShellKeys, "show-screen-recording-ui", "Record a Screencast Interactively"},
    {"Launchers", kMediaKeys, "home", "Home Folder"},
    {"Launchers", kMediaKeys, "www", "Launch Web Browser"},
    {"Launchers", kMediaKeys, "terminal", "Launch Terminal"},
    {"Launchers", kMediaKeys, "search", "Search"},
    {"Sound and Media", kMediaKeys, "volume-up", "Volume Up"},
    {"Sound and Media", kMediaKeys, "volume-down", "Volume Down"},
    {"Sound and Media", kMediaKeys, "volume-mute", "Volume Mute"},
    {"Sound and Media", kMediaKeys, "play", "Play (or Play/Pause)"},
    {"Sound and Media", kMediaKeys, "next", "Next Track"},
    {"Sound and Media", kMediaKeys, "previous", "Previous Track"},
};

std::vector<std::string> accelerators(const gx::Key& key) {
    if (key.type() == "s") {
        auto s = key.getString();
        return s.empty() ? std::vector<std::string>{} : std::vector<std::string>{s};
    }
    return key.getStrv();
}

void setAccelerator(const gx::Key& key, const std::string& accel) {
    if (key.type() == "s") key.setString(accel);
    else key.setStrv(accel.empty() ? std::vector<std::string>{} : std::vector<std::string>{accel});
}

/// "<Super><Shift>Left" -> "Super+Shift+←".
std::string prettyAccel(const std::string& accel) {
    if (accel.empty()) return "Disabled";
    std::string out;
    size_t i = 0;
    while (i < accel.size() && accel[i] == '<') {
        const size_t e = accel.find('>', i);
        if (e == std::string::npos) break;
        std::string mod = accel.substr(i + 1, e - i - 1);
        if (mod == "Primary" || mod == "Control" || mod == "Ctrl") mod = "Ctrl";
        out += mod + "+";
        i = e + 1;
    }
    std::string key = accel.substr(i);
    static const std::map<std::string, std::string> kNames = {
        {"Left", "←"}, {"Right", "→"}, {"Up", "↑"}, {"Down", "↓"}, {"Page_Up", "Page Up"}, {"Page_Down", "Page Down"},
        {"Print", "Print Screen"}, {"Return", "Enter"}, {"space", "Space"}, {"BackSpace", "Backspace"},
        {"XF86AudioRaiseVolume", "Volume Up"}, {"XF86AudioLowerVolume", "Volume Down"}, {"XF86AudioMute", "Mute"},
        {"XF86AudioPlay", "Play"}, {"XF86AudioNext", "Next"}, {"XF86AudioPrev", "Previous"}, {"XF86Explorer", "Explorer"},
        {"XF86WWW", "WWW"}, {"XF86Search", "Search"}, {"XF86Tools", "Tools"}, {"XF86ScreenSaver", "Screensaver"},
        {"comma", ","}, {"period", "."}, {"minus", "-"}, {"equal", "="}, {"slash", "/"}, {"Above_Tab", "`"},
    };
    if (auto it = kNames.find(key); it != kNames.end()) key = it->second;
    else if (key.size() == 1) key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    return out + key;
}

/// GTK accelerator name for a key the toolkit reports, empty when it can't be bound.
std::string keyName(Key k) {
    if (k >= Key::A && k <= Key::Z) return std::string(1, static_cast<char>('a' + (static_cast<int>(k) - static_cast<int>(Key::A))));
    if (k >= Key::Num0 && k <= Key::Num9) return std::string(1, static_cast<char>('0' + (static_cast<int>(k) - static_cast<int>(Key::Num0))));
    if (k >= Key::F1 && k <= Key::F12) return "F" + std::to_string(static_cast<int>(k) - static_cast<int>(Key::F1) + 1);
    switch (k) {
        case Key::Left: return "Left";
        case Key::Right: return "Right";
        case Key::Up: return "Up";
        case Key::Down: return "Down";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "Page_Up";
        case Key::PageDown: return "Page_Down";
        case Key::Delete: return "Delete";
        case Key::Insert: return "Insert";
        case Key::Tab: return "Tab";
        case Key::Space: return "space";
        case Key::Enter: return "Return";
        case Key::Comma: return "comma";
        case Key::Period: return "period";
        case Key::Minus: return "minus";
        case Key::Equals: return "equal";
        case Key::Slash: return "slash";
        default: return {};
    }
}

/// Focusable box that turns the next key press into an accelerator. Escape cancels, Backspace disables.
class KeyCaptureView : public Widget {
public:
    std::function<void(std::optional<std::string>)> onDone;  ///< nullopt = cancelled, "" = disabled

    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool focusOnClick() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    Size measureContent(Size /*available*/) override { return {300, 54}; }
    void draw(Painter& p) override {
        const auto& c = theme().colors;
        p.fillRoundedRect(frame(), 10, c.controlBackground, isFocused() ? c.accent : c.separator, isFocused() ? 2.0f : 1.0f);
        p.drawText(mayo::uiText(m_hint), frame(), theme().font(TextStyle::Callout), c.secondaryLabel, TextAlign::Center);
    }
    bool onPointerDown(const PointerEvent& /*e*/) override { return true; }
    bool onKeyDown(const KeyEvent& e) override {
        if (e.key == Key::Escape && e.modifiers == Modifiers::None) {
            if (onDone) onDone(std::nullopt);
            return true;
        }
        if (e.key == Key::Backspace && e.modifiers == Modifiers::None) {
            if (onDone) onDone(std::string());
            return true;
        }
        const std::string name = keyName(e.key);
        if (name.empty()) return true;
        const bool fkey = e.key >= Key::F1 && e.key <= Key::F12;
        if (e.modifiers == Modifiers::None && !fkey) {
            m_hint = "Use a modifier: Super, Ctrl or Alt";
            invalidate();
            return true;
        }
        std::string accel;
        if (hasModifier(e.modifiers, Modifiers::Super)) accel += "<Super>";
        if (hasModifier(e.modifiers, Modifiers::Ctrl)) accel += "<Control>";
        if (hasModifier(e.modifiers, Modifiers::Alt)) accel += "<Alt>";
        if (hasModifier(e.modifiers, Modifiers::Shift)) accel += "<Shift>";
        if (onDone) onDone(accel + name);
        return true;
    }

private:
    std::string m_hint = "Press a shortcut · Esc cancels · Backspace disables";
};

/// Popover that records a shortcut for `apply`.
void recordShortcut(const Element& anchor, const std::string& title, std::function<void(const std::string&)> apply) {
    auto capture = std::make_shared<KeyCaptureView>();
    auto pop = Popover(VStack({Label("Set Shortcut").font(TextStyle::Headline), Label(title).secondary(), Element(capture)}).spacing(8));
    std::weak_ptr<PopoverView> weak = pop.node();
    capture->onDone = [weak, apply](std::optional<std::string> accel) {
        if (auto p = weak.lock()) p->dismiss();
        if (accel) apply(*accel);
    };
    pop.show(anchor);
    if (auto* h = capture->host()) h->setFocus(capture.get(), true);
}

Element shortcutRow(const ShortcutDef& def) {
    gx::Key key(def.schema, def.key);
    if (!key) return Element();
    auto button = Button(prettyAccel(accelerators(key).empty() ? "" : accelerators(key)[0])).controlSize(ControlSize::Small);
    auto reset = Button(Icon::Repeat).style(ButtonStyle::Borderless).tooltip("Reset to Default").controlSize(ControlSize::Small);
    auto sync = [key, button, reset]() mutable {
        const auto a = accelerators(key);
        button.setTitle(prettyAccel(a.empty() ? "" : a[0]));
        reset.setHidden(key.isDefault());
    };
    sync();
    key.watch(sync);
    button.onClick([key, anchor = ui::Anchor(button), title = std::string(def.title)] {
        recordShortcut(anchor, title, [key](const std::string& accel) { setAccelerator(key, accel); });
    });
    reset.onClick([key] { key.reset(); });
    return FormRow(def.title, HStack({reset, button}).spacing(4).align(Alignment::Center));
}

// Custom shortcuts live in relocatable schemas listed by path.
constexpr const char* kCustomSchema = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding";
constexpr const char* kCustomBase = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/";

class CustomShortcuts {
public:
    CustomShortcuts() {
        m_list.watch([this] { rebuild(); });
        auto add = Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("Add Shortcut…");
        add.onClick([this, anchor = ui::Anchor(add)] { edit(anchor, {}); });
        m_section.setAccessory(add);
        rebuild();
    }
    [[nodiscard]] Element element() const { return m_section.element(); }

private:
    void rebuild() {
        std::vector<Element> rows;
        for (const auto& path : m_list.getStrv()) {
            gx::Key name(kCustomSchema, "name", path);
            gx::Key command(kCustomSchema, "command", path);
            gx::Key binding(kCustomSchema, "binding", path);
            if (!name) continue;
            auto editButton = Button(Icon::Pencil).style(ButtonStyle::Borderless).tooltip("Edit");
            editButton.onClick([this, anchor = ui::Anchor(editButton), path] { edit(anchor, path); });
            auto remove = Button(Icon::Trash).style(ButtonStyle::Borderless).tooltip("Remove");
            remove.onClick([this, path] {
                auto v = m_list.getStrv();
                v.erase(std::remove(v.begin(), v.end(), path), v.end());
                m_list.setStrv(v);
                for (const char* k : {"name", "command", "binding"}) gx::Key(kCustomSchema, k, path).reset();
            });
            rows.push_back(FormRow(HStack({VStack({Label(name.getString()), Label(command.getString()).font(TextStyle::Subheadline).secondary().lineLimit(1)})
                                               .spacing(1),
                                           Spacer(), Label(prettyAccel(binding.getString())).secondary(), editButton, remove})
                                       .spacing(8)
                                       .align(Alignment::Center)));
        }
        m_section.setRows(rows);
    }

    void edit(const Element& anchor, const std::string& existing) {
        std::string path = existing;
        if (path.empty()) {
            auto taken = m_list.getStrv();
            for (int i = 0;; ++i) {
                path = std::string(kCustomBase) + "custom" + std::to_string(i) + "/";
                if (std::find(taken.begin(), taken.end(), path) == taken.end()) break;
            }
        }
        gx::Key name(kCustomSchema, "name", path);
        gx::Key command(kCustomSchema, "command", path);
        gx::Key binding(kCustomSchema, "binding", path);
        auto nameField = TextField("Name").text(existing.empty() ? "" : name.getString()).width(280);
        auto commandField = TextField("Command").text(existing.empty() ? "" : command.getString()).width(280);
        auto accel = std::make_shared<std::string>(existing.empty() ? "" : binding.getString());
        auto shortcut = Button(prettyAccel(*accel));
        shortcut.onClick([accel, weakButton = std::weak_ptr<ButtonView>(shortcut.node())] {
            auto b = weakButton.lock();
            if (!b) return;
            recordShortcut(Element(b), "Custom shortcut", [accel, weakButton](const std::string& a) {
                *accel = a;
                if (auto bb = weakButton.lock()) bb->setTitle(prettyAccel(a));
            });
        });
        auto save = Button(existing.empty() ? "Add" : "Save").style(ButtonStyle::Prominent);
        auto cancel = Button("Cancel");
        auto pop = Popover(VStack({Label(existing.empty() ? "Add Custom Shortcut" : "Edit Shortcut").font(TextStyle::Headline), nameField,
                                   commandField, HStack({Label("Shortcut").secondary(), Spacer(), shortcut}).align(Alignment::Center),
                                   HStack({Spacer(), cancel, save}).spacing(8)})
                               .spacing(8)
                               .width(300));
        std::weak_ptr<PopoverView> weak = pop.node();
        cancel.onClick([weak] {
            if (auto p = weak.lock()) p->dismiss();
        });
        save.onClick([this, weak, path, name, command, binding, nameField, commandField, accel] {
            if (nameField.value().empty() || commandField.value().empty()) return;
            name.setString(nameField.value());
            command.setString(commandField.value());
            binding.setString(*accel);
            auto v = m_list.getStrv();
            if (std::find(v.begin(), v.end(), path) == v.end()) {
                v.push_back(path);
                m_list.setStrv(v);
            }
            if (auto p = weak.lock()) p->dismiss();
            rebuild();
        });
        pop.show(anchor);
    }

    gx::Key m_list{kMediaKeys, "custom-keybindings"};
    ui::DynamicSection m_section{"Custom Shortcuts", "No custom shortcuts.", "Run any command with a key combination."};
};

class InputSources {
public:
    InputSources() {
        m_sources.watch([this] { rebuild(); });
        m_add.onClick([this] { openAdd(); });
        m_section.setAccessory(m_add);
        rebuild();
    }

    void openAdd() {
        std::vector<ui::Option> items;
        for (const auto& [id, name] : xkbLayouts().names) items.push_back({name, id});
        std::sort(items.begin(), items.end(), [](const ui::Option& a, const ui::Option& b) { return a.label < b.label; });
        ui::chooseFromList(ui::Anchor(m_add), items, [this](const std::string& id) {
            auto s = readSources(m_sources);
            if (std::find(s.begin(), s.end(), std::pair<std::string, std::string>{"xkb", id}) == s.end()) s.emplace_back("xkb", id);
            writeSources(m_sources, s);
        }, "Search Languages and Layouts");
    }
    [[nodiscard]] Element element() const { return m_section.element(); }

private:
    void rebuild() {
        const Sources sources = readSources(m_sources);
        std::vector<Element> rows;
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& [type, id] = sources[i];
            auto up = Button(Icon::ChevronUp).style(ButtonStyle::Borderless).tooltip("Move Up").disabled(i == 0);
            up.onClick([this, i] {
                auto s = readSources(m_sources);
                if (i > 0 && i < s.size()) std::swap(s[i], s[i - 1]);
                writeSources(m_sources, s);
            });
            auto remove = Button(Icon::Minus).style(ButtonStyle::Borderless).tooltip("Remove").disabled(sources.size() <= 1);
            remove.onClick([this, i] {
                auto s = readSources(m_sources);
                if (s.size() > 1 && i < s.size()) s.erase(s.begin() + static_cast<long>(i));
                writeSources(m_sources, s);
            });
            std::string code = id.substr(0, id.find('+'));
            for (auto& ch : code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            auto badge = Canvas([code](Painter& p, const Rect& r) {
                p.fillRoundedRect(r, 4, theme().colors.secondaryFill);
                p.drawText(code, r, theme().font(TextStyle::Caption).withWeight(FontWeight::Semibold), theme().colors.secondaryLabel,
                           TextAlign::Center);
            });
            badge.frame(30, 18);
            rows.push_back(FormRow(HStack({badge, Label(sourceName(type, id)), Spacer(), up, remove}).spacing(10).align(Alignment::Center)));
        }
        m_section.setRows(rows);
    }

    gx::Key m_sources{kInputSources, "sources"};
    Button m_add = Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("Add Input Source…");
    ui::DynamicSection m_section{"Input Sources", "No input sources.", "The first source is the default. Switch with Super+Space."};
};

}  // namespace

namespace shared {
const std::map<std::string, std::string>& xkbLayoutNames() { return xkbLayouts().names; }
}  // namespace shared

namespace {

struct KeyboardModel {
    InputSources sources;
    CustomShortcuts custom;
};

}  // namespace

Element keyboardPanel(Context& ctx) {
    if (desk::kde()) return kde::keyboardPanel(ctx);
    auto model = std::make_shared<KeyboardModel>();
    if (ctx.demo == "add") ctx.app.after(0.6, [m = model.get()] { m->sources.openAdd(); });
    if (ctx.demo == "shortcut")
        ctx.app.after(0.6, [anchor = ctx.anchor] { recordShortcut(anchor, "Show the Overview", [](const std::string&) {}); });
    std::vector<Element> sections{
        model->sources.element(),
        ui::section("Input Source Switching",
                    {ui::pickerRow("Switch Sources", gx::Key(kInputSources, "per-window"),
                                   {{"For All Windows", "false"}, {"Per Window", "true"}})}),
        ui::section("Typing",
                    {ui::toggleRow("Key Repeat", gx::Key(kKeyboard, "repeat"), "Holding a key repeats its character"),
                     ui::sliderValueRow("Delay Until Repeat", gx::Key(kKeyboard, "delay"), 100, 2000, 50, ms),
                     ui::sliderValueRow("Repeat Interval", gx::Key(kKeyboard, "repeat-interval"), 10, 110, 1, ms, "Shorter is faster"),
                     ui::toggleRow("Remember Num Lock", gx::Key(kKeyboard, "remember-numlock-state"))}),
        ui::section("Special Keys",
                    {xkbOptionRow("Compose Key", {"compose:"},
                                  {{"Off", ""}, {"Right Alt", "compose:ralt"}, {"Left Super", "compose:lwin"}, {"Right Super", "compose:rwin"},
                                   {"Menu", "compose:menu"}, {"Right Ctrl", "compose:rctrl"}, {"Caps Lock", "compose:caps"},
                                   {"Scroll Lock", "compose:sclk"}},
                                  "Type special characters as sequences, e.g. Compose, ', e → é"),
                     xkbOptionRow("Alternate Characters Key", {"lv3:"},
                                  {{"Layout Default", ""}, {"Right Alt", "lv3:ralt_switch"}, {"Left Alt", "lv3:lalt_switch"},
                                   {"Right Super", "lv3:rwin_switch"}, {"Menu", "lv3:menu_switch"}}),
                     xkbOptionRow("Caps Lock", {"caps:", "ctrl:nocaps", "ctrl:swapcaps"},
                                  {{"Caps Lock", ""}, {"Escape", "caps:escape"}, {"Ctrl", "ctrl:nocaps"}, {"Swap with Ctrl", "ctrl:swapcaps"},
                                   {"Backspace", "caps:backspace"}, {"Disabled", "caps:none"}})}),
        model->custom.element(),
    };
    // Built-in shortcuts, grouped like GNOME Settings.
    std::map<std::string, std::vector<Element>> groups;
    std::vector<std::string> order;
    for (const auto& def : kShortcuts) {
        auto row = shortcutRow(def);
        if (!row) continue;
        if (!groups.contains(def.group)) order.emplace_back(def.group);
        groups[def.group].push_back(row);
    }
    for (const auto& g : order) sections.push_back(ui::section("Shortcuts: " + g, groups[g]));
    return ui::page(sections, model);
}

// Accessibility ---------------------------------------------------------------------------------------------

namespace {

/// "Large Text" is text-scaling-factor 1.25 (GNOME's own definition).
FormRow largeTextRow() {
    gx::Key key(kInterface, "text-scaling-factor");
    auto t = Toggle("", key.getDouble(1) > 1.01).onChange([key](bool on) { key.setDouble(on ? 1.25 : 1.0); });
    key.watch([key, t]() mutable { t.setOn(key.getDouble(1) > 1.01); });
    return FormRow("Large Text", t, "Makes text 25% bigger");
}

}  // namespace

Element accessibilityPanel(Context& ctx) {
    if (desk::kde()) return kde::accessibilityPanel(ctx);
    constexpr const char* kA11y = "org.gnome.desktop.a11y";
    constexpr const char* kA11yIface = "org.gnome.desktop.a11y.interface";
    constexpr const char* kApps = "org.gnome.desktop.a11y.applications";
    constexpr const char* kA11yKeys = "org.gnome.desktop.a11y.keyboard";
    constexpr const char* kA11yMouse = "org.gnome.desktop.a11y.mouse";
    constexpr const char* kMagnifier = "org.gnome.desktop.a11y.magnifier";
    auto percent = [](float v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f×", static_cast<double>(v));
        return std::string(buf);
    };
    auto seconds = [](float v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(v));
        return std::string(buf);
    };
    return ui::page({
        ui::section({}, {ui::toggleRow("Accessibility Menu", gx::Key(kA11y, "always-show-universal-access-status"),
                                       "Always show the accessibility menu in the top bar")}),
        ui::section("Seeing",
                    {ui::toggleRow("High Contrast", gx::Key(kA11yIface, "high-contrast"), "Stronger borders and text"),
                     ui::toggleRow("Reduce Animation", gx::Key(kInterface, "enable-animations"), {}, true),
                     largeTextRow(),
                     ui::pickerRow("Cursor Size", gx::Key(kInterface, "cursor-size"),
                                   {{"Default", "24"}, {"Medium", "32"}, {"Large", "48"}, {"Larger", "64"}, {"Largest", "96"}}),
                     ui::toggleRow("Always Show Scrollbars", gx::Key(kInterface, "overlay-scrolling"), {}, true),
                     ui::toggleRow("Status Shapes", gx::Key(kA11yIface, "show-status-shapes"), "Shapes in addition to colours for status indicators"),
                     ui::toggleRow("Locate Pointer", gx::Key(kInterface, "locate-pointer"), "Press Ctrl to highlight the pointer"),
                     ui::toggleRow("Screen Reader", gx::Key(kApps, "screen-reader-enabled"), "Reads the screen aloud (Orca)")}),
        ui::section("Zoom",
                    {ui::toggleRow("Desktop Zoom", gx::Key(kApps, "screen-magnifier-enabled")),
                     ui::sliderValueRow("Magnification", gx::Key(kMagnifier, "mag-factor"), 1, 20, 0.25f, percent),
                     ui::pickerRow("Magnifier Position", gx::Key(kMagnifier, "screen-position"),
                                   {{"Full Screen", "full-screen"}, {"Top Half", "top-half"}, {"Bottom Half", "bottom-half"},
                                    {"Left Half", "left-half"}, {"Right Half", "right-half"}}),
                     ui::toggleRow("Invert Colors", gx::Key(kMagnifier, "invert-lightness"))}),
        ui::section("Hearing",
                    {ui::toggleRow("Overamplification", gx::Key("org.gnome.desktop.sound", "allow-volume-above-100-percent")),
                     ui::toggleRow("Visual Alerts", gx::Key("org.gnome.desktop.wm.preferences", "visual-bell"), "Flash the screen for alert sounds"),
                     ui::pickerRow("Flash Area", gx::Key("org.gnome.desktop.wm.preferences", "visual-bell-type"),
                                   {{"Window Title", "frame-flash"}, {"Entire Screen", "fullscreen-flash"}})}),
        ui::section("Typing",
                    {ui::toggleRow("Screen Keyboard", gx::Key(kApps, "screen-keyboard-enabled")),
                     ui::toggleRow("Sticky Keys", gx::Key(kA11yKeys, "stickykeys-enable"), "Press modifier keys one after another"),
                     ui::toggleRow("Slow Keys", gx::Key(kA11yKeys, "slowkeys-enable"), "Keys must be held before they register"),
                     ui::toggleRow("Bounce Keys", gx::Key(kA11yKeys, "bouncekeys-enable"), "Ignore fast repeated presses of the same key"),
                     ui::toggleRow("Enable by Keyboard", gx::Key(kA11yKeys, "enable"), "Turn these features on and off with the keyboard"),
                     ui::toggleRow("Cursor Blinking", gx::Key(kInterface, "cursor-blink"), "In text fields")}),
        ui::section("Pointing & Clicking",
                    {ui::toggleRow("Mouse Keys", gx::Key(kA11yKeys, "mousekeys-enable"), "Control the pointer with the keypad"),
                     ui::toggleRow("Simulated Secondary Click", gx::Key(kA11yMouse, "secondary-click-enabled"),
                                   "Hold the primary button to right-click"),
                     ui::sliderValueRow("Secondary Click Delay", gx::Key(kA11yMouse, "secondary-click-time"), 0.5f, 3, 0.1f, seconds),
                     ui::toggleRow("Hover Click", gx::Key(kA11yMouse, "dwell-click-enabled"), "Click by resting the pointer"),
                     ui::sliderValueRow("Hover Delay", gx::Key(kA11yMouse, "dwell-time"), 0.2f, 3, 0.1f, seconds),
                     ui::sliderValueRow("Double-Click Delay", gx::Key(kMouse, "double-click"), 100, 1000, 50, ms)}),
    });
}
