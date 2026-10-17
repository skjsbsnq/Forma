// Appearance, Multitasking, Notifications, Search and Apps: almost entirely GSettings, plus GIO's
// application registry for default apps.
#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "glib_support.hpp"
#include "kconfig.hpp"
#include "panels.hpp"
#include "panels_shared.hpp"
#include "ui.hpp"

using namespace mayo;
namespace fs = std::filesystem;

namespace {

constexpr const char* kInterface = "org.gnome.desktop.interface";
constexpr const char* kBackground = "org.gnome.desktop.background";
constexpr const char* kMutter = "org.gnome.mutter";
constexpr const char* kWm = "org.gnome.desktop.wm.preferences";

std::string fileUri(const std::string& path) {
    char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    std::string s = uri ? uri : "";
    g_free(uri);
    return s;
}

std::string extension(const std::string& path) { return ui::lower(fs::path(path).extension().string()); }

bool isImage(const std::string& path) {
    static const std::set<std::string> kExt = {".png", ".jpg", ".jpeg", ".webp", ".jxl", ".svg", ".bmp", ".gif", ".tif", ".tiff", ".avif", ".heic"};
    return kExt.contains(extension(path));
}

std::vector<std::string> dataDirs() {
    std::vector<std::string> dirs{g_get_user_data_dir()};
    for (const char* const* d = g_get_system_data_dirs(); *d; ++d) dirs.emplace_back(*d);
    return dirs;
}

// Appearance: style previews -------------------------------------------------------------------------

/// A miniature desktop in the given appearance: wallpaper wash, a window with a sidebar and content.
void drawStylePreview(Painter& p, const Rect& r, bool dark, Color accent) {
    const Theme t = Theme::make(dark ? Appearance::Dark : Appearance::Light, accent);
    const auto& c = t.colors;
    p.pushClip(r, 8);
    p.fillVerticalGradient(r, 8, accent.mix(dark ? Color::black() : Color::white(), dark ? 0.45f : 0.35f),
                           accent.mix(dark ? Color::black() : Color::white(), dark ? 0.7f : 0.1f));
    const Rect w{r.x + r.width * 0.12f, r.y + r.height * 0.16f, r.width * 0.76f, r.height * 0.68f};
    p.drawShadow(w, 5, Color::black(0.25f), 6, {0, 2});
    p.fillRoundedRect(w, 5, c.windowBackground, c.windowBorder, 0.5f);
    const Rect side{w.x + 3, w.y + 3, w.width * 0.3f, w.height - 6};
    p.fillRoundedRect(side, 3.5f, c.secondaryFill);
    const Color lights[] = {c.red, c.yellow, c.green};
    for (int i = 0; i < 3; ++i) p.fillCircle({side.x + 5 + static_cast<float>(i) * 5.5f, side.y + 5}, 1.8f, lights[i]);
    for (int i = 0; i < 3; ++i) {
        p.fillRoundedRect({side.x + 4, side.y + 13 + static_cast<float>(i) * 7, side.width - 8, 4}, 2,
                          i == 0 ? c.accent : c.fill);
    }
    const float cx = side.right() + 6;
    p.fillRoundedRect({cx, w.y + 8, w.right() - cx - 8, 5}, 2.5f, c.fill);
    p.fillRoundedRect({cx, w.y + 17, (w.right() - cx - 8) * 0.7f, 5}, 2.5f, c.fill);
    p.fillRoundedRect({w.right() - 28, w.bottom() - 14, 20, 8}, 4, c.accent);
    p.popClip();
    p.strokeRoundedRect(r, 8, c.separator, 1);
}

struct NamedColor {
    const char* id;
    const char* name;
    Color color;
};

// GNOME's accent palette (libadwaita); these are the values the desktop renders, not UI colours of ours.
const NamedColor kGnomeAccents[] = {
    {"blue", "Blue", Color::hex(0x3584e4)},     {"teal", "Teal", Color::hex(0x2190a4)},
    {"green", "Green", Color::hex(0x3a944a)},   {"yellow", "Yellow", Color::hex(0xc88800)},
    {"orange", "Orange", Color::hex(0xed5b00)}, {"red", "Red", Color::hex(0xe62d42)},
    {"pink", "Pink", Color::hex(0xd56199)},     {"purple", "Purple", Color::hex(0x9141ac)},
    {"slate", "Slate", Color::hex(0x6f8396)},
};

Color gnomeAccent(const gx::Key& key) {
    const std::string id = key.getString("blue");
    for (const auto& a : kGnomeAccents) {
        if (id == a.id) return a.color;
    }
    return kGnomeAccents[0].color;
}

Element styleSection() {
    gx::Key scheme(kInterface, "color-scheme");
    gx::Key accent(kInterface, "accent-color");

    auto styles = ui::ChoiceGrid(2, {150, 118}, 18, [accent](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
        const auto& c = theme().colors;
        const Rect preview{cell.x, cell.y, cell.width, cell.height - 22};
        drawStylePreview(p, preview, i == 1, accent ? gnomeAccent(accent) : c.accent);
        if (selected) p.strokeRoundedRect(preview.outset(3.5f), 11, c.accent, 3);
        else if (hovered) p.strokeRoundedRect(preview.outset(3.5f), 11, c.secondaryFill, 3);
        const Font f = theme().font(TextStyle::Subheadline).withWeight(selected ? FontWeight::Semibold : FontWeight::Regular);
        p.drawText(mayo::uiText(i == 0 ? "Default" : "Dark"), {cell.x, preview.bottom() + 5, cell.width, 16}, f,
                   selected ? c.label : c.secondaryLabel, TextAlign::Center);
    });
    auto syncStyle = [scheme, styles] { styles->setSelected(scheme.getString() == "prefer-dark" ? 1 : 0); };
    syncStyle();
    styles.onChoose([scheme](int i) { scheme.setString(i == 1 ? "prefer-dark" : "default"); });
    scheme.watch(syncStyle);
    if (!scheme) styles.disabled();

    std::vector<Element> rows{FormRow(HStack({Spacer(), styles, Spacer()}))};
    if (accent) {
        auto dots = ui::ChoiceGrid(9, {22, 22}, 12, [](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
            const Point ctr = cell.center();
            const Color col = kGnomeAccents[i].color;
            p.drawShadow(cell, 11, Color::black(0.15f), 2, {0, 0.5f});
            p.fillCircle(ctr, 11, col);
            p.strokeCircle(ctr, 11, Color::black(0.12f), 0.5f);
            if (selected) p.fillCircle(ctr, 3, Color::white());
            else if (hovered) p.strokeCircle(ctr, 13.5f, theme().colors.secondaryFill, 2);
        });
        dots.caption([](int i) { return std::string(kGnomeAccents[i].name); });
        auto syncAccent = [accent, dots, styles] {
            const std::string id = accent.getString();
            for (int i = 0; i < 9; ++i) {
                if (id == kGnomeAccents[i].id) dots->setSelected(i);
            }
            styles->invalidate();  // the previews use the accent
        };
        syncAccent();
        dots.onChoose([accent](int i) { accent.setString(kGnomeAccents[i].id); });
        accent.watch(syncAccent);
        rows.push_back(FormRow("Accent Color", dots));
    }
    return ui::section("Style", rows);
}

// Appearance: wallpapers ---------------------------------------------------------------------------------

struct Wallpaper {
    std::string name;
    std::string light;
    std::string dark;
    std::string options = "zoom";
    Color fallback = Color::hex(0x3071AE);
    std::shared_ptr<ImageData> thumb;
};

std::string xmlText(const std::string& block, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    size_t a = block.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = block.find("</" + tag + ">", a);
    if (b == std::string::npos) return {};
    std::string s = block.substr(a, b - a);
    for (auto [from, to] : {std::pair{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}}) {
        for (size_t i; (i = s.find(from)) != std::string::npos;) s.replace(i, std::string(from).size(), to);
    }
    return ui::trim(s);
}

std::vector<Wallpaper> findWallpapers() {
    std::vector<Wallpaper> out;
    std::set<std::string> seen;
    for (const auto& dir : dataDirs()) {
        std::error_code ec;
        const fs::path props = fs::path(dir) / "gnome-background-properties";
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(props, ec)) {
            if (e.path().extension() == ".xml") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::ifstream in(f);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string xml = ss.str();
            for (size_t pos = 0; (pos = xml.find("<wallpaper", pos)) != std::string::npos;) {
                size_t end = xml.find("</wallpaper>", pos);
                if (end == std::string::npos) break;
                const std::string block = xml.substr(pos, end - pos);
                pos = end;
                if (block.find("deleted=\"true\"") != std::string::npos) continue;
                Wallpaper w;
                w.name = xmlText(block, "name");
                w.light = xmlText(block, "filename");
                w.dark = xmlText(block, "filename-dark");
                if (auto o = xmlText(block, "options"); !o.empty()) w.options = o;
                if (auto c = Color::parse(xmlText(block, "pcolor"))) w.fallback = *c;
                if (w.light.empty() || !fs::exists(w.light, ec) || !seen.insert(w.light).second) continue;
                if (!w.dark.empty() && !fs::exists(w.dark, ec)) w.dark.clear();
                out.push_back(std::move(w));
            }
        }
    }
    // Pictures the user added (GNOME Settings copies them here too).
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(g_get_user_data_dir()) / "backgrounds", ec)) {
        const std::string path = e.path().string();
        if (!isImage(path) || !seen.insert(path).second) continue;
        Wallpaper w;
        w.name = e.path().stem().string();
        w.light = path;
        out.push_back(std::move(w));
    }
    return out;
}

/// Small preview of an image. GNOME's own thumbnail cache first (it has decoded the JPEG XL wallpapers
/// already), then stb for the formats it knows, then ImageMagick into our cache. Worker thread only.
std::shared_ptr<ImageData> makeThumbnail(const std::string& path) {
    const std::string uri = fileUri(path);
    if (uri.empty()) return nullptr;
    char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), -1);
    const std::string hash = md5;
    g_free(md5);
    const std::string cache = g_get_user_cache_dir();
    std::error_code ec;
    for (const char* size : {"large", "x-large", "normal"}) {
        const std::string t = cache + "/thumbnails/" + size + "/" + hash + ".png";
        if (fs::exists(t, ec)) {
            if (auto img = ImageData::load(t, 320)) return img;
        }
    }
    static const std::set<std::string> kStb = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga"};
    if (kStb.contains(extension(path))) return ImageData::load(path, 320);
    const fs::path own = fs::path(cache) / "mayonnaise" / "system-settings" / "thumbnails" / (hash + ".png");
    if (!fs::exists(own, ec)) {
        const char* tool = gx::haveProgram("magick") ? "magick" : gx::haveProgram("convert") ? "convert" : nullptr;
        if (!tool) return nullptr;
        fs::create_directories(own.parent_path(), ec);
        if (!gx::runSync({tool, path + "[0]", "-thumbnail", "320x320", own.string()}).ok()) return nullptr;
    }
    return ImageData::load(own.string(), 320);
}

/// Draws `img` covering `r` (aspect fill), rounded.
void drawCover(Painter& p, const Rect& r, const ImageData& img, float radius) {
    const float ia = static_cast<float>(img.width()) / static_cast<float>(std::max(1, img.height()));
    const float ra = r.width / r.height;
    Rect src{0, 0, 1, 1};
    if (ia > ra) {
        src.width = ra / ia;
        src.x = (1 - src.width) * 0.5f;
    } else {
        src.height = ia / ra;
        src.y = (1 - src.height) * 0.5f;
    }
    p.drawImage(r, img, radius, 1, src);
}

Element backgroundSection(Context& ctx) {
    gx::Key uri(kBackground, "picture-uri");
    gx::Key uriDark(kBackground, "picture-uri-dark");
    gx::Key options(kBackground, "picture-options");
    if (!uri) return Element();

    auto walls = std::make_shared<std::vector<Wallpaper>>(findWallpapers());
    auto grid = ui::ChoiceGrid(static_cast<int>(walls->size()), {140, 88}, 12,
                               [walls](Painter& p, const Rect& cell, int i, bool selected, bool hovered) {
                                   const auto& c = theme().colors;
                                   const Wallpaper& w = (*walls)[static_cast<size_t>(i)];
                                   if (w.thumb) {
                                       drawCover(p, cell, *w.thumb, 8);
                                   } else {
                                       p.fillVerticalGradient(cell, 8, w.fallback.mix(Color::white(), 0.2f), w.fallback);
                                   }
                                   p.strokeRoundedRect(cell, 8, Color::black(0.1f), 0.5f);
                                   if (selected) p.strokeRoundedRect(cell.outset(3.5f), 11, c.accent, 3);
                                   else if (hovered) p.strokeRoundedRect(cell.outset(3.5f), 11, c.secondaryFill, 3);
                               });
    grid.wraps().caption([walls](int i) { return (*walls)[static_cast<size_t>(i)].name; });

    auto sync = [walls, grid, uri, uriDark] {
        const std::string light = uri.getString();
        const std::string dark = uriDark.getString();
        int sel = -1;
        for (size_t i = 0; i < walls->size(); ++i) {
            const auto& w = (*walls)[i];
            if (fileUri(w.light) == light || (!w.dark.empty() && fileUri(w.dark) == dark)) sel = static_cast<int>(i);
        }
        grid->setSelected(sel);
    };
    auto loadThumbs = [walls, grid](size_t from) {
        for (size_t i = from; i < walls->size(); ++i) {
            gx::background([walls, grid, i, path = (*walls)[i].light] {
                auto img = makeThumbnail(path);
                if (!img) return;
                gx::toUi([walls, grid, i, img] {
                    if (i < walls->size()) (*walls)[i].thumb = img;
                    grid->invalidate();
                });
            });
        }
    };
    auto apply = [uri, uriDark, options](const Wallpaper& w) {
        uri.setString(fileUri(w.light));
        if (uriDark) uriDark.setString(fileUri(w.dark.empty() ? w.light : w.dark));
        if (options) options.setString(w.options);
    };
    sync();
    loadThumbs(0);
    grid.onChoose([walls, apply](int i) { apply((*walls)[static_cast<size_t>(i)]); });
    uri.watch(sync);
    uriDark.watch(sync);

    auto add = Button(Icon::Plus, "Add Picture…");
    add.onClick([&app = ctx.app, &window = ctx.window, walls, grid, apply, loadThumbs, sync] {
        app.openFiles({{"Images", "png;jpg;jpeg;webp;jxl;svg;avif"}}, true,
                      [walls, grid, apply, loadThumbs, sync](std::vector<std::string> paths) mutable {
                          if (paths.empty()) return;
                          const size_t first = walls->size();
                          const fs::path dir = fs::path(g_get_user_data_dir()) / "backgrounds";
                          for (const auto& p : paths) {
                              // Copied like GNOME Settings does, so the wallpaper survives the original moving.
                              std::string target = p;
                              if (!gx::readOnly()) {
                                  std::error_code ec;
                                  fs::create_directories(dir, ec);
                                  const fs::path t = dir / fs::path(p).filename();
                                  if (fs::exists(t, ec) || fs::copy_file(p, t, ec)) target = t.string();
                              }
                              Wallpaper w;
                              w.name = fs::path(p).stem().string();
                              w.light = target;
                              walls->push_back(std::move(w));
                          }
                          grid->setCount(static_cast<int>(walls->size()));
                          loadThumbs(first);
                          apply(walls->back());
                          sync();
                      },
                      &window);
    });

    auto picker = ui::pickerRow("Picture Position", options,
                                {{"Zoom", "zoom"}, {"Centered", "centered"}, {"Scaled", "scaled"},
                                 {"Stretched", "stretched"}, {"Spanned", "spanned"}, {"Wallpaper", "wallpaper"}});
    return ui::section("Background", {FormRow(grid).padding(Insets(10, 8)), picker, FormRow(HStack({Spacer(), add}))});
}

// Appearance: fonts and themes ------------------------------------------------------------------------------

std::vector<std::string> fontFamilies() {
    static std::vector<std::string> cached;
    if (!cached.empty()) return cached;
    auto r = gx::runSync({"fc-list", ":", "family"});
    std::set<std::string> fams;
    for (const auto& line : ui::split(r.out, '\n')) {
        const std::string f = ui::trim(line.substr(0, line.find(',')));
        if (!f.empty() && f[0] != '.') fams.insert(f);
    }
    cached.assign(fams.begin(), fams.end());
    return cached;
}

/// "Adwaita Sans 11" -> {"Adwaita Sans", 11}.
std::pair<std::string, int> splitFont(const std::string& desc) {
    const size_t sp = desc.find_last_of(' ');
    if (sp != std::string::npos) {
        const std::string tail = desc.substr(sp + 1);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; }))
            return {desc.substr(0, sp), static_cast<int>(std::atof(tail.c_str()))};
    }
    return {desc, 11};
}

FormRow fontRow(const std::string& title, const gx::Key& key) {
    auto button = Button("");
    std::vector<std::string> sizes;
    for (int s = 6; s <= 24; ++s) sizes.push_back(std::to_string(s));
    auto size = Picker(sizes, 0);
    auto sync = [key, button, size]() mutable {
        auto [family, pt] = splitFont(key.getString());
        button.setTitle(family);
        size->setSelected(std::clamp(pt, 6, 24) - 6);
    };
    sync();
    button.onClick([key, anchor = ui::Anchor(button)] {
        std::vector<ui::Option> items;
        for (const auto& f : fontFamilies()) items.push_back({f, f});
        ui::chooseFromList(anchor, items, [key](const std::string& family) {
            key.setString(family + " " + std::to_string(splitFont(key.getString()).second));
        }, "Search Fonts");
    });
    size.onChange([key](int i) { key.setString(splitFont(key.getString()).first + " " + std::to_string(i + 6)); });
    key.watch(sync);
    if (!key) {
        button.disabled();
        size.disabled();
    }
    return FormRow(title, HStack({button, size}).spacing(6).align(Alignment::Center));
}

/// Theme directories under every icons/ or themes/ dir that satisfy `accept`.
std::vector<ui::Option> themes(const char* sub, const std::function<bool(const fs::path&)>& accept) {
    std::set<std::string> names;
    std::vector<fs::path> roots;
    for (const auto& d : dataDirs()) roots.push_back(fs::path(d) / sub);
    if (std::string(sub) == "icons") roots.push_back(fs::path(g_get_home_dir()) / ".icons");
    if (std::string(sub) == "themes") roots.push_back(fs::path(g_get_home_dir()) / ".themes");
    for (const auto& root : roots) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (e.is_directory(ec) && accept(e.path())) names.insert(e.path().filename().string());
        }
    }
    std::vector<ui::Option> out;
    for (const auto& n : names) out.push_back({n, n});
    return out;
}

Element fontsSection() {
    auto scale = [](float v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2f×", static_cast<double>(v));
        return std::string(buf);
    };
    return ui::section("Fonts", {
        fontRow("Interface Text", gx::Key(kInterface, "font-name")),
        fontRow("Document Text", gx::Key(kInterface, "document-font-name")),
        fontRow("Monospace Text", gx::Key(kInterface, "monospace-font-name")),
        ui::sliderValueRow("Scaling Factor", gx::Key(kInterface, "text-scaling-factor"), 0.5f, 3.0f, 0.05f, scale),
        ui::pickerRow("Hinting", gx::Key(kInterface, "font-hinting"),
                      {{"Full", "full"}, {"Medium", "medium"}, {"Slight", "slight"}, {"None", "none"}}),
        ui::pickerRow("Antialiasing", gx::Key(kInterface, "font-antialiasing"),
                      {{"Standard (Grayscale)", "grayscale"}, {"Subpixel (LCD screens)", "rgba"}, {"None", "none"}}),
    });
}

Element themesSection() {
    std::error_code ec;
    auto cursors = themes("icons", [&](const fs::path& p) { return fs::exists(p / "cursors", ec); });
    auto icons = themes("icons", [&](const fs::path& p) {
        return fs::exists(p / "index.theme", ec) && !fs::exists(p / "cursors", ec) && p.filename() != "default";
    });
    auto legacy = themes("themes", [&](const fs::path& p) { return fs::exists(p / "gtk-3.0", ec); });
    std::vector<Element> rows{
        ui::pickerRow("Cursor", gx::Key(kInterface, "cursor-theme"), cursors),
        ui::pickerRow("Cursor Size", gx::Key(kInterface, "cursor-size"),
                      {{"Default (24)", "24"}, {"Medium (32)", "32"}, {"Large (48)", "48"}, {"Larger (64)", "64"},
                       {"Largest (96)", "96"}}),
        ui::pickerRow("Icons", gx::Key(kInterface, "icon-theme"), icons),
    };
    // Adwaita is built into GTK 3 and has no directory; offer it anyway.
    if (std::none_of(legacy.begin(), legacy.end(), [](const ui::Option& o) { return o.value == "Adwaita"; }))
        legacy.insert(legacy.begin(), {"Adwaita", "Adwaita"});
    rows.push_back(ui::pickerRow("Legacy Applications", gx::Key(kInterface, "gtk-theme"), legacy,
                                 "GTK 3 theme; GTK 4 apps always use Adwaita"));
    return ui::section("Themes", rows);
}

}  // namespace

namespace shared {
void stylePreview(Painter& p, const Rect& r, bool dark, Color accent) { drawStylePreview(p, r, dark, accent); }
std::shared_ptr<ImageData> thumbnail(const std::string& path) { return makeThumbnail(path); }
void cover(Painter& p, const Rect& r, const ImageData& img, float radius) { drawCover(p, r, img, radius); }
std::vector<std::string> fontFamilies() { return ::fontFamilies(); }
std::vector<ui::Option> themeDirs(const char* sub, const std::function<bool(const fs::path&)>& accept) { return themes(sub, accept); }
std::vector<std::string> dataDirs() { return ::dataDirs(); }
std::string fileUri(const std::string& path) { return ::fileUri(path); }
bool isImage(const std::string& path) { return ::isImage(path); }
}  // namespace shared

Element appearancePanel(Context& ctx) {
    if (desk::kde()) return kde::appearancePanel(ctx);
    if (ctx.demo == "fonts") {
        ctx.app.after(0.6, [anchor = ctx.anchor] {
            std::vector<ui::Option> items;
            for (const auto& f : fontFamilies()) items.push_back({f, f});
            ui::chooseFromList(anchor, items, [](const std::string&) {}, "Search Fonts");
        });
    }
    std::vector<Element> sections{styleSection()};
    if (auto bg = backgroundSection(ctx)) sections.push_back(bg);
    sections.push_back(fontsSection());
    sections.push_back(themesSection());
    return ui::page(sections);
}

// Multitasking ---------------------------------------------------------------------------------------------

Element multitaskingPanel(Context& ctx) {
    if (desk::kde()) return kde::multitaskingPanel(ctx);
    gx::Key dynamic(kMutter, "dynamic-workspaces");
    gx::Key count(kWm, "num-workspaces");
    std::vector<std::string> numbers;
    for (int i = 1; i <= 16; ++i) numbers.push_back(std::to_string(i));
    auto countPicker = Picker(numbers, static_cast<int>(std::clamp<std::int64_t>(count.getInt(4), 1, 16)) - 1);
    countPicker.onChange([count](int i) { count.setInt(i + 1); });
    auto syncCount = [count, countPicker, dynamic]() mutable {
        countPicker->setSelected(static_cast<int>(std::clamp<std::int64_t>(count.getInt(4), 1, 16)) - 1);
        countPicker.setDisabled(!count || dynamic.getBool());
    };
    syncCount();
    count.watch(syncCount);
    dynamic.watch(syncCount);

    return ui::page({
        ui::section("General",
                    {ui::toggleRow("Hot Corner", gx::Key(kInterface, "enable-hot-corners"),
                                   "Touch the top-left corner to open the Activities Overview"),
                     ui::toggleRow("Active Screen Edges", gx::Key(kMutter, "edge-tiling"),
                                   "Drag windows against the top, left, and right screen edges to resize them")}),
        ui::section("Workspaces",
                    {ui::pickerRow("Workspaces", dynamic, {{"Dynamic", "true"}, {"Fixed Number", "false"}},
                                   "Dynamic workspaces are removed when empty"),
                     FormRow("Number of Workspaces", countPicker),
                     ui::pickerRow("Multi-Monitor", gx::Key(kMutter, "workspaces-only-on-primary"),
                                   {{"Primary Display Only", "true"}, {"All Displays", "false"}},
                                   "Where workspaces switch")}),
        ui::section("App Switching",
                    {ui::pickerRow("Switch Between", gx::Key("org.gnome.shell.app-switcher", "current-workspace-only"),
                                   {{"Apps on All Workspaces", "false"}, {"Apps on the Current Workspace", "true"}})}),
        ui::section("Windows",
                    {ui::pickerRow("Titlebar Buttons", gx::Key(kWm, "button-layout"),
                                   {{"Close", "appmenu:close"},
                                    {"Minimize, Close", "appmenu:minimize,close"},
                                    {"Minimize, Maximize, Close", "appmenu:minimize,maximize,close"},
                                    {"Close on the Left", "close:appmenu"},
                                    {"All on the Left", "close,minimize,maximize:appmenu"}}),
                     ui::pickerRow("Double-Click Titlebar", gx::Key(kWm, "action-double-click-titlebar"),
                                   {{"Maximize", "toggle-maximize"}, {"Minimize", "minimize"}, {"Lower", "lower"},
                                    {"Menu", "menu"}, {"Nothing", "none"}}),
                     ui::pickerRow("Window Focus", gx::Key(kWm, "focus-mode"),
                                   {{"Click to Focus", "click"}, {"Focus on Hover", "sloppy"},
                                    {"Focus Follows Mouse", "mouse"}}),
                     ui::toggleRow("Center New Windows", gx::Key(kMutter, "center-new-windows")),
                     ui::toggleRow("Attach Modal Dialogs", gx::Key(kMutter, "attach-modal-dialogs"),
                                   "Dialogs move with their parent window")}),
    });
}

// Notifications --------------------------------------------------------------------------------------------

namespace {

constexpr const char* kNotifications = "org.gnome.desktop.notifications";
constexpr const char* kAppNotifications = "org.gnome.desktop.notifications.application";

std::string appPath(const std::string& child) { return "/org/gnome/desktop/notifications/application/" + child + "/"; }

}  // namespace

Element notificationsPanel(Context& ctx) {
    if (desk::kde()) return kde::notificationsPanel(ctx);
    gx::Key children(kNotifications, "application-children");
    auto apps = std::make_shared<ui::DynamicSection>("App Notifications", "No apps have sent notifications yet.");
    auto rebuild = [children, apps] {
        struct Entry {
            std::string name;
            std::string child;
        };
        std::vector<Entry> entries;
        for (const auto& child : children.getStrv()) {
            gx::Key id(kAppNotifications, "application-id", appPath(child));
            std::string name = gx::appName(id.getString());
            if (name.empty()) continue;  // uninstalled apps
            entries.push_back({name, child});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return ui::lower(a.name) < ui::lower(b.name); });
        std::vector<Element> rows;
        for (const auto& e : entries) {
            const std::string path = appPath(e.child);
            gx::Key enable(kAppNotifications, "enable", path);
            auto toggle = Toggle("", enable.getBool(true)).onChange([enable](bool on) { enable.set(on); });
            enable.watch([enable, toggle]() mutable { toggle.setOn(enable.getBool(true)); });
            auto row = ui::ClickRow({ui::letterTile(e.name), Label(e.name), Spacer(), toggle});
            row.onClick([anchor = ui::Anchor(row), path, name = e.name] {
                auto details = ui::section(name, {
                    ui::toggleRow("Sound Alerts", gx::Key(kAppNotifications, "enable-sound-alerts", path)),
                    ui::toggleRow("Notification Popups", gx::Key(kAppNotifications, "show-banners", path)),
                    ui::toggleRow("Lock Screen Notifications", gx::Key(kAppNotifications, "show-in-lock-screen", path)),
                    ui::toggleRow("Show Message Content on Lock Screen",
                                  gx::Key(kAppNotifications, "details-in-lock-screen", path)),
                });
                Popover(VStack({details}).width(380)).show(anchor);
            });
            rows.push_back(row);
        }
        apps->setRows(rows);
    };
    rebuild();
    children.watch(rebuild);

    return ui::page({
        ui::section({}, {ui::toggleRow("Do Not Disturb", gx::Key(kNotifications, "show-banners"),
                                       "Notification popups are hidden; they still appear in the message tray", true),
                         ui::toggleRow("Lock Screen Notifications", gx::Key(kNotifications, "show-in-lock-screen"))}),
        apps->element(),
    });
}

// Search -----------------------------------------------------------------------------------------------------

Element searchPanel(Context& ctx) {
    if (desk::kde()) return kde::searchPanel(ctx);
    constexpr const char* kProviders = "org.gnome.desktop.search-providers";
    gx::Key disableExternal(kProviders, "disable-external");
    gx::Key disabled(kProviders, "disabled");
    gx::Key enabled(kProviders, "enabled");

    struct Provider {
        std::string id;
        std::string name;
        bool defaultDisabled = false;
    };
    std::vector<Provider> providers;
    std::set<std::string> seen;
    for (const auto& dir : dataDirs()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(dir) / "gnome-shell" / "search-providers", ec)) {
            GKeyFile* kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, e.path().c_str(), G_KEY_FILE_NONE, nullptr)) {
                const char* group = "Shell Search Provider";
                char* id = g_key_file_get_string(kf, group, "DesktopId", nullptr);
                if (id && seen.insert(id).second) {
                    std::string name = gx::appName(id);
                    if (!name.empty())
                        providers.push_back({id, name, g_key_file_get_boolean(kf, group, "DefaultDisabled", nullptr) != FALSE});
                }
                g_free(id);
            }
            g_key_file_free(kf);
        }
    }
    std::sort(providers.begin(), providers.end(), [](const Provider& a, const Provider& b) { return a.name < b.name; });

    std::vector<Element> rows;
    auto isOn = [disabled, enabled](const Provider& p) {
        auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
        return p.defaultDisabled ? has(enabled.getStrv(), p.id) : !has(disabled.getStrv(), p.id);
    };
    for (const auto& p : providers) {
        auto t = Toggle("", isOn(p)).onChange([p, disabled, enabled](bool on) {
            // Default-on providers are listed in "disabled" when off, default-off ones in "enabled" when on.
            const gx::Key& list = p.defaultDisabled ? enabled : disabled;
            auto v = list.getStrv();
            v.erase(std::remove(v.begin(), v.end(), p.id), v.end());
            if (on == p.defaultDisabled) v.push_back(p.id);
            list.setStrv(v);
        });
        rows.push_back(ui::ClickRow({ui::letterTile(p.name), Label(p.name), Spacer(), t}));
        auto sync = [p, t, isOn]() mutable { t.setOn(isOn(p)); };
        disabled.watch(sync);
        enabled.watch(sync);
        auto syncEnabled = [t, disableExternal]() mutable { t.setDisabled(disableExternal.getBool()); };
        syncEnabled();
        disableExternal.watch(syncEnabled);
    }

    std::vector<Element> sections{
        ui::section({}, {ui::toggleRow("App Search", disableExternal, "Search results from apps in the Activities Overview", true)}),
        ui::section("Search Results", rows, "Apps can be turned off individually."),
    };

    // File search locations, when the Tracker miner is installed.
    gx::Key locations("org.freedesktop.Tracker3.Miner.Files", "index-recursive-directories");
    if (locations) {
        auto list = std::make_shared<ui::DynamicSection>("Search Locations", "No folders are searched.",
                                                         "Files in these folders appear in search results.");
        auto rebuild = [locations, list] {
            std::vector<Element> rs;
            for (const auto& dir : locations.getStrv()) {
                static const std::pair<const char*, GUserDirectory> kSpecial[] = {
                    {"&DESKTOP", G_USER_DIRECTORY_DESKTOP}, {"&DOCUMENTS", G_USER_DIRECTORY_DOCUMENTS},
                    {"&DOWNLOAD", G_USER_DIRECTORY_DOWNLOAD}, {"&MUSIC", G_USER_DIRECTORY_MUSIC},
                    {"&PICTURES", G_USER_DIRECTORY_PICTURES}, {"&VIDEOS", G_USER_DIRECTORY_VIDEOS}};
                std::string path = dir == "$HOME" ? g_get_home_dir() : dir;
                for (const auto& [token, ud] : kSpecial) {
                    if (dir == token) path = g_get_user_special_dir(ud) ? g_get_user_special_dir(ud) : dir;
                }
                auto remove = Button(Icon::Minus).style(ButtonStyle::Borderless).tooltip("Remove").onClick([locations, dir] {
                    auto v = locations.getStrv();
                    v.erase(std::remove(v.begin(), v.end(), dir), v.end());
                    locations.setStrv(v);
                });
                rs.push_back(FormRow(HStack({ui::glyph(Icon::Folder), Label(fs::path(path).filename().string()),
                                             Label(path).secondary().font(TextStyle::Subheadline).lineLimit(1), Spacer(), remove})
                                         .spacing(8)
                                         .align(Alignment::Center)));
            }
            list->setRows(rs);
        };
        rebuild();
        locations.watch(rebuild);
        auto add = Button(Icon::Plus).style(ButtonStyle::Borderless).tooltip("Add Folder…");
        add.onClick([&app = ctx.app, &window = ctx.window, locations] {
            app.openFolder([locations](std::vector<std::string> paths) {
                if (paths.empty()) return;
                auto v = locations.getStrv();
                if (std::find(v.begin(), v.end(), paths[0]) == v.end()) v.push_back(paths[0]);
                locations.setStrv(v);
            }, &window);
        });
        list->setAccessory(add);
        sections.push_back(list->element());
    }
    return ui::page(sections);
}

// Apps ---------------------------------------------------------------------------------------------------------

namespace {

struct DefaultCategory {
    const char* title;
    const char* mime;
    std::vector<const char*> related;  ///< also claimed when the chosen app handles them
};

const DefaultCategory kDefaults[] = {
    {"Web", "x-scheme-handler/http", {"x-scheme-handler/https", "text/html", "application/xhtml+xml"}},
    {"Mail", "x-scheme-handler/mailto", {}},
    {"Calendar", "text/calendar", {}},
    {"Music", "audio/x-vorbis+ogg", {"audio/mpeg", "audio/flac", "audio/x-wav", "audio/mp4", "audio/ogg"}},
    {"Video", "video/x-ogm+ogg", {"video/mp4", "video/x-matroska", "video/webm", "video/quicktime"}},
    {"Photos", "image/jpeg", {"image/png", "image/gif", "image/webp", "image/jxl", "image/avif"}},
    {"Files", "inode/directory", {}},
    {"Text Editor", "text/plain", {}},
    {"Documents", "application/pdf", {}},
};

struct AppChoice {
    std::string id;
    std::string name;
};

std::vector<AppChoice> appsFor(const char* mime) {
    std::vector<AppChoice> out;
    GList* all = g_app_info_get_all_for_type(mime);
    for (GList* l = all; l; l = l->next) {
        auto* info = G_APP_INFO(l->data);
        const char* id = g_app_info_get_id(info);
        if (!id || std::any_of(out.begin(), out.end(), [&](const AppChoice& a) { return a.id == id; })) continue;
        out.push_back({id, g_app_info_get_display_name(info)});
    }
    g_list_free_full(all, g_object_unref);
    return out;
}

std::string defaultFor(const char* mime) {
    gx::Object<GAppInfo> info(g_app_info_get_default_for_type(mime, FALSE));
    const char* id = info ? g_app_info_get_id(info.get()) : nullptr;
    return id ? id : "";
}

void setDefault(const DefaultCategory& cat, const std::string& id, const Element& anchor) {
    if (gx::readOnly()) return;
    gx::Object<GDesktopAppInfo> info(g_desktop_app_info_new(id.c_str()));
    if (!info) return;
    GError* err = nullptr;
    if (!g_app_info_set_as_default_for_type(G_APP_INFO(info.get()), cat.mime, &err)) {
        ui::message(anchor, "Couldn't change the default app", err ? err->message : "");
        g_clear_error(&err);
        return;
    }
    for (const char* mime : cat.related) {
        auto handlers = appsFor(mime);
        if (std::any_of(handlers.begin(), handlers.end(), [&](const AppChoice& a) { return a.id == id; }))
            g_app_info_set_as_default_for_type(G_APP_INFO(info.get()), mime, nullptr);
    }
}

void onAppsChanged(GAppInfoMonitor* /*m*/, gpointer data) {
    auto* fn = static_cast<std::function<void()>*>(data);
    gx::toUi([fn] { (*fn)(); });
}

}  // namespace

Element appsPanel(Context& ctx) {
    auto card = std::make_shared<ui::DynamicSection>("Default Apps", "No apps are installed.");
    auto rebuild = std::make_shared<std::function<void()>>();
    *rebuild = [card, anchor = ctx.anchor] {
        std::vector<Element> rows;
        for (const auto& cat : kDefaults) {
            auto apps = appsFor(cat.mime);
            std::vector<std::string> names;
            int sel = -1;
            const std::string current = defaultFor(cat.mime);
            for (size_t i = 0; i < apps.size(); ++i) {
                names.push_back(apps[i].name);
                if (apps[i].id == current) sel = static_cast<int>(i);
            }
            if (sel < 0) {
                names.insert(names.begin(), "None");
                apps.insert(apps.begin(), AppChoice{});
                sel = 0;
            }
            auto picker = Picker(names, sel);
            picker.onChange([&cat, apps, anchor](int i) {
                if (!apps[static_cast<size_t>(i)].id.empty()) setDefault(cat, apps[static_cast<size_t>(i)].id, anchor);
            });
            if (apps.size() <= 1 && apps[0].id.empty()) picker.disabled();
            rows.push_back(FormRow(cat.title, picker));
        }
        card->setRows(rows);
    };
    (*rebuild)();
    // Installing or removing apps, or another program changing mimeapps.list, refreshes the pickers.
    g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(onAppsChanged), new std::function<void()>(*rebuild));

    return ui::page({
        card->element(),
        desk::kde() ? kde::removableMediaSection()
                    : ui::section("Removable Media",
                                  {ui::toggleRow("Automatically Open Media", gx::Key("org.gnome.desktop.media-handling", "autorun-never"),
                                                 "Ask what to do when a disc or camera is connected", true),
                                   ui::toggleRow("Mount Drives Automatically", gx::Key("org.gnome.desktop.media-handling", "automount"))}),
    });
}
