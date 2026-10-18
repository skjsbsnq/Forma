// Window management panels that KDE's System Settings splits into separate modules but that belong together
// here: Window Decorations (kwinrc [org.kde.kdecoration2]), Desktop Effects and KWin Scripts (KWin's own
// plugin packages), and Autostart (the freedesktop autostart directories, which Plasma and GNOME share).
//
// Everything is read from the same files KDE reads, so the two settings apps stay in sync: kwinrc entries are
// watched, and the effect/script lists are rebuilt when a package is installed or removed.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "glib_support.hpp"
#include "json.hpp"
#include "kconfig.hpp"
#include "panels.hpp"
#include "panels_shared.hpp"
#include "ui.hpp"

using namespace mayo;
namespace fs = std::filesystem;

namespace {

constexpr const char* kKWinRc = "kwinrc";

std::string readAll(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Packages -------------------------------------------------------------------------------------------------

/// One KWin plugin package: an effect, a script or a decoration.
struct Package {
    std::string id;
    std::string name;
    std::string description;
    std::string category;
    std::string configModule;  ///< X-KDE-ConfigModule: a KCM that configures this package, if any
    bool enabledByDefault = false;
};

/// Localized metadata value: KDE packages ship "Name[zh_CN]" beside "Name". The JSON parser keeps bracketed
/// keys as they are, so the language list from GLib picks the best match.
std::string localized(const json::Value& object, const std::string& key) {
    for (const char* const* lang = g_get_language_names(); *lang; ++lang) {
        const std::string withLang = key + "[" + *lang + "]";
        if (const std::string v = object[withLang].str(); !v.empty()) return v;
    }
    return object[key].str();
}

/// Reads metadata.json (a package) or <id>.json (a built-in effect file).
Package readPackage(const fs::path& file) {
    Package p;
    const std::string text = readAll(file);
    if (text.empty()) return p;
    const json::Value doc = json::Value::parse(text);
    const json::Value& kp = doc["KPlugin"];
    p.id = kp["Id"].str();
    p.name = localized(kp, "Name");
    p.description = localized(kp, "Description");
    p.category = kp["Category"].str();
    p.configModule = doc["X-KDE-ConfigModule"].str();
    const json::Value& byDefault = kp["EnabledByDefault"];
    p.enabledByDefault = byDefault.type() == json::Value::Type::Bool ? byDefault.boolean() : false;
    return p;
}

/// Every package of one kind, from $XDG_DATA_HOME and $XDG_DATA_DIRS (the first one wins per id).
std::vector<Package> packages(const std::string& sub, const char* fileInDir) {
    std::vector<Package> out;
    std::set<std::string> seen;
    for (const auto& dir : shared::dataDirs()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(dir) / sub, ec)) {
            std::error_code ec2;
            Package p;
            if (fileInDir) {
                const fs::path meta = e.path() / fileInDir;
                if (!fs::exists(meta, ec2)) continue;
                p = readPackage(meta);
            } else {
                if (!e.path().string().ends_with(".json")) continue;
                p = readPackage(e.path());
            }
            if (p.id.empty()) p.id = e.path().stem().string();
            if (p.name.empty()) p.name = p.id;
            if (!seen.insert(p.id).second) continue;
            out.push_back(std::move(p));
        }
    }
    return out;
}

/// Package ids that exist as built-in effects (`builtin-effects/<id>.json`) and as packages
/// (`effects/<id>/metadata.json`) can overlap; callers merge both lists.
std::vector<Package> kwinEffects() {
    auto list = packages("kwin/effects", "metadata.json");
    for (auto& p : packages("kwin/builtin-effects", nullptr)) {
        if (std::none_of(list.begin(), list.end(), [&](const Package& q) { return q.id == p.id; }))
            list.push_back(std::move(p));
    }
    std::sort(list.begin(), list.end(), [](const Package& a, const Package& b) {
        if (a.category != b.category) return a.category < b.category;
        return ui::lower(a.name) < ui::lower(b.name);
    });
    return list;
}

std::vector<Package> kwinScripts() {
    auto list = packages("kwin/scripts", "metadata.json");
    std::sort(list.begin(), list.end(),
              [](const Package& a, const Package& b) { return ui::lower(a.name) < ui::lower(b.name); });
    return list;
}

// Window decorations ---------------------------------------------------------------------------------------

/// The letters KWin accepts in ButtonsOnLeft / ButtonsOnRight, verified against `KWin supportInformation`
/// (decorationButtonsRight: 6, 3, 4, 5 for the default "HIAX"). Unknown letters are dropped by KWin.
const std::pair<char, const char*> kButtonLetters[] = {
    {'M', "Window Menu"},  {'N', "On All Desktops"},
    {'S', "Shade"},        {'I', "Minimize"},
    {'A', "Maximize"},     {'X', "Close"},
    {'H', "Context Help"}, {'B', "Keep Below"},
    {'F', "Keep Above"},   {'E', "Exclude From Capture"},
    {'_', "Spacer"},
};

constexpr const char* kButtonDefault = "HIAX";

std::vector<Package> decorationPackages() {
    std::vector<Package> out;
    std::set<std::string> seen;
    // Decorations live as bare plugins (org.kde.breeze.so) rather than packages; the file name is the id KWin
    // stores in library=.
    for (const char* dir : {"/usr/lib/qt6/plugins/org.kde.kdecoration3", "/usr/lib64/qt6/plugins/org.kde.kdecoration3",
                            "/usr/lib/x86_64-linux-gnu/qt6/plugins/org.kde.kdecoration3"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".so") continue;
            Package p;
            p.id = e.path().stem().string();
            // Aurorae themes are chosen through their own plugin, so only the plain decoration plugins count.
            if (!seen.insert(p.id).second) continue;
            p.name = p.id;
            // Plugin metadata is embedded in the .so; the id doubles as the human name for the built-in ones.
            static const std::map<std::string, std::string> kNames{
                {"org.kde.breeze", "Breeze"}, {"org.kde.oxygen", "Oxygen"}, {"kos_decoration", "KOS"}};
            if (auto it = kNames.find(p.id); it != kNames.end()) p.name = it->second;
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Package& a, const Package& b) { return ui::lower(a.name) < ui::lower(b.name); });
    return out;
}

/// Aurorae (SVG) themes: /usr/share/aurorae/themes/<name> and ~/.local/share/aurorae/themes/<name>.
std::vector<Package> auroraeThemes() {
    std::vector<Package> out;
    std::set<std::string> seen;
    for (const auto& dir : shared::dataDirs()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(dir) / "aurorae" / "themes", ec)) {
            const std::string name = e.path().filename().string();
            if (!seen.insert(name).second) continue;
            out.push_back({name, name, {}, "Aurorae", {}, false});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Package& a, const Package& b) { return ui::lower(a.name) < ui::lower(b.name); });
    return out;
}

// Effects and scripts --------------------------------------------------------------------------------------

/// The `[Plugins]` group holds "<id>Enabled" for every effect and script; a missing entry means "default".
Element pluginRow(const Package& p, const gx::Key& key) {
    auto toggle = Toggle("", key.getBool(p.enabledByDefault)).onChange([key](bool on) { key.set(on); });
    key.watch([key, toggle, fallback = p.enabledByDefault]() mutable { toggle.setOn(key.getBool(fallback)); });
    // The name and description come from installed metadata, already localized by KDE: build the row by hand
    // so they bypass the UI catalogue (a name that happens to match a UI string must not be replaced).
    auto text = VStack({Label(p.name).verbatim().lineLimit(1),
                        Label(p.description).verbatim().font(TextStyle::Subheadline).secondary().lineLimit(2)})
                    .spacing(1);
    auto row = FormRow(Element(HStack({text, Spacer(), toggle}).spacing(12).align(Alignment::Center)));
    if (!key) toggle.disabled();
    return row;
}

}  // namespace

namespace kde {

namespace {

/// A checkbox-free ordered editor for a ButtonsOnLeft / ButtonsOnRight string, like the keyboard layouts
/// editor: one row per button with up / remove, plus an accessory "Add" button. KWin draws the buttons in
/// the order of the letters, so reordering is part of the setting.
class ButtonOrder {
public:
    ButtonOrder(std::string title, const gx::Key& key, const std::string& fallback) {
        m_section = ui::DynamicSection(std::move(title), "No titlebar buttons on this side.");
        m_section.setAccessory(m_add);
        m_add.onClick([this] { openAdd(); });
        m_key = key;
        m_fallback = fallback;
        key.watch([this] { rebuild(); });
        rebuild();
    }

    [[nodiscard]] Element element() const { return m_section.element(); }

    /// Opens the "add button" chooser (used by the button and by demo runs).
    void openAdd() {
        std::vector<ui::Option> items;
        for (const auto& [letter, label] : kButtonLetters) {
            if (letter == '_') continue;
            if (letters().find(letter) != std::string::npos) continue;
            items.push_back({label, std::string(1, letter)});
        }
        if (items.empty()) return;
        ui::chooseFromList(
            ui::Anchor(m_add), items,
            [this](const std::string& letter) {
                if (letter.empty()) return;
                write(letters() + letter);
            },
            "Search Buttons");
    }

private:
    [[nodiscard]] std::string letters() const {
        const std::string v = m_key.getString();
        return v.empty() ? m_fallback : v;
    }

    void write(const std::string& value) { m_key.setString(value); }

    void rebuild() {
        const std::string current = letters();
        std::vector<Element> rows;
        for (size_t i = 0; i < current.size(); ++i) {
            const char c = current[i];
            std::string label = "Unknown button";
            for (const auto& [letter, name] : kButtonLetters) {
                if (letter == c) label = name;
            }
            auto up = Button(Icon::ChevronUp).style(ButtonStyle::Borderless).tooltip("Move Left").disabled(i == 0);
            up.onClick([this, i] {
                auto s = letters();
                if (i > 0 && i < s.size()) std::swap(s[i], s[i - 1]);
                write(s);
            });
            auto down = Button(Icon::ChevronDown)
                            .style(ButtonStyle::Borderless)
                            .tooltip("Move Right")
                            .disabled(i + 1 >= current.size());
            down.onClick([this, i] {
                auto s = letters();
                if (i + 1 < s.size()) std::swap(s[i], s[i + 1]);
                write(s);
            });
            auto remove = Button(Icon::Minus).style(ButtonStyle::Borderless).tooltip("Remove");
            remove.onClick([this, c] {
                auto s = letters();
                if (auto pos = s.find(c); pos != std::string::npos) s.erase(pos, 1);
                write(s);
            });
            auto badge = Canvas([c](Painter& p, const Rect& r) {
                p.fillRoundedRect(r, 4, theme().colors.secondaryFill);
                p.drawText(std::string(1, c), r, theme().font(TextStyle::Caption).withWeight(FontWeight::Semibold),
                           theme().colors.secondaryLabel, TextAlign::Center);
            });
            badge.frame(22, 18);
            rows.push_back(FormRow(
                HStack({badge, Label(label), Spacer(), up, down, remove}).spacing(10).align(Alignment::Center)));
        }
        m_section.setRows(rows);
    }

    gx::Key m_key;
    std::string m_fallback;
    Button m_add = Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("Add Button…");
    ui::DynamicSection m_section{"", ""};
};

}  // namespace

Element decorationsPanel(Context& ctx) {
    auto decorations = std::make_shared<std::vector<Package>>(decorationPackages());
    auto themes = std::make_shared<std::vector<Package>>(auroraeThemes());

    gx::Key library = kcfg::key(kKWinRc, "org.kde.kdecoration2", "library", "s", "org.kde.breeze", kcfg::Reload::KWin);
    gx::Key themeName = kcfg::key(kKWinRc, "org.kde.kdecoration2", "theme", "s", "", kcfg::Reload::KWin);
    gx::Key borderSize = kcfg::key(kKWinRc, "org.kde.kdecoration2", "BorderSize", "s", "Normal", kcfg::Reload::KWin);
    gx::Key borderAuto = kcfg::key(kKWinRc, "org.kde.kdecoration2", "BorderSizeAuto", "b", "true", kcfg::Reload::KWin);
    gx::Key showToolTips = kcfg::key(kKWinRc, "org.kde.kdecoration2", "ShowToolTips", "b", "true", kcfg::Reload::KWin);
    gx::Key closeOnDoubleClick =
        kcfg::key(kKWinRc, "org.kde.kdecoration2", "CloseOnDoubleClickOnMenu", "b", "false", kcfg::Reload::KWin);
    gx::Key left = kcfg::key(kKWinRc, "org.kde.kdecoration2", "ButtonsOnLeft", "s", "", kcfg::Reload::KWin);
    gx::Key right =
        kcfg::key(kKWinRc, "org.kde.kdecoration2", "ButtonsOnRight", "s", kButtonDefault, kcfg::Reload::KWin);

    // The decoration picker spans two entries: library= holds the plugin, and Aurorae themes add theme=.
    auto decorationBackend = std::make_shared<gx::KeyBackend>();
    decorationBackend->type = "s";
    decorationBackend->fallback = "org.kde.breeze";
    decorationBackend->read = [library, themeName]() -> std::optional<std::string> {
        const std::string lib = library.getString();
        if (lib == "org.kde.kwin.aurorae" || lib == "org.kde.kwin.aurorae.v2") {
            void* unused = nullptr;
            (void)unused;
            return "aurorae:" + themeName.getString();
        }
        return lib;
    };
    decorationBackend->write = [](const std::optional<std::string>& value) {
        if (!value) return;
        if (value->rfind("aurorae:", 0) == 0) {
            kcfg::write(kKWinRc, "org.kde.kdecoration2", "theme", value->substr(8), kcfg::Reload::KWin);
            kcfg::write(kKWinRc, "org.kde.kdecoration2", "library", "org.kde.kwin.aurorae", kcfg::Reload::KWin);
        } else {
            kcfg::write(kKWinRc, "org.kde.kdecoration2", "library", *value, kcfg::Reload::KWin);
        }
    };
    decorationBackend->watch = [library, themeName](std::function<void()> fn) {
        kcfg::watch(kKWinRc, "org.kde.kdecoration2", "library", fn);
        kcfg::watch(kKWinRc, "org.kde.kdecoration2", "theme", fn);
    };
    gx::Key decoration(decorationBackend, "library");

    std::vector<ui::Option> choices;
    for (const auto& p : *decorations) choices.push_back({p.name, p.id});
    for (const auto& t : *themes) choices.push_back({t.name + "  ·  Aurorae", "aurorae:" + t.id});

    // The page owns the editors: the sections keep only their Element, so without this the objects would die
    // at the end of the function and key.watch() / demo callbacks would point at freed memory.
    struct DecorationsModel {
        std::shared_ptr<ButtonOrder> left;
        std::shared_ptr<ButtonOrder> right;
    };
    auto model = std::make_shared<DecorationsModel>();
    model->left = std::make_shared<ButtonOrder>("Left Buttons", left, "");
    model->right = std::make_shared<ButtonOrder>("Right Buttons", right, kButtonDefault);
    if (ctx.demo == "buttons") ctx.app.after(0.8, [p = model->left.get()] { p->openAdd(); });

    auto borderRow = ui::pickerRow("Border Size", borderSize,
                                   {{"No Borders", "None"},
                                    {"No Side Borders", "NoSides"},
                                    {"Tiny", "Tiny"},
                                    {"Normal", "Normal"},
                                    {"Large", "Large"},
                                    {"Very Large", "VeryLarge"},
                                    {"Huge", "Huge"},
                                    {"Very Huge", "VeryHuge"},
                                    {"Oversized", "Oversized"}},
                                   "Resizable edges around the window");
    // While the theme picks the border size, the explicit one has no effect; disable it instead of hiding it,
    // so the row doesn't jump when the toggle changes.
    auto syncBorder = [borderRow, borderAuto]() mutable { borderRow.node()->setDisabled(borderAuto.getBool(true)); };
    syncBorder();
    borderAuto.watch(syncBorder);

    return ui::page(
        {
            ui::hero(Icon::Windows, theme().colors.indigo, "Window Decorations",
                     "The titlebar layout, border size and decoration plugin KWin draws every window with."),
            ui::section("Decoration",
                        {ui::pickerRow("Decoration", decoration, choices, "Aurorae themes need the Aurorae plugin")}),
            model->left->element(),
            model->right->element(),
            ui::section("Borders", {borderRow,
                                    ui::toggleRow("Follow the Theme's Recommendation", borderAuto,
                                                  "The decoration plugin picks the border size it was designed for"),
                                    ui::toggleRow("Button Tooltips", showToolTips,
                                                  "Show a tooltip when hovering a titlebar button"),
                                    ui::toggleRow("Close on Double-Click of the Window Menu", closeOnDoubleClick,
                                                  "Double-clicking the window menu button closes the window")}),
        },
        model);
}

Element effectsPanel(Context& ctx) {
    auto effects = std::make_shared<std::vector<Package>>(kwinEffects());
    auto scripts = std::make_shared<std::vector<Package>>(kwinScripts());

    auto effectsCard =
        std::make_shared<ui::DynamicSection>("", "", "Effects marked by the system as required are always on.");
    auto scriptCard = std::make_shared<ui::DynamicSection>(
        "KWin Scripts", "No KWin scripts are installed.",
        "Scripts add behaviour to the window manager; they are installed from a file or from store.kde.org.");

    auto rebuild = std::make_shared<std::function<void()>>();
    *rebuild = [effectsCard, scriptCard, effects, scripts] {
        // Group by the metadata's English category id, in the order KDE lists them (alphabetically); the
        // heading goes through the UI catalogue.
        std::vector<Element> rows;
        std::string category;
        std::vector<Element> current;
        for (const auto& p : *effects) {
            if (!category.empty() && p.category != category) {
                rows.push_back(ui::section(category, current));
                current.clear();
            }
            category = p.category.empty() ? "Other" : p.category;
            current.push_back(pluginRow(p, kcfg::key(kKWinRc, "Plugins", p.id + "Enabled", "b",
                                                     p.enabledByDefault ? "true" : "false", kcfg::Reload::KWin)));
        }
        if (!current.empty()) rows.push_back(ui::section(category, current));
        effectsCard->setRows(rows);

        std::vector<Element> scriptRows;
        for (const auto& p : *scripts) {
            scriptRows.push_back(pluginRow(p, kcfg::key(kKWinRc, "Plugins", p.id + "Enabled", "b",
                                                        p.enabledByDefault ? "true" : "false", kcfg::Reload::KWin)));
        }
        scriptCard->setRows(scriptRows);
    };
    // Installing a script copies the package into ~/.local/share/kwin/scripts and KWin picks it up from the
    // Plugins entry written when its toggle is switched on.
    auto install = Button(Icon::Plus, "Install from File…");
    install.onClick([&app = ctx.app, &window = ctx.window, anchor = ctx.anchor, scripts, rebuild] {
        app.openFiles(
            {{"KWin Scripts", "kwinscript;zip"}, {"All Files", "*"}}, false,
            [anchor, scripts, rebuild](std::vector<std::string> paths) {
                if (paths.empty()) return;
                const std::string tool = kcfg::findTool({"kpackagetool6", "kpackagetool5"});
                if (tool.empty()) {
                    ui::message(anchor, "Can't install the script", "kpackagetool6 isn't installed.");
                    return;
                }
                std::vector<std::string> argv{tool, "--type", "KWin/Script", "--install", paths[0]};
                gx::run(argv, [anchor, scripts, rebuild](const gx::ProcessResult& r) {
                    if (!r.ok()) {
                        ui::message(anchor, "Couldn't install the script", ui::trim(r.err));
                        return;
                    }
                    *scripts = kwinScripts();
                    (*rebuild)();
                });
            },
            &window);
    });
    scriptCard->setAccessory(install);
    (*rebuild)();

    return ui::page({
        ui::hero(Icon::Sparkles, theme().colors.purple, "Desktop Effects",
                 "Compositor effects KWin runs while drawing windows. Turning one off frees GPU time."),
        effectsCard->element(),
        scriptCard->element(),
    });
}

}  // namespace kde

// Autostart ------------------------------------------------------------------------------------------------

namespace {

struct AutostartEntry {
    std::string path;  ///< absolute .desktop path
    std::string fileName;
    std::string name;
    std::string command;
    bool enabled = true;
    bool system = false;  ///< from /etc/xdg/autostart (cannot be deleted, only hidden)
};

/// One "Key=value" line of a .desktop file.
std::string desktopValue(const std::string& text, const std::string& key) {
    for (const auto& line : ui::split(text, '\n')) {
        const std::string t = ui::trim(line);
        if (t.rfind(key + "=", 0) != 0) continue;
        return ui::trim(t.substr(key.size() + 1));
    }
    return {};
}

/// "Name[zh_CN]" for the user's language, falling back to "Name". The desktop-entry spec lists the locale
/// keys in the same order as glib's language names.
std::string desktopLocalized(const std::string& text, const std::string& key) {
    for (const char* const* lang = g_get_language_names(); *lang; ++lang) {
        if (const std::string v = desktopValue(text, key + "[" + *lang + "]"); !v.empty()) return v;
    }
    return desktopValue(text, key);
}

void desktopSet(const std::string& path, const std::string& key, const std::optional<std::string>& value) {
    if (gx::readOnly()) return;
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (ui::trim(line).rfind(key + "=", 0) == 0) {
            if (value) lines.push_back(key + "=" + *value);
        } else {
            lines.push_back(line);
        }
    }
    if (value &&
        std::none_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.rfind(key + "=", 0) == 0; }))
        lines.push_back(key + "=" + *value);
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

std::string userAutostartDir() { return std::string(g_get_user_config_dir()) + "/autostart"; }

std::vector<AutostartEntry> autostartEntries() {
    std::vector<AutostartEntry> out;
    std::set<std::string> seen;
    // The user's own entries shadow the system ones with the same file name.
    for (const auto& [dir, system] :
         {std::pair{std::string(userAutostartDir()), false}, std::pair{std::string("/etc/xdg/autostart"), true}}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".desktop") continue;
            const std::string name = e.path().filename().string();
            if (!seen.insert(name).second) continue;
            const std::string text = readAll(e.path());
            if (text.empty()) continue;
            if (desktopValue(text, "Hidden") == "true") continue;
            if (desktopValue(text, "X-KDE-autostart-phase") == "0") continue;  // startup services, not apps
            AutostartEntry entry;
            entry.path = e.path().string();
            entry.fileName = name;
            entry.name = desktopLocalized(text, "Name");
            if (entry.name.empty()) entry.name = name;
            entry.command = desktopValue(text, "Exec");
            // GNOME and Plasma both use X-GNOME-Autostart-enabled for the on/off state (Hidden removes the
            // entry entirely, and was filtered above).
            entry.enabled = desktopValue(text, "X-GNOME-Autostart-enabled") != "false";
            entry.system = system;
            out.push_back(std::move(entry));
        }
    }
    std::sort(out.begin(), out.end(), [](const AutostartEntry& a, const AutostartEntry& b) {
        if (a.system != b.system) return !a.system;
        return ui::lower(a.name) < ui::lower(b.name);
    });
    return out;
}

}  // namespace

Element autostartPanel(Context& ctx) {
    auto card = std::make_shared<ui::DynamicSection>(
        "Login Items", "No applications start automatically.",
        "Applications listed here run when you log in. System entries come from /etc/xdg/autostart.");

    auto rebuild = std::make_shared<std::function<void()>>();
    *rebuild = [card, rebuild, anchor = ctx.anchor] {
        std::vector<Element> rows;
        for (const auto& e : autostartEntries()) {
            auto toggle = Toggle("", e.enabled);
            toggle.onChange([path = e.path, rebuild](bool on) {
                desktopSet(path, "X-GNOME-Autostart-enabled", on ? "true" : "false");
                (*rebuild)();
            });
            auto remove = Button(Icon::Trash).style(ButtonStyle::Borderless).tooltip("Remove");
            remove.onClick([path = e.path, name = e.name, anchor, rebuild] {
                ui::confirm(anchor, "Remove “" + name + "”?", "It no longer starts when you log in.", "Remove",
                            [path, rebuild] {
                                if (gx::readOnly()) return;
                                std::error_code ec;
                                fs::remove(path, ec);
                                (*rebuild)();
                            });
            });
            // System entries are owned by a package: hide them instead of deleting their file.
            remove.setHidden(e.system);
            auto text = VStack({Label(e.name).verbatim().lineLimit(1),
                                Label(e.command).verbatim().font(TextStyle::Subheadline).secondary().lineLimit(1)})
                            .spacing(1);
            rows.push_back(FormRow(
                HStack({ui::letterTile(e.name), text, Spacer(), toggle, remove}).spacing(10).align(Alignment::Center)));
        }
        card->setRows(rows);
    };
    (*rebuild)();

    // Demo hook: flip the first entry, so a run against a scratch XDG_CONFIG_HOME can check the write path.
    if (ctx.demo == "toggle") {
        ctx.app.after(0.5, [entries = autostartEntries()] {
            if (entries.empty()) return;
            desktopSet(entries.front().path, "X-GNOME-Autostart-enabled", entries.front().enabled ? "false" : "true");
        });
    }

    auto add = Button(Icon::Plus, "Add Application…");
    add.onClick([anchor = ctx.anchor, &app = ctx.app, &window = ctx.window, rebuild] {
        app.openFiles(
            {{"Applications", "desktop"}}, false,
            [anchor, rebuild](std::vector<std::string> paths) {
                if (paths.empty() || gx::readOnly()) return;
                std::error_code ec;
                fs::create_directories(userAutostartDir(), ec);
                const fs::path target = fs::path(userAutostartDir()) / fs::path(paths[0]).filename();
                fs::copy_file(paths[0], target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    ui::message(anchor, "Couldn't add the login item", ec.message());
                    return;
                }
                (*rebuild)();
            },
            &window);
    });
    card->setAccessory(add);

    return ui::page({
        ui::hero(Icon::Play, theme().colors.green, "Login Items",
                 "Applications that start when you sign in, from the user and system autostart folders."),
        card->element(),
    });
}
