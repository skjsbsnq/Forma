// KDE Plasma versions of the GNOME-specific panels. They bind the same ui:: rows to KConfig entries (kwinrc,
// kdeglobals, kcminputrc, kxkbrc, powerdevilrc, ...) and use Plasma's own tools where a file write alone
// isn't enough (plasma-apply-colorscheme, plasma-apply-wallpaperimage, ...). Pointer devices go through
// KWin's InputDevice D-Bus objects, which apply and store the settings themselves.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "glib_support.hpp"
#include "json.hpp"
#include "kconfig.hpp"
#include "panels.hpp"
#include "panels_shared.hpp"
#include "ui.hpp"

using namespace mayo;
namespace fs = std::filesystem;
using kcfg::Reload;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// KPlugin.Name from a package's metadata.json (or Name= from metadata.desktop), else `fallback`.
std::string packageName(const fs::path& dir, const std::string& fallback) {
    std::error_code ec;
    if (fs::exists(dir / "metadata.json", ec)) {
        const auto doc = json::Value::parse(readFile(dir / "metadata.json"));
        if (auto n = doc["KPlugin"]["Name"].str(); !n.empty()) return n;
    }
    std::ifstream in(dir / "metadata.desktop");
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("Name=", 0) == 0) return ui::trim(line.substr(5));
    }
    return fallback;
}

/// Package directories (or files) under <data dir>/<sub>, first one wins for each name.
std::vector<fs::path> packages(const std::string& sub) {
    std::vector<fs::path> out;
    std::set<std::string> seen;
    for (const auto& d : shared::dataDirs()) {
        std::error_code ec;
        std::vector<fs::path> here;
        for (const auto& e : fs::directory_iterator(fs::path(d) / sub, ec)) here.push_back(e.path());
        std::sort(here.begin(), here.end());
        for (const auto& p : here) {
            if (seen.insert(p.filename().string()).second) out.push_back(p);
        }
    }
    return out;
}

/// Runs a Plasma tool if it is installed; returns false when it isn't.
bool runTool(std::initializer_list<const char*> names, const std::vector<std::string>& args) {
    const std::string tool = kcfg::findTool(names);
    if (tool.empty()) return false;
    std::vector<std::string> argv{tool};
    argv.insert(argv.end(), args.begin(), args.end());
    gx::run(argv, {});
    return true;
}

std::optional<Color> parseRgb(const std::string& text) {
    auto parts = ui::split(text, ',');
    if (parts.size() < 3) return std::nullopt;
    auto c = [&](int i) { return static_cast<float>(std::clamp(std::atoi(parts[static_cast<size_t>(i)].c_str()), 0, 255)) / 255.0f; };
    return Color{c(0), c(1), c(2), 1};
}

std::string hexOf(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)),
                  static_cast<int>(std::lround(c.b * 255)));
    return buf;
}

FormRow timeoutRow(const std::string& title, const gx::Key& enabled, const gx::Key& seconds, const std::vector<int>& choices,
                   const std::string& subtitle = {}) {
    std::vector<std::string> names{"Never"};
    for (int s : choices) names.push_back(s < 60 ? std::to_string(s) + " seconds" : ui::formatDuration(s));
    auto picker = Picker(names, 0);
    auto sync = [enabled, seconds, picker, choices]() mutable {
        if (!enabled.getBool()) return picker->setSelected(0);
        const auto secs = seconds.getInt();
        size_t best = 0;
        for (size_t i = 1; i < choices.size(); ++i) {
            if (std::abs(choices[i] - secs) < std::abs(choices[best] - secs)) best = i;
        }
        picker->setSelected(static_cast<int>(best) + 1);
    };
    sync();
    picker.onChange([enabled, seconds, choices](int i) {
        if (i == 0) return enabled.set(false);
        seconds.setInt(choices[static_cast<size_t>(i - 1)]);
        enabled.set(true);
    });
    enabled.watch(sync);
    seconds.watch(sync);
    return FormRow(title, picker, subtitle);
}

// Pointer devices (KWin) -------------------------------------------------------------------------------------

constexpr const char* kKWin = "org.kde.KWin";
constexpr const char* kInputRoot = "/org/kde/KWin/InputDevice";
constexpr const char* kInputDevice = "org.kde.KWin.InputDevice";

/// Object paths of KWin's pointer devices, touchpads or the others (mice, trackballs).
std::vector<std::string> pointerDevices(bool touchpads) {
    std::vector<std::string> out;
    auto names = gx::property(gx::sessionBus(), kKWin, kInputRoot, "org.kde.KWin.InputDeviceManager", "devicesSysNames");
    if (!names) return out;
    for (const auto& n : gx::strv(names.get())) {
        const std::string path = std::string(kInputRoot) + "/" + n;
        auto p = gx::properties(gx::sessionBus(), kKWin, path.c_str(), kInputDevice);
        if (!p || !gx::lookupBool(p.get(), "pointer")) continue;
        if (gx::lookupBool(p.get(), "touchpad") == touchpads) out.push_back(path);
    }
    return out;
}

std::string variantText(GVariant* v) {
    if (!v) return {};
    switch (g_variant_classify(v)) {
        case G_VARIANT_CLASS_BOOLEAN: return g_variant_get_boolean(v) ? "true" : "false";
        case G_VARIANT_CLASS_DOUBLE: {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];
            return g_ascii_formatd(buf, sizeof buf, "%.4g", g_variant_get_double(v));
        }
        case G_VARIANT_CLASS_INT32: return std::to_string(g_variant_get_int32(v));
        case G_VARIANT_CLASS_UINT32: return std::to_string(g_variant_get_uint32(v));
        case G_VARIANT_CLASS_STRING: return g_variant_get_string(v, nullptr);
        default: return {};
    }
}

/// A property of every device in `paths`, read from the first one. `supports` names the property that says
/// whether the hardware has the feature at all.
gx::Key deviceKey(const std::vector<std::string>& paths, const std::string& prop, const std::string& type,
                  const char* supports = nullptr) {
    if (paths.empty()) return {};
    if (supports) {
        auto s = gx::property(gx::sessionBus(), kKWin, paths[0].c_str(), kInputDevice, supports);
        if (!s || !g_variant_get_boolean(s.get())) return {};
    }
    auto b = std::make_shared<gx::KeyBackend>();
    b->type = type;
    b->fallback = type == "b" ? "false" : "0";
    b->read = [paths, prop]() -> std::optional<std::string> {
        auto v = gx::property(gx::sessionBus(), kKWin, paths[0].c_str(), kInputDevice, prop.c_str());
        if (!v) return std::nullopt;
        return variantText(v.get());
    };
    b->write = [paths, prop, type](const std::optional<std::string>& text) {
        if (!text) return;
        for (const auto& p : paths) {
            GVariant* v = type == "b" ? g_variant_new_boolean(*text == "true") : g_variant_new_double(g_ascii_strtod(text->c_str(), nullptr));
            gx::setProperty(gx::sessionBus(), kKWin, p.c_str(), kInputDevice, prop.c_str(), v);
        }
    };
    return gx::Key(b, prop);
}

FormRow flatAccelerationRow(const std::string& title, const gx::Key& flat) {
    return ui::toggleRow(title, flat, "Faster movements travel further", true);
}

// Keyboard layouts (kxkbrc) ------------------------------------------------------------------------------------

class KdeLayouts {
public:
    KdeLayouts() {
        m_layouts.watch([this] { rebuild(); });
        m_variants.watch([this] { rebuild(); });
        m_add.onClick([this] { openAdd(); });
        m_section.setAccessory(m_add);
        rebuild();
    }

    void openAdd() {
        std::vector<ui::Option> items;
        for (const auto& [id, name] : shared::xkbLayoutNames()) items.push_back({name, id});
        std::sort(items.begin(), items.end(), [](const ui::Option& a, const ui::Option& b) { return a.label < b.label; });
        ui::chooseFromList(ui::Anchor(m_add), items, [this](const std::string& id) {
            auto s = read();
            if (std::find(s.begin(), s.end(), id) == s.end()) s.push_back(id);
            write(s);
        }, "Search Layouts");
    }
    [[nodiscard]] Element element() const { return m_section.element(); }

private:
    /// "tr+f" style ids.
    [[nodiscard]] std::vector<std::string> read() const {
        const auto layouts = m_layouts.getStrv();
        const auto variants = m_variants.getStrv();
        std::vector<std::string> out;
        for (size_t i = 0; i < layouts.size(); ++i) {
            if (layouts[i].empty()) continue;
            const std::string v = i < variants.size() ? variants[i] : "";
            out.push_back(v.empty() ? layouts[i] : layouts[i] + "+" + v);
        }
        return out;
    }

    void write(const std::vector<std::string>& ids) {
        std::vector<std::string> layouts;
        std::vector<std::string> variants;
        for (const auto& id : ids) {
            const size_t plus = id.find('+');
            layouts.push_back(id.substr(0, plus));
            variants.push_back(plus == std::string::npos ? "" : id.substr(plus + 1));
        }
        kcfg::write("kxkbrc", "Layout", "Use", "true");
        kcfg::write("kxkbrc", "Layout", "VariantList", gx::joinList(variants));
        kcfg::write("kxkbrc", "Layout", "LayoutList", gx::joinList(layouts), Reload::Keyboard);
    }

    void rebuild() {
        const auto ids = read();
        std::vector<Element> rows;
        for (size_t i = 0; i < ids.size(); ++i) {
            const std::string& id = ids[i];
            auto up = Button(Icon::ChevronUp).style(ButtonStyle::Borderless).tooltip("Move Up").disabled(i == 0);
            up.onClick([this, i] {
                auto s = read();
                if (i > 0 && i < s.size()) std::swap(s[i], s[i - 1]);
                write(s);
            });
            auto remove = Button(Icon::Minus).style(ButtonStyle::Borderless).tooltip("Remove").disabled(ids.size() <= 1);
            remove.onClick([this, i] {
                auto s = read();
                if (s.size() > 1 && i < s.size()) s.erase(s.begin() + static_cast<long>(i));
                write(s);
            });
            std::string code = id.substr(0, id.find('+'));
            for (auto& ch : code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            auto badge = Canvas([code](Painter& p, const Rect& r) {
                p.fillRoundedRect(r, 4, theme().colors.secondaryFill);
                p.drawText(code, r, theme().font(TextStyle::Caption).withWeight(FontWeight::Semibold), theme().colors.secondaryLabel,
                           TextAlign::Center);
            });
            badge.frame(30, 18);
            auto it = shared::xkbLayoutNames().find(id);
            const std::string name = it != shared::xkbLayoutNames().end() ? it->second : id;
            rows.push_back(FormRow(HStack({badge, Label(name), Spacer(), up, remove}).spacing(10).align(Alignment::Center)));
        }
        m_section.setRows(rows);
    }

    gx::Key m_layouts = kcfg::key("kxkbrc", "Layout", "LayoutList", "as", "");
    gx::Key m_variants = kcfg::key("kxkbrc", "Layout", "VariantList", "as", "");
    Button m_add = Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("Add Layout…");
    ui::DynamicSection m_section{"Keyboard Layouts", "No layouts configured: the system default is used.",
                                 "The first layout is the default. Switch with Meta+Alt+K."};
};

/// XKB options sharing a prefix, kept in kxkbrc's Options list.
FormRow optionRow(const std::string& title, const std::vector<std::string>& prefixes, std::vector<ui::Option> options,
                  const std::string& subtitle = {}) {
    gx::Key key = kcfg::key("kxkbrc", "Layout", "Options", "as", "");
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
        // Without ResetOldOptions, KDE only ever adds options on top of the old ones.
        kcfg::write("kxkbrc", "Layout", "ResetOldOptions", "true");
        kcfg::write("kxkbrc", "Layout", "Options", gx::joinList(v), Reload::Keyboard);
    });
    key.watch(sync);
    return FormRow(title, picker, subtitle);
}

// Appearance ------------------------------------------------------------------------------------------------

struct ColorScheme {
    std::string id;
    std::string name;
    bool dark = false;
};

std::vector<ColorScheme> colorSchemes() {
    std::vector<ColorScheme> out;
    for (const auto& path : packages("color-schemes")) {
        if (path.extension() != ".colors") continue;
        GKeyFile* kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
            ColorScheme s;
            s.id = path.stem().string();
            char* name = g_key_file_get_locale_string(kf, "General", "Name", nullptr, nullptr);
            s.name = name ? name : s.id;
            g_free(name);
            char* bg = g_key_file_get_string(kf, "Colors:Window", "BackgroundNormal", nullptr);
            if (auto c = parseRgb(bg ? bg : "")) s.dark = 0.299f * c->r + 0.587f * c->g + 0.114f * c->b < 0.5f;
            g_free(bg);
            out.push_back(std::move(s));
        }
        g_key_file_free(kf);
    }
    std::sort(out.begin(), out.end(), [](const ColorScheme& a, const ColorScheme& b) { return ui::lower(a.name) < ui::lower(b.name); });
    return out;
}

struct NamedColor {
    const char* name;
    Color color;
};

// Plasma's accent palette (the colours the desktop renders, not UI colours of ours).
const NamedColor kPlasmaAccents[] = {
    {"Pink", Color::hex(0xe93a9a)},  {"Red", Color::hex(0xe93d58)},    {"Orange", Color::hex(0xe9643a)},
    {"Yellow", Color::hex(0xe8cb2d)}, {"Green", Color::hex(0x3dd425)},  {"Teal", Color::hex(0x00d3b8)},
    {"Blue", Color::hex(0x3daee9)},  {"Purple", Color::hex(0xb875dc)}, {"Violet", Color::hex(0x926ee4)},
    {"Gray", Color::hex(0x686b6f)},
};
constexpr int kAccentCount = static_cast<int>(std::size(kPlasmaAccents));

int accentIndex(const std::string& rgb) {
    auto c = parseRgb(rgb);
    if (!c) return -1;
    for (int i = 0; i < kAccentCount; ++i) {
        if (hexOf(kPlasmaAccents[i].color) == hexOf(*c)) return i;
    }
    return -1;
}

Element styleSection() {
    auto schemes = std::make_shared<std::vector<ColorScheme>>(colorSchemes());
    auto applyScheme = [](const std::string& id) {
        if (!runTool({"plasma-apply-colorscheme"}, {id})) kcfg::write("kdeglobals", "General", "ColorScheme", id, Reload::Style);
    };
    gx::Key scheme = kcfg::appliedKey("kdeglobals", "General", "ColorScheme", "s", "BreezeLight", applyScheme);
    gx::Key accent = kcfg::appliedKey("kdeglobals", "General", "AccentColor", "s", "", [](const std::string& rgb) {
        auto c = parseRgb(rgb);
        if (!c || !runTool({"plasma-apply-colorscheme"}, {"--accent-color", hexOf(*c)}))
            kcfg::write("kdeglobals", "General", "AccentColor", rgb, Reload::Style);
    });

    auto find = [schemes](const std::string& preferred, bool dark) -> std::string {
        for (const auto& s : *schemes) {
            if (s.id == preferred) return s.id;
        }
        for (const auto& s : *schemes) {
            if (s.dark == dark) return s.id;
        }
        return preferred;
    };
    const std::string lightId = find("BreezeLight", false);
    const std::string darkId = find("BreezeDark", true);
    auto isDark = [schemes](const std::string& id) {
        for (const auto& s : *schemes) {
            if (s.id == id) return s.dark;
        }
        return ui::lower(id).find("dark") != std::string::npos;
    };

    auto styles = ui::ChoiceGrid(2, {150, 118}, 18, [accent](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
        const auto& c = theme().colors;
        const Rect preview{cell.x, cell.y, cell.width, cell.height - 22};
        shared::stylePreview(p, preview, i == 1, parseRgb(accent.getString()).value_or(c.accent));
        if (selected) p.strokeRoundedRect(preview.outset(3.5f), 11, c.accent, 3);
        else if (hovered) p.strokeRoundedRect(preview.outset(3.5f), 11, c.secondaryFill, 3);
        const Font f = theme().font(TextStyle::Subheadline).withWeight(selected ? FontWeight::Semibold : FontWeight::Regular);
        p.drawText(mayo::uiText(i == 0 ? "Light" : "Dark"), {cell.x, preview.bottom() + 5, cell.width, 16}, f,
                   selected ? c.label : c.secondaryLabel, TextAlign::Center);
    });
    auto syncStyle = [scheme, styles, isDark] { styles->setSelected(isDark(scheme.getString()) ? 1 : 0); };
    syncStyle();
    styles.onChoose([scheme, lightId, darkId](int i) { scheme.setString(i == 1 ? darkId : lightId); });
    scheme.watch(syncStyle);

    std::vector<ui::Option> schemeOptions;
    for (const auto& s : *schemes) schemeOptions.push_back({s.name, s.id});

    auto dots = ui::ChoiceGrid(kAccentCount, {22, 22}, 12, [](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
        const Point ctr = cell.center();
        p.drawShadow(cell, 11, Color::black(0.15f), 2, {0, 0.5f});
        p.fillCircle(ctr, 11, kPlasmaAccents[i].color);
        p.strokeCircle(ctr, 11, Color::black(0.12f), 0.5f);
        if (selected) p.fillCircle(ctr, 3, Color::white());
        else if (hovered) p.strokeCircle(ctr, 13.5f, theme().colors.secondaryFill, 2);
    });
    dots.caption([](int i) { return i >= 0 ? std::string(kPlasmaAccents[i].name) : std::string("From Color Scheme"); });
    auto syncAccent = [accent, dots, styles] {
        dots->setSelected(accentIndex(accent.getString()));
        styles->invalidate();
    };
    syncAccent();
    dots.onChoose([accent](int i) {
        const Color c = kPlasmaAccents[i].color;
        accent.setString(std::to_string(std::lround(c.r * 255)) + "," + std::to_string(std::lround(c.g * 255)) + "," +
                         std::to_string(std::lround(c.b * 255)));
    });
    accent.watch(syncAccent);

    std::vector<Element> rows{FormRow(HStack({Spacer(), styles, Spacer()})), FormRow("Accent Color", dots)};
    if (!schemeOptions.empty()) rows.push_back(ui::pickerRow("Color Scheme", scheme, schemeOptions));

    // Global themes bundle colours, icons, Plasma style and layout.
    std::vector<ui::Option> globals;
    for (const auto& dir : packages("plasma/look-and-feel")) globals.push_back({packageName(dir, dir.filename().string()), dir.filename().string()});
    if (!globals.empty() && !kcfg::findTool({"plasma-apply-lookandfeel"}).empty()) {
        rows.push_back(ui::pickerRow("Global Theme",
                                     kcfg::appliedKey("kdeglobals", "KDE", "LookAndFeelPackage", "s", "org.kde.breeze.desktop",
                                                      [](const std::string& id) { runTool({"plasma-apply-lookandfeel"}, {"-a", id}); }),
                                     globals, "Applies colours, icons, Plasma style and cursors together"));
    }
    return ui::section("Style", rows);
}

struct Wallpaper {
    std::string name;
    std::string target;  ///< what plasma-apply-wallpaperimage gets: a package directory or an image
    std::string preview;
    std::shared_ptr<ImageData> thumb;
};

/// Largest image of a wallpaper package ("contents/images/3840x2160.png").
std::string packageImage(const fs::path& dir) {
    std::string best;
    long bestArea = -1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "contents" / "images", ec)) {
        if (!shared::isImage(e.path().string())) continue;
        int w = 0;
        int h = 0;
        std::sscanf(e.path().stem().c_str(), "%dx%d", &w, &h);
        const long area = static_cast<long>(w) * h;
        if (area > bestArea) {
            bestArea = area;
            best = e.path().string();
        }
    }
    return best;
}

std::vector<Wallpaper> findWallpapers() {
    std::vector<Wallpaper> out;
    for (const auto& p : packages("wallpapers")) {
        std::error_code ec;
        Wallpaper w;
        if (fs::is_directory(p, ec)) {
            const std::string image = packageImage(p);
            if (image.empty()) continue;
            w.name = packageName(p, p.filename().string());
            w.target = p.string();
            w.preview = fs::exists(p / "contents" / "screenshot.png", ec) ? (p / "contents" / "screenshot.png").string() : image;
        } else if (shared::isImage(p.string())) {
            w.name = p.stem().string();
            w.target = w.preview = p.string();
        } else {
            continue;
        }
        out.push_back(std::move(w));
    }
    return out;
}

constexpr const char* kAppletsRc = "plasma-org.kde.plasma.desktop-appletsrc";

/// Desktop containments' image wallpaper groups.
std::vector<std::string> wallpaperGroups() {
    std::vector<std::string> out;
    const std::string suffix = "][Wallpaper][org.kde.image][General";
    for (const auto& g : kcfg::groups(kAppletsRc)) {
        if (g.size() > suffix.size() && g.compare(g.size() - suffix.size(), suffix.size(), suffix) == 0) out.push_back(g);
    }
    return out;
}

std::string normalizePath(std::string p) {
    if (p.rfind("file://", 0) == 0) p = p.substr(7);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

Element backgroundSection(Context& ctx) {
    if (kcfg::findTool({"plasma-apply-wallpaperimage"}).empty()) return Element();
    auto walls = std::make_shared<std::vector<Wallpaper>>(findWallpapers());
    auto grid = ui::ChoiceGrid(static_cast<int>(walls->size()), {140, 88}, 12,
                               [walls](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
                                   const auto& c = theme().colors;
                                   const Wallpaper& w = (*walls)[static_cast<size_t>(i)];
                                   if (w.thumb) shared::cover(p, cell, *w.thumb, 8);
                                   else p.fillVerticalGradient(cell, 8, c.secondaryFill, c.fill);
                                   p.strokeRoundedRect(cell, 8, Color::black(0.1f), 0.5f);
                                   if (selected) p.strokeRoundedRect(cell.outset(3.5f), 11, c.accent, 3);
                                   else if (hovered) p.strokeRoundedRect(cell.outset(3.5f), 11, c.secondaryFill, 3);
                               });
    grid.wraps().caption([walls](int i) { return (*walls)[static_cast<size_t>(i)].name; });

    auto sync = [walls, grid] {
        std::string current;
        for (const auto& g : wallpaperGroups()) {
            if (auto v = kcfg::read(kAppletsRc, g, "Image")) {
                current = normalizePath(*v);
                break;
            }
        }
        int sel = -1;
        for (size_t i = 0; i < walls->size(); ++i) {
            if (normalizePath((*walls)[i].target) == current) sel = static_cast<int>(i);
        }
        grid->setSelected(sel);
    };
    auto loadThumbs = [walls, grid](size_t from) {
        for (size_t i = from; i < walls->size(); ++i) {
            gx::background([walls, grid, i, path = (*walls)[i].preview] {
                auto img = shared::thumbnail(path);
                if (!img) return;
                gx::toUi([walls, grid, i, img] {
                    if (i < walls->size()) (*walls)[i].thumb = img;
                    grid->invalidate();
                });
            });
        }
    };
    auto apply = [](const Wallpaper& w) { runTool({"plasma-apply-wallpaperimage"}, {w.target}); };
    sync();
    loadThumbs(0);
    grid.onChoose([walls, apply](int i) { apply((*walls)[static_cast<size_t>(i)]); });
    for (const auto& g : wallpaperGroups()) kcfg::watch(kAppletsRc, g, "Image", sync);

    auto add = Button(Icon::Plus, "Add Picture…");
    add.onClick([&app = ctx.app, &window = ctx.window, walls, grid, apply, loadThumbs] {
        app.openFiles({{"Images", "png;jpg;jpeg;webp;jxl;svg;avif"}}, true,
                      [walls, grid, apply, loadThumbs](std::vector<std::string> paths) {
                          if (paths.empty()) return;
                          const size_t first = walls->size();
                          const fs::path dir = fs::path(g_get_user_data_dir()) / "wallpapers";
                          for (const auto& p : paths) {
                              std::string target = p;
                              if (!gx::readOnly()) {
                                  std::error_code ec;
                                  fs::create_directories(dir, ec);
                                  const fs::path t = dir / fs::path(p).filename();
                                  if (fs::exists(t, ec) || fs::copy_file(p, t, ec)) target = t.string();
                              }
                              walls->push_back({fs::path(p).stem().string(), target, target, nullptr});
                          }
                          grid->setCount(static_cast<int>(walls->size()));
                          loadThumbs(first);
                          apply(walls->back());
                      },
                      &window);
    });
    return ui::section("Wallpaper", {FormRow(grid).padding(Insets(10, 8)), FormRow(HStack({Spacer(), add}))},
                       "Applies to every desktop; Plasma's lock screen keeps its own wallpaper.");
}

/// Qt font strings: "Noto Sans,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1" (family, point size, ...).
FormRow qtFontRow(const std::string& title, const std::string& group, const std::string& entry, const std::string& fallback) {
    gx::Key key = kcfg::key("kdeglobals", group, entry, "s", fallback, Reload::Fonts);
    auto fields = [key] {
        auto f = ui::split(key.getString(), ',');
        if (f.size() < 2) f = ui::split(key.getString() + ",10", ',');
        return f;
    };
    auto join = [](const std::vector<std::string>& f) {
        std::string s;
        for (size_t i = 0; i < f.size(); ++i) s += (i ? "," : "") + f[i];
        return s;
    };
    auto button = Button("");
    std::vector<std::string> sizes;
    for (int s = 6; s <= 24; ++s) sizes.push_back(std::to_string(s));
    auto size = Picker(sizes, 0);
    auto sync = [fields, button, size]() mutable {
        const auto f = fields();
        button.setTitle(f[0]);
        size->setSelected(std::clamp(std::atoi(f[1].c_str()), 6, 24) - 6);
    };
    sync();
    button.onClick([key, fields, join, anchor = ui::Anchor(button)] {
        std::vector<ui::Option> items;
        for (const auto& f : shared::fontFamilies()) items.push_back({f, f});
        ui::chooseFromList(anchor, items, [key, fields, join](const std::string& family) {
            auto f = fields();
            f[0] = family;
            key.setString(join(f));
        }, "Search Fonts");
    });
    size.onChange([key, fields, join](int i) {
        auto f = fields();
        f[1] = std::to_string(i + 6);
        key.setString(join(f));
    });
    key.watch(sync);
    return FormRow(title, HStack({button, size}).spacing(6).align(Alignment::Center));
}

Element fontsSection() {
    const std::string tail = ",-1,5,400,0,0,0,0,0,0,0,0,0,0,1";
    return ui::section("Fonts", {
        qtFontRow("General", "General", "font", "Noto Sans,10" + tail),
        qtFontRow("Fixed Width", "General", "fixed", "Hack,10" + tail),
        qtFontRow("Small", "General", "smallestReadableFont", "Noto Sans,8" + tail),
        qtFontRow("Toolbar", "General", "toolBarFont", "Noto Sans,10" + tail),
        qtFontRow("Menu", "General", "menuFont", "Noto Sans,10" + tail),
        qtFontRow("Window Title", "WM", "activeFont", "Noto Sans,10" + tail),
        ui::toggleRow("Anti-Aliasing", kcfg::key("kdeglobals", "General", "XftAntialias", "b", "true", Reload::Fonts)),
        ui::pickerRow("Hinting", kcfg::key("kdeglobals", "General", "XftHintStyle", "s", "hintslight", Reload::Fonts),
                      {{"Full", "hintfull"}, {"Medium", "hintmedium"}, {"Slight", "hintslight"}, {"None", "hintnone"}}),
        ui::pickerRow("Sub-Pixel Rendering", kcfg::key("kdeglobals", "General", "XftSubPixel", "s", "none", Reload::Fonts),
                      {{"None", "none"}, {"RGB", "rgb"}, {"BGR", "bgr"}, {"Vertical RGB", "vrgb"}, {"Vertical BGR", "vbgr"}}),
    }, "Applications pick up font changes when they start.");
}

Element themesSection() {
    std::error_code ec;
    auto cursors = shared::themeDirs("icons", [&](const fs::path& p) { return fs::exists(p / "cursors", ec); });
    auto icons = shared::themeDirs("icons", [&](const fs::path& p) {
        return fs::exists(p / "index.theme", ec) && !fs::exists(p / "cursors", ec) && p.filename() != "default" && p.filename() != "hicolor";
    });
    std::vector<ui::Option> plasmaStyles;
    for (const auto& dir : packages("plasma/desktoptheme")) {
        const std::string id = dir.filename().string();
        plasmaStyles.push_back({id == "default" ? "Breeze" : packageName(dir, id), id});
    }
    std::vector<ui::Option> widgetStyles{{"Breeze", "Breeze"}, {"Fusion", "Fusion"}};
    for (const char* dir : {"/usr/lib/qt6/plugins/styles", "/usr/lib64/qt6/plugins/styles", "/usr/lib/x86_64-linux-gnu/qt6/plugins/styles"}) {
        if (fs::exists(fs::path(dir) / "libkvantum.so", ec)) widgetStyles.push_back({"Kvantum", "kvantum"});
        if (fs::exists(fs::path(dir) / "oxygen6.so", ec) || fs::exists(fs::path(dir) / "oxygen.so", ec)) widgetStyles.push_back({"Oxygen", "Oxygen"});
    }

    std::vector<Element> rows{
        ui::pickerRow("Icons", kcfg::appliedKey("kdeglobals", "Icons", "Theme", "s", "breeze", [](const std::string& t) {
                          if (!runTool({"plasma-changeicons"}, {t})) kcfg::write("kdeglobals", "Icons", "Theme", t, Reload::Icons);
                      }), icons),
        ui::pickerRow("Cursor", kcfg::appliedKey("kcminputrc", "Mouse", "cursorTheme", "s", "breeze_cursors", [](const std::string& t) {
                          if (!runTool({"plasma-apply-cursortheme"}, {t})) kcfg::write("kcminputrc", "Mouse", "cursorTheme", t, Reload::Cursor);
                      }), cursors),
        ui::pickerRow("Cursor Size", kcfg::key("kcminputrc", "Mouse", "cursorSize", "i", "24", Reload::Cursor),
                      {{"Default (24)", "24"}, {"Medium (32)", "32"}, {"Large (48)", "48"}, {"Larger (64)", "64"}, {"Largest (96)", "96"}}),
        ui::pickerRow("Application Style", kcfg::key("kdeglobals", "KDE", "widgetStyle", "s", "Breeze", Reload::Style), widgetStyles,
                      "Qt widget style; restart apps to see it everywhere"),
    };
    if (!plasmaStyles.empty() && !kcfg::findTool({"plasma-apply-desktoptheme"}).empty()) {
        rows.push_back(ui::pickerRow("Plasma Style", kcfg::appliedKey("plasmarc", "Theme", "name", "s", "default", [](const std::string& t) {
                                         runTool({"plasma-apply-desktoptheme"}, {t});
                                     }), plasmaStyles, "Panels and widgets"));
    }
    return ui::section("Themes", rows);
}

}  // namespace

namespace kde {

Element appearancePanel(Context& ctx) {
    if (ctx.demo == "fonts") {
        ctx.app.after(0.6, [anchor = ctx.anchor] {
            std::vector<ui::Option> items;
            for (const auto& f : shared::fontFamilies()) items.push_back({f, f});
            ui::chooseFromList(anchor, items, [](const std::string&) {}, "Search Fonts");
        });
    }
    std::vector<Element> sections{styleSection()};
    if (auto bg = backgroundSection(ctx)) sections.push_back(bg);
    sections.push_back(fontsSection());
    sections.push_back(themesSection());
    return ui::page(sections);
}

// Multitasking ------------------------------------------------------------------------------------------------

namespace {

constexpr const char* kDesktops = "org.kde.KWin.VirtualDesktopManager";

/// Number of virtual desktops through KWin's D-Bus manager (it creates and removes them live).
class VirtualDesktops {
public:
    VirtualDesktops() {
        std::vector<std::string> numbers;
        for (int i = 1; i <= 20; ++i) numbers.push_back(std::to_string(i));
        m_picker = Picker(numbers, std::clamp(count(), 1, 20) - 1);
        m_picker.onChange([this](int i) { setCount(i + 1); });
        if (count() <= 0) m_picker.disabled();
        gx::subscribe(gx::sessionBus(), kKWin, kDesktops, nullptr, "/VirtualDesktopManager",
                      [this](const std::string&, const std::string&, GVariant*) { m_changed(); });
    }
    [[nodiscard]] Element row() const { return FormRow("Number of Desktops", m_picker); }

private:
    static int count() {
        auto v = gx::property(gx::sessionBus(), kKWin, "/VirtualDesktopManager", kDesktops, "count");
        return v ? static_cast<int>(g_variant_get_uint32(v.get())) : 0;
    }
    void setCount(int n) {
        auto list = gx::property(gx::sessionBus(), kKWin, "/VirtualDesktopManager", kDesktops, "desktops");
        if (!list) return;
        std::vector<std::pair<guint32, std::string>> desktops;
        GVariantIter it;
        g_variant_iter_init(&it, list.get());
        guint32 pos = 0;
        const char* id = nullptr;
        const char* name = nullptr;
        while (g_variant_iter_next(&it, "(u&s&s)", &pos, &id, &name)) desktops.emplace_back(pos, id);
        std::sort(desktops.begin(), desktops.end());
        for (int i = static_cast<int>(desktops.size()); i < n; ++i) {
            gx::perform(gx::sessionBus(), kKWin, "/VirtualDesktopManager", kDesktops, "createDesktop",
                        g_variant_new("(us)", static_cast<guint32>(i), ("Desktop " + std::to_string(i + 1)).c_str()));
        }
        for (int i = static_cast<int>(desktops.size()) - 1; i >= n; --i) {
            gx::perform(gx::sessionBus(), kKWin, "/VirtualDesktopManager", kDesktops, "removeDesktop",
                        g_variant_new("(s)", desktops[static_cast<size_t>(i)].second.c_str()));
        }
    }

    Picker m_picker{std::vector<std::string>{"1"}, 0};
    gx::Debounce m_changed{0.2, [this] {
        if (int c = count(); c > 0) m_picker->setSelected(std::clamp(c, 1, 20) - 1);
    }};
};

gx::Key kwin(const std::string& group, const std::string& key, const std::string& type, const std::string& fallback) {
    return kcfg::key("kwinrc", group, key, type, fallback, Reload::KWin);
}

}  // namespace

Element multitaskingPanel(Context& /*ctx*/) {
    auto desktops = std::make_shared<VirtualDesktops>();
    return ui::page(
        {
            ui::section("Screen Edges",
                        {ui::toggleRow("Hot Corner", kcfg::flagKey("kwinrc", "Effect-overview", "BorderActivate", "7", "9", true, Reload::KWin),
                                       "Touch the top-left corner to open the Overview"),
                         ui::toggleRow("Maximize at the Top Edge", kwin("Windows", "ElectricBorderMaximize", "b", "true"),
                                       "Drag a window against the top edge to maximize it"),
                         ui::toggleRow("Tile at the Side Edges", kwin("Windows", "ElectricBorderTiling", "b", "true"),
                                       "Drag a window against the left or right edge to fill that half")}),
            ui::section("Virtual Desktops",
                        {desktops->row(),
                         ui::pickerRow("Rows", kcfg::key("kwinrc", "Desktops", "Rows", "i", "1", Reload::KWin),
                                       {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}}),
                         ui::toggleRow("Navigation Wraps Around", kwin("Windows", "RollOverDesktops", "b", "true"))}),
            ui::section("Task Switcher",
                        {ui::pickerRow("Switch Between", kwin("TabBox", "DesktopMode", "i", "1"),
                                       {{"Windows on All Desktops", "0"}, {"Windows on the Current Desktop", "1"}}),
                         ui::pickerRow("Show Windows From", kwin("TabBox", "MultiScreenMode", "i", "0"),
                                       {{"All Screens", "0"}, {"The Current Screen", "1"}})}),
            ui::section("Windows",
                        {ui::pickerRow("Titlebar Buttons", kcfg::key("kwinrc", "org.kde.kdecoration2", "ButtonsOnRight", "s", "IAX", Reload::KWin),
                                       {{"Close", "X"}, {"Minimize, Close", "IX"}, {"Minimize, Maximize, Close", "IAX"},
                                        {"Help, Minimize, Maximize, Close", "HIAX"}}),
                         ui::pickerRow("Double-Click Titlebar", kwin("Windows", "TitlebarDoubleClickCommand", "s", "Maximize"),
                                       {{"Maximize", "Maximize"}, {"Minimize", "Minimize"}, {"Shade", "Shade"}, {"Lower", "Lower"},
                                        {"Close", "Close"}, {"Nothing", "Nothing"}}),
                         ui::pickerRow("Window Focus", kwin("Windows", "FocusPolicy", "s", "ClickToFocus"),
                                       {{"Click to Focus", "ClickToFocus"}, {"Focus Follows Mouse", "FocusFollowsMouse"},
                                        {"Focus Under Mouse", "FocusUnderMouse"}, {"Focus Strictly Under Mouse", "FocusStrictlyUnderMouse"}}),
                         ui::pickerRow("New Window Placement", kwin("Windows", "Placement", "s", "Centered"),
                                       {{"Centered", "Centered"}, {"Minimal Overlapping", "Smart"}, {"Maximized", "Maximizing"},
                                        {"Under the Pointer", "UnderMouse"}, {"Top-Left Corner", "ZeroCornered"}, {"Random", "Random"}}),
                         ui::toggleRow("Separate Screen Focus", kwin("Windows", "SeparateScreenFocus", "b", "false"),
                                       "Each screen keeps its own focused window")}),
        },
        desktops);
}

// Notifications -------------------------------------------------------------------------------------------------

Element notificationsPanel(Context& /*ctx*/) {
    constexpr const char* kRc = "plasmanotifyrc";
    // Do Not Disturb is a date: notifications are inhibited until then (KConfig's QDateTime list format).
    auto dndBackend = std::make_shared<gx::KeyBackend>();
    dndBackend->type = "b";
    dndBackend->fallback = "false";
    dndBackend->read = []() -> std::optional<std::string> {
        auto v = kcfg::read(kRc, "DoNotDisturb", "Until");
        if (!v || v->empty()) return std::nullopt;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        std::sscanf(v->c_str(), "%d,%d,%d,%d,%d", &y, &mo, &d, &h, &mi);
        GDateTime* until = g_date_time_new_local(y, std::max(mo, 1), std::max(d, 1), h, mi, 0);
        GDateTime* now = g_date_time_new_now_local();
        const bool active = until && g_date_time_compare(until, now) > 0;
        if (until) g_date_time_unref(until);
        g_date_time_unref(now);
        return active ? "true" : "false";
    };
    dndBackend->write = [](const std::optional<std::string>& v) {
        kcfg::write(kRc, "DoNotDisturb", "Until", v && *v == "true" ? std::optional<std::string>("2099,12,31,23,59,0") : std::nullopt);
    };
    dndBackend->watch = [](std::function<void()> fn) { kcfg::watch(kRc, "DoNotDisturb", "Until", std::move(fn)); };

    auto apps = std::make_shared<ui::DynamicSection>("App Notifications", "No apps have sent notifications yet.");
    std::vector<Element> rows;
    struct Entry {
        std::string name;
        std::string group;
    };
    std::vector<Entry> entries;
    for (const auto& g : kcfg::groups(kRc)) {
        if (g.rfind("Applications][", 0) != 0) continue;
        const std::string id = g.substr(14);
        std::string name = gx::appName(id + ".desktop");
        if (name.empty()) continue;
        entries.push_back({name, g});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return ui::lower(a.name) < ui::lower(b.name); });
    for (const auto& e : entries) {
        gx::Key show = kcfg::key(kRc, e.group, "ShowPopups", "b", "true");
        auto toggle = Toggle("", show.getBool(true)).onChange([show](bool on) { show.set(on); });
        show.watch([show, toggle]() mutable { toggle.setOn(show.getBool(true)); });
        auto row = ui::ClickRow({ui::letterTile(e.name), Label(e.name), Spacer(), toggle});
        row.onClick([anchor = ui::Anchor(row), group = e.group, name = e.name] {
            auto details = ui::section(name, {
                ui::toggleRow("Show Popups", kcfg::key(kRc, group, "ShowPopups", "b", "true")),
                ui::toggleRow("Show in Do Not Disturb", kcfg::key(kRc, group, "ShowPopupsInDndMode", "b", "false")),
                ui::toggleRow("Keep in History", kcfg::key(kRc, group, "ShowInHistory", "b", "true")),
                ui::toggleRow("Show Badges", kcfg::key(kRc, group, "ShowBadges", "b", "true"), "Unread counts on the app's task"),
            });
            Popover(VStack({details}).width(380)).show(anchor);
        });
        rows.push_back(row);
    }
    apps->setRows(rows);

    return ui::page({
        ui::section({}, {ui::toggleRow("Do Not Disturb", gx::Key(dndBackend, "Until"), "Notification popups are hidden until you turn this off"),
                         ui::toggleRow("Critical Notifications in Do Not Disturb", kcfg::key(kRc, "Notifications", "CriticalInDndMode", "b", "true")),
                         ui::toggleRow("Do Not Disturb While Screens Are Mirrored", kcfg::key(kRc, "DoNotDisturb", "WhenScreensMirrored", "b", "true"),
                                       "Useful for presentations")}),
        ui::section("Popups",
                    {ui::pickerRow("Position", kcfg::key(kRc, "Notifications", "PopupPosition", "s", "CloseToWidget"),
                                   {{"Near the Notification Icon", "CloseToWidget"}, {"Top Left", "TopLeft"}, {"Top Center", "TopCenter"},
                                    {"Top Right", "TopRight"}, {"Bottom Left", "BottomLeft"}, {"Bottom Center", "BottomCenter"},
                                    {"Bottom Right", "BottomRight"}}),
                     ui::pickerRow("Hide After", kcfg::key(kRc, "Notifications", "PopupTimeout", "i", "5000"),
                                   {{"3 seconds", "3000"}, {"5 seconds", "5000"}, {"7 seconds", "7000"}, {"10 seconds", "10000"},
                                    {"15 seconds", "15000"}, {"30 seconds", "30000"}}),
                     ui::toggleRow("Low Priority Popups", kcfg::key(kRc, "Notifications", "LowPriorityPopups", "b", "false")),
                     ui::toggleRow("Application Progress", kcfg::key(kRc, "Jobs", "InNotifications", "b", "true"),
                                   "Copying files and downloads show their progress"),
                     ui::toggleRow("Badges in the Task Manager", kcfg::key(kRc, "Badges", "InTaskManager", "b", "true"))}),
        apps->element(),
    });
}

// Search ------------------------------------------------------------------------------------------------------------

Element searchPanel(Context& /*ctx*/) {
    constexpr const char* kBaloo = "baloofilerc";
    auto indexing = std::make_shared<gx::KeyBackend>();
    indexing->type = "b";
    indexing->fallback = "true";
    indexing->read = [] { return kcfg::read(kBaloo, "Basic Settings", "Indexing-Enabled"); };
    indexing->write = [](const std::optional<std::string>& v) {
        const bool on = !v || *v == "true";
        kcfg::write(kBaloo, "Basic Settings", "Indexing-Enabled", on ? "true" : "false");
        runTool({"balooctl6", "balooctl"}, {on ? "enable" : "disable"});
    };
    indexing->watch = [](std::function<void()> fn) { kcfg::watch(kBaloo, "Basic Settings", "Indexing-Enabled", std::move(fn)); };
    return ui::page({
        ui::section("File Search",
                    {ui::toggleRow("File Indexing", gx::Key(indexing, "Indexing-Enabled"), "Find files by name in KRunner and Dolphin"),
                     ui::toggleRow("Index File Content", kcfg::key(kBaloo, "General", "only basicIndexing", "b", "false"),
                                   "Search inside documents, not just names", true),
                     ui::toggleRow("Index Hidden Files", kcfg::key(kBaloo, "General", "index hidden folders", "b", "false"))}),
        ui::section("KRunner",
                    {ui::pickerRow("Position", kcfg::key("krunnerrc", "General", "FreeFloating", "b", "false"),
                                   {{"Top of the Screen", "false"}, {"Center", "true"}}),
                     ui::toggleRow("Search When Typing on the Desktop", kcfg::key("krunnerrc", "General", "ActivateWhenTypingOnDesktop", "b", "true")),
                     ui::pickerRow("History", kcfg::key("krunnerrc", "General", "historyBehavior", "s", "CompletionSuggestion"),
                                   {{"Suggest Completions", "CompletionSuggestion"}, {"Show Recent Searches", "ImmediateCompletion"},
                                    {"Off", "Disabled"}})}),
    });
}

// Mouse & Touchpad ------------------------------------------------------------------------------------------------

Element mousePanel(Context& /*ctx*/) {
    const auto mice = pointerDevices(false);
    const auto pads = pointerDevices(true);
    auto all = mice;
    all.insert(all.end(), pads.begin(), pads.end());
    auto scroll = [](float v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2f×", static_cast<double>(v));
        return std::string(buf);
    };
    std::vector<Element> sections{
        ui::section("General",
                    {ui::pickerRow("Primary Button", deviceKey(all, "leftHanded", "b", "supportsLeftHanded"), {{"Left", "false"}, {"Right", "true"}},
                                   "Order of the physical buttons"),
                     ui::sliderValueRow("Double-Click Delay", kcfg::key("kdeglobals", "KDE", "DoubleClickInterval", "i", "400", Reload::Mouse),
                                        100, 1000, 50, shared::ms),
                     ui::pickerRow("Clicking Files and Folders", kcfg::key("kdeglobals", "KDE", "SingleClick", "b", "false", Reload::Mouse),
                                   {{"Selects Them", "false"}, {"Opens Them", "true"}})})};
    if (all.empty()) {
        sections.push_back(ui::section({}, {FormRow(Label("Pointer devices are configured through KWin on Wayland; it isn't available.").secondary())}));
        return ui::page(sections);
    }
    if (!mice.empty()) {
        sections.push_back(ui::section(
            "Mouse",
            {ui::sliderRow("Pointer Speed", deviceKey(mice, "pointerAcceleration", "d", "supportsPointerAcceleration"), -1, 1, 0, "Slow to fast"),
             flatAccelerationRow("Mouse Acceleration", deviceKey(mice, "pointerAccelerationProfileFlat", "b", "supportsPointerAccelerationProfileFlat")),
             ui::toggleRow("Natural Scrolling", deviceKey(mice, "naturalScroll", "b", "supportsNaturalScroll"), "Scrolling moves the content, not the view"),
             ui::sliderValueRow("Scrolling Speed", deviceKey(mice, "scrollFactor", "d"), 0.2f, 3, 0.05f, scroll),
             ui::toggleRow("Middle Click Emulation", deviceKey(mice, "middleEmulation", "b", "supportsMiddleEmulation"),
                           "Pressing both buttons together pastes")}));
    }
    if (!pads.empty()) {
        sections.push_back(ui::section(
            "Touchpad",
            {ui::toggleRow("Touchpad", deviceKey(pads, "enabled", "b", "supportsDisableEvents")),
             ui::toggleRow("Disable While Typing", deviceKey(pads, "disableWhileTyping", "b", "supportsDisableWhileTyping")),
             ui::sliderRow("Pointer Speed", deviceKey(pads, "pointerAcceleration", "d", "supportsPointerAcceleration"), -1, 1, 0, "Slow to fast"),
             flatAccelerationRow("Touchpad Acceleration", deviceKey(pads, "pointerAccelerationProfileFlat", "b", "supportsPointerAccelerationProfileFlat"))}));
        sections.push_back(ui::section(
            "Clicking",
            {ui::pickerRow("Secondary Click", deviceKey(pads, "clickMethodClickfinger", "b", "supportsClickMethodClickfinger"),
                           {{"Two Finger Push", "true"}, {"Corner Push", "false"}}),
             ui::toggleRow("Tap to Click", deviceKey(pads, "tapToClick", "b"), "Quickly touch the touchpad to click"),
             ui::toggleRow("Tap and Drag", deviceKey(pads, "tapAndDrag", "b"), "Double-tap and hold to drag"),
             ui::toggleRow("Drag Lock", deviceKey(pads, "tapDragLock", "b"), "Lifting the finger briefly doesn't end a drag"),
             ui::pickerRow("Tap Buttons", deviceKey(pads, "lmrTapButtonMap", "b"),
                           {{"Two Fingers Right, Three Middle", "false"}, {"Two Fingers Middle, Three Right", "true"}})}));
        sections.push_back(ui::section(
            "Scrolling",
            {ui::toggleRow("Two-Finger Scrolling", deviceKey(pads, "scrollTwoFinger", "b", "supportsScrollTwoFinger")),
             ui::toggleRow("Edge Scrolling", deviceKey(pads, "scrollEdge", "b", "supportsScrollEdge"), "Scroll along the right edge"),
             ui::toggleRow("Natural Scrolling", deviceKey(pads, "naturalScroll", "b", "supportsNaturalScroll"), "Scrolling moves the content, not the view"),
             ui::sliderValueRow("Scrolling Speed", deviceKey(pads, "scrollFactor", "d"), 0.2f, 3, 0.05f, scroll)}));
    }
    return ui::page(sections);
}

// Keyboard -------------------------------------------------------------------------------------------------------------

Element keyboardPanel(Context& ctx) {
    auto layouts = std::make_shared<KdeLayouts>();
    if (ctx.demo == "add") ctx.app.after(0.6, [l = layouts.get()] { l->openAdd(); });
    auto rate = [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + "/s"; };
    auto shortcuts = Button("Open Shortcut Settings…");
    shortcuts.onClick([anchor = ctx.anchor] {
        // Global shortcuts belong to kglobalaccel; Plasma's own editor handles conflicts and app actions.
        if (!runTool({"systemsettings"}, {"kcm_keys"}) && !runTool({"kcmshell6", "kcmshell5"}, {"kcm_keys"}))
            ui::message(anchor, "Plasma's System Settings isn't installed", "Shortcuts are edited with systemsettings (kcm_keys).");
    });
    return ui::page(
        {
            layouts->element(),
            ui::section("Layout Switching",
                        {ui::pickerRow("Switch Layouts", kcfg::key("kxkbrc", "Layout", "SwitchMode", "s", "Global", Reload::Keyboard),
                                       {{"For All Windows", "Global"}, {"Per Virtual Desktop", "Desktop"}, {"Per App", "WinClass"},
                                        {"Per Window", "Window"}})}),
            ui::section("Typing",
                        {ui::toggleRow("Key Repeat", kcfg::flagKey("kcminputrc", "Keyboard", "KeyRepeat", "repeat", "nothing", true, Reload::KWin),
                                       "Holding a key repeats its character"),
                         ui::sliderValueRow("Delay Until Repeat", kcfg::key("kcminputrc", "Keyboard", "RepeatDelay", "i", "600", Reload::KWin), 100,
                                            2000, 50, shared::ms),
                         ui::sliderValueRow("Repeat Rate", kcfg::key("kcminputrc", "Keyboard", "RepeatRate", "d", "25", Reload::KWin), 2, 100, 1,
                                            rate, "Characters per second"),
                         ui::pickerRow("Num Lock at Login", kcfg::key("kcminputrc", "Keyboard", "NumLock", "i", "2"),
                                       {{"On", "0"}, {"Off", "1"}, {"Leave Unchanged", "2"}})}),
            ui::section("Special Keys",
                        {optionRow("Compose Key", {"compose:"},
                                   {{"Off", ""}, {"Right Alt", "compose:ralt"}, {"Left Meta", "compose:lwin"}, {"Right Meta", "compose:rwin"},
                                    {"Menu", "compose:menu"}, {"Right Ctrl", "compose:rctrl"}, {"Caps Lock", "compose:caps"},
                                    {"Scroll Lock", "compose:sclk"}},
                                   "Type special characters as sequences, e.g. Compose, ', e → é"),
                         optionRow("Alternate Characters Key", {"lv3:"},
                                   {{"Layout Default", ""}, {"Right Alt", "lv3:ralt_switch"}, {"Left Alt", "lv3:lalt_switch"},
                                    {"Right Meta", "lv3:rwin_switch"}, {"Menu", "lv3:menu_switch"}}),
                         optionRow("Caps Lock", {"caps:", "ctrl:nocaps", "ctrl:swapcaps"},
                                   {{"Caps Lock", ""}, {"Escape", "caps:escape"}, {"Ctrl", "ctrl:nocaps"}, {"Swap with Ctrl", "ctrl:swapcaps"},
                                    {"Backspace", "caps:backspace"}, {"Disabled", "caps:none"}})}),
            ui::section("Shortcuts", {FormRow("Keyboard Shortcuts", shortcuts, "Global shortcuts for Plasma, KWin and apps")}),
        },
        layouts);
}

// Accessibility -----------------------------------------------------------------------------------------------------------

Element accessibilityPanel(Context& /*ctx*/) {
    auto access = [](const std::string& group, const std::string& key, const std::string& fallback) {
        return kcfg::key("kaccessrc", group, key, "b", fallback, Reload::KWin);
    };
    return ui::page({
        ui::section("Seeing",
                    {ui::toggleRow("Reduce Animation", kcfg::flagKey("kdeglobals", "KDE", "AnimationDurationFactor", "0", std::nullopt, false, Reload::KWin)),
                     ui::pickerRow("Cursor Size", kcfg::key("kcminputrc", "Mouse", "cursorSize", "i", "24", Reload::Cursor),
                                   {{"Default", "24"}, {"Medium", "32"}, {"Large", "48"}, {"Larger", "64"}, {"Largest", "96"}}),
                     ui::toggleRow("Shake Cursor to Find It", kwin("Plugins", "shakecursorEnabled", "b", "true"), "The pointer grows while you shake it"),
                     ui::toggleRow("Screen Reader", access("ScreenReader", "Enabled", "false"), "Reads the screen aloud (Orca)")}),
        ui::section("Zoom & Colors",
                    {ui::toggleRow("Zoom", kwin("Plugins", "zoomEnabled", "b", "true"), "Meta+= and Meta+- zoom the whole screen"),
                     ui::toggleRow("Magnifier", kwin("Plugins", "magnifierEnabled", "b", "false"), "A lens follows the pointer"),
                     ui::toggleRow("Invert Colors", kwin("Plugins", "invertEnabled", "b", "false"), "Meta+Ctrl+I inverts the screen"),
                     ui::toggleRow("Color Blindness Correction", kwin("Plugins", "colorblindnesscorrectionEnabled", "b", "false"))}),
        ui::section("Hearing",
                    {ui::toggleRow("Over-Amplification", kcfg::key("plasmaparc", "General", "RaiseMaximumVolume", "b", "false")),
                     ui::toggleRow("Visual Bell", access("Bell", "VisibleBell", "false"), "Flash the screen for the system bell"),
                     ui::toggleRow("Audible Bell", access("Bell", "SystemBell", "true"))}),
        ui::section("Typing",
                    {ui::toggleRow("Sticky Keys", access("Keyboard", "StickyKeys", "false"), "Press modifier keys one after another"),
                     ui::toggleRow("Slow Keys", access("Keyboard", "SlowKeys", "false"), "Keys must be held before they register"),
                     ui::toggleRow("Bounce Keys", access("Keyboard", "BounceKeys", "false"), "Ignore fast repeated presses of the same key"),
                     ui::toggleRow("Enable by Keyboard", access("Keyboard", "Gestures", "false"),
                                   "Turn these features on and off with keyboard gestures")}),
        ui::section("Pointing & Clicking",
                    {ui::toggleRow("Mouse Keys", access("Mouse", "MouseKeys", "false"), "Control the pointer with the keypad"),
                     ui::sliderValueRow("Double-Click Delay", kcfg::key("kdeglobals", "KDE", "DoubleClickInterval", "i", "400", Reload::Mouse), 100,
                                        1000, 50, shared::ms)}),
    });
}

// Privacy & Security --------------------------------------------------------------------------------------------------------

Element privacyPanel(Context& ctx) {
    auto locker = [](const std::string& key, const std::string& type, const std::string& fallback) {
        return kcfg::key("kscreenlockerrc", "Daemon", key, type, fallback, Reload::ScreenLocker);
    };
    auto clearHistory = Button("Clear History…");
    clearHistory.onClick([anchor = ui::Anchor(clearHistory)] {
        ui::confirm(anchor, "Clear file history?", "The list of recently used files is emptied for every app.", "Clear History", [] {
            if (gx::readOnly()) return;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(fs::path(g_get_user_data_dir()) / "RecentDocuments", ec)) fs::remove(e.path(), ec);
            std::ofstream out(fs::path(g_get_user_data_dir()) / "recently-used.xbel", std::ios::trunc);
            out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xbel version=\"1.0\"\n"
                   "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
                   "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n</xbel>\n";
        });
    });
    auto emptyTrash = Button("Empty Trash…");
    emptyTrash.onClick([anchor = ui::Anchor(emptyTrash), appAnchor = ctx.anchor] {
        ui::confirm(anchor, "Empty the Trash?", "All items in the Trash are permanently deleted.", "Empty Trash", [appAnchor] {
            gx::run({"gio", "trash", "--empty"}, [appAnchor](const gx::ProcessResult& r) {
                if (!r.ok()) ui::message(appAnchor, "Couldn't empty the Trash", r.err);
            });
        });
    });
    return ui::page({
        ui::section("Screen Lock",
                    {ui::toggleRow("Lock Automatically", locker("Autolock", "b", "true"), "After the screen has been idle"),
                     ui::pickerRow("Lock After", locker("Timeout", "i", "5"),
                                   {{"1 minute", "1"}, {"2 minutes", "2"}, {"3 minutes", "3"}, {"5 minutes", "5"}, {"10 minutes", "10"},
                                    {"15 minutes", "15"}, {"30 minutes", "30"}, {"1 hour", "60"}}),
                     ui::toggleRow("Lock After Waking From Sleep", locker("LockOnResume", "b", "true")),
                     ui::pickerRow("Unlock Without Password For", locker("LockGrace", "i", "5"),
                                   {{"No Grace Period", "0"}, {"5 seconds", "5"}, {"30 seconds", "30"}, {"1 minute", "60"}, {"2 minutes", "120"}},
                                   "Right after the screen locks by itself")}),
        ui::section("File History",
                    {ui::toggleRow("Remember Recent Files", kcfg::key("kdeglobals", "RecentDocuments", "UseRecent", "b", "true")),
                     ui::pickerRow("Activity Tracking",
                                   kcfg::key("kactivitymanagerd-pluginsrc", "Plugin-org.kde.ActivityManager.Resources.Scoring", "what-to-remember", "i",
                                             "0"),
                                   {{"Remember All Apps", "0"}, {"Remember Nothing", "2"}}, "Recent documents and apps in launchers"),
                     FormRow(HStack({Spacer(), clearHistory}))}),
        ui::section("Trash", {FormRow(HStack({Spacer(), emptyTrash}))}),
    });
}

Element removableMediaSection() {
    constexpr const char* kRc = "kded_device_automounterrc";
    return ui::section("Removable Media",
                       {ui::toggleRow("Mount Drives When Plugged In", kcfg::key(kRc, "General", "AutomountOnPlugin", "b", "false")),
                        ui::toggleRow("Mount Drives at Login", kcfg::key(kRc, "General", "AutomountOnLogin", "b", "false"),
                                      "Drives that were mounted before")});
}

// Power (PowerDevil) ------------------------------------------------------------------------------------------------------------

std::vector<Element> powerSections() {
    constexpr const char* kRc = "powerdevilrc";
    auto profile = [&](const std::string& p, int dim, int off, int suspend) {
        auto key = [&](const std::string& sub, const std::string& k, const std::string& type, const std::string& fallback) {
            return kcfg::key(kRc, p + "][" + sub, k, type, fallback, Reload::PowerDevil);
        };
        const std::vector<int> short_{30, 60, 120, 180, 300, 600, 900, 1800};
        const std::vector<int> long_{60, 120, 300, 600, 900, 1200, 1800, 2700, 3600, 7200};
        return std::vector<Element>{
            timeoutRow("Dim Screen", key("Display", "DimDisplayWhenIdle", "b", "true"), key("Display", "DimDisplayIdleTimeoutSec", "i", std::to_string(dim)),
                       short_),
            timeoutRow("Turn Off Screen", key("Display", "TurnOffDisplayWhenIdle", "b", "true"),
                       key("Display", "TurnOffDisplayIdleTimeoutSec", "i", std::to_string(off)), short_),
            timeoutRow("Suspend", kcfg::flagKey(kRc, p + "][SuspendAndShutdown", "AutoSuspendAction", "1", "0", true, Reload::PowerDevil),
                       key("SuspendAndShutdown", "AutoSuspendIdleTimeoutSec", "i", std::to_string(suspend)), long_, "After this much inactivity"),
        };
    };
    std::vector<Element> out{ui::section("When Plugged In", profile("AC", 300, 600, 900))};
    std::error_code ec;
    bool battery = false;
    for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) battery = battery || e.path().filename().string().rfind("BAT", 0) == 0;
    if (battery) out.push_back(ui::section("On Battery", profile("Battery", 120, 300, 600)));

    const std::vector<std::string> all{"AC][SuspendAndShutdown", "Battery][SuspendAndShutdown", "LowBattery][SuspendAndShutdown"};
    const std::vector<ui::Option> actions{{"Sleep", "1"}, {"Hibernate", "2"}, {"Shut Down", "8"}, {"Lock Screen", "32"},
                                          {"Show Logout Screen", "16"}, {"Turn Off Screen", "64"}, {"Do Nothing", "0"}};
    std::vector<Element> buttons{ui::pickerRow("Power Button", kcfg::multiKey(kRc, all, "PowerButtonAction", "i", "16", Reload::PowerDevil), actions)};
    if (fs::exists("/proc/acpi/button/lid", ec) && battery)
        buttons.push_back(ui::pickerRow("Closing the Lid", kcfg::multiKey(kRc, all, "LidAction", "i", "1", Reload::PowerDevil), actions));
    out.push_back(ui::section("Buttons", buttons));
    return out;
}

// Displays: Night Light ------------------------------------------------------------------------------------------------------------

Element nightLightSection() {
    std::vector<ui::Option> hours;
    for (int h = 0; h < 48; ++h) {
        char label[16];
        std::snprintf(label, sizeof label, "%02d:%02d", h / 2, (h % 2) * 30);
        char value[16];
        std::snprintf(value, sizeof value, "%02d%02d", h / 2, (h % 2) * 30);
        hours.push_back({label, value});
    }
    gx::Key mode = kwin("NightColor", "Mode", "s", "Automatic");
    auto from = ui::pickerRow("From", kwin("NightColor", "EveningBeginFixed", "s", "1800"), hours);
    auto to = ui::pickerRow("To", kwin("NightColor", "MorningBeginFixed", "s", "0600"), hours);
    auto sync = [mode, from, to] {
        from.node()->setDisabled(mode.getString() != "Times");
        to.node()->setDisabled(mode.getString() != "Times");
    };
    sync();
    mode.watch(sync);
    return ui::section("Night Light",
                       {ui::toggleRow("Night Light", kwin("NightColor", "Active", "b", "false"), "Makes the screen warmer to reduce eye strain at night"),
                        ui::pickerRow("Schedule", mode, {{"Sunset to Sunrise", "Automatic"}, {"Custom Times", "Times"}, {"Always On", "Constant"}}),
                        from, to,
                        ui::sliderRow("Color Temperature", kwin("NightColor", "NightTemperature", "i", "4500"), 1000, 6500, 100, "Left is warmer")});
}

// Network: proxy --------------------------------------------------------------------------------------------------------------------

Element proxySection() {
    constexpr const char* kRc = "kioslaverc";
    constexpr const char* kGroup = "Proxy Settings";
    gx::Key mode = kcfg::key(kRc, kGroup, "ProxyType", "i", "0", Reload::Kio);
    auto text = [&](const std::string& title, const std::string& key, const std::string& placeholder) {
        return ui::textRow(title, kcfg::key(kRc, kGroup, key, "s", "", Reload::Kio), placeholder, 300);
    };
    auto manual = ui::section("Manual Proxy", {text("HTTP Proxy", "httpProxy", "http://proxy.example.com 8080"),
                                               text("HTTPS Proxy", "httpsProxy", "http://proxy.example.com 8080"),
                                               text("FTP Proxy", "ftpProxy", "http://proxy.example.com 8080"),
                                               text("SOCKS Proxy", "socksProxy", "socks://proxy.example.com 1080"),
                                               text("No Proxy For", "NoProxyFor", "localhost, 127.0.0.0/8")},
                              "Host and port separated by a space, like Plasma writes them.");
    auto automatic = ui::section("Automatic Proxy", {text("Configuration URL", "Proxy Config Script", "http://example.com/proxy.pac")});
    auto sync = [mode, manual, automatic] {
        manual.widget()->setHidden(mode.getText() != "1");
        automatic.widget()->setHidden(mode.getText() != "2");
    };
    sync();
    mode.watch(sync);
    return VStack({ui::section("Proxy", {ui::pickerRow("Network Proxy", mode,
                                                       {{"Off", "0"}, {"Manual", "1"}, {"Automatic (PAC File)", "2"},
                                                        {"Detect Automatically", "3"}, {"From Environment Variables", "4"}},
                                                       "Used by KDE apps; others read their own settings")}),
                   manual, automatic})
        .spacing(22);
}

// Sound ------------------------------------------------------------------------------------------------------------------------------

Element soundsSection() {
    return ui::section("Sounds", {ui::toggleRow("Volume Feedback", kcfg::key("plasmaparc", "General", "AudioFeedback", "b", "true"),
                                                "Play a sound when the volume changes"),
                                  ui::toggleRow("Over-Amplification", kcfg::key("plasmaparc", "General", "RaiseMaximumVolume", "b", "false"),
                                                "Allows raising the volume above 100%; may distort the sound")});
}

std::string plasmaVersion() {
    static const std::string version = [] {
        if (!gx::haveProgram("plasmashell")) return std::string();
        auto r = gx::runSync({"plasmashell", "--version"});
        auto parts = ui::split(ui::trim(r.out), ' ');
        return r.ok() && parts.size() >= 2 ? parts.back() : std::string();
    }();
    return version;
}

}  // namespace kde
