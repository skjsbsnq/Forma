#include "file_panels.hpp"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "applications.hpp"
#include "file_icons.hpp"

namespace fs = std::filesystem;
using namespace mayo;

namespace {

float textWidth(const std::string& text, const Font& font) {
    Label probe(text);
    probe.font(font);
    return probe.node()->naturalWidth();
}

}  // namespace

namespace finder {

namespace {

constexpr float kPad = 12;          // panel padding
constexpr float kRowHeight = 30;    // one application
constexpr float kIconSize = 20;
constexpr float kHeaderHeight = 22;
constexpr float kFooterHeight = 62;  // checkbox row + button row
constexpr float kMaxListHeight = 7 * kRowHeight;
constexpr float kWheelRow = 44;     // points per wheel notch
constexpr float kContentWidth = 336;

// --- application icons ----------------------------------------------------------------------------------

/// Themed SVG icons of applications, decoded once (a 20 pt row never needs more than 64 pixels). The desktop's
/// icon theme comes from GSettings; hicolor and Adwaita are always searched. Empty when there's no vector icon.
std::shared_ptr<ImageData> iconForThemeNameImpl(const std::string& name) {
    static std::map<std::string, std::shared_ptr<ImageData>> cache;
    if (name.empty() || name.find('/') != std::string::npos) return nullptr;
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    std::vector<std::string> roots;
    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home) {
        roots.push_back(std::string(home) + "/icons");
    } else {
        roots.push_back(homeDirectory() + "/.local/share/icons");
    }
    const char* system = std::getenv("XDG_DATA_DIRS");
    std::stringstream ss(system && *system ? system : "/usr/local/share/:/usr/share/");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty()) roots.push_back(dir + "/icons");
    }
    std::string theme;
    // GSettings is the only record of the desktop's icon theme when the environment doesn't say.
    if (!std::getenv("MAYO_DEMO")) {
        if (FILE* pipe = ::popen("gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null", "r")) {
            char buf[256] = {};
            if (std::fgets(buf, sizeof buf, pipe)) {
                std::string value(buf);
                while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
                if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') value = value.substr(1, value.size() - 2);
                if (!value.empty() && value.find('/') == std::string::npos && value.find(' ') == std::string::npos) theme = value;
            }
            ::pclose(pipe);
        }
    }
    // Theme first, then the fallbacks every icon theme inherits from.
    std::vector<std::string> themes;
    if (!theme.empty()) themes.push_back(theme);
    for (const char* fallback : {"hicolor", "Adwaita"}) {
        if (std::find(themes.begin(), themes.end(), fallback) == themes.end()) themes.emplace_back(fallback);
    }
    // Raster sizes first: the decoder reads PNG, not the SVG most themes ship in scalable/.
    const char* sizes[] = {"256x256/apps", "128x128/apps", "64x64/apps", "48x48/apps", "32x32/apps", "scalable/apps"};
    const char* suffixes[] = {".png", ".svg", ".xpm"};
    std::shared_ptr<ImageData> image;
    for (const auto& t : themes) {
        for (const auto& root : roots) {
            for (const char* size : sizes) {
                for (const char* suffix : suffixes) {
                    const std::string path = root + "/" + t + "/" + size + "/" + name + suffix;
                    if (::access(path.c_str(), R_OK) != 0) continue;
                    image = ImageData::load(path, 64);
                    if (image) break;
                }
                if (image) break;
            }
            // Legacy: /usr/share/pixmaps/<name>.png, still where several entries point.
            if (!image) {
                for (const auto& iconsRoot : roots) {
                    const std::string base = iconsRoot.substr(0, iconsRoot.size() - std::strlen("/icons"));
                    for (const char* suffix : suffixes) {
                        const std::string path = base + "/pixmaps/" + name + suffix;
                        if (::access(path.c_str(), R_OK) != 0) continue;
                        image = ImageData::load(path, 64);
                        if (image) break;
                    }
                    if (image) break;
                }
            }
            if (image) break;
        }
        if (image) break;
    }
    cache[name] = image;
    return image;
}

/// The theme's icon, or a generic application mark coloured from the name so entries stay distinguishable.
void drawApplicationIcon(Painter& p, const Rect& r, const std::string& id, const std::shared_ptr<ImageData>& image) {
    if (image && image->width() > 0) {
        p.drawImage(r, *image);
        return;
    }
    const Theme& t = theme();
    const float d = std::min(r.width, r.height);
    const Rect box{r.center().x - d * 0.5f, r.center().y - d * 0.5f, d, d};
    uint32_t hash = 2166136261u;
    for (unsigned char c : id) hash = (hash ^ c) * 16777619u;
    const Color base = t.isDark() ? t.colors.secondaryFill.mix(t.colors.accent, static_cast<float>(hash % 100) / 250.0f)
                                  : t.colors.tertiaryFill.mix(t.colors.accent, 0.25f + static_cast<float>(hash % 100) / 400.0f);
    p.fillRoundedRect(box, d * 0.24f, base);
    p.drawIcon(Icon::Square, box.inset(d * 0.3f), t.colors.secondaryLabel.withAlpha(0.7f), 1.2f);
}

// --- permissions ----------------------------------------------------------------------------------------

enum class Access : std::uint8_t { None, ReadOnly, ReadWrite };

Access accessFor(unsigned mode, int who) {
    const unsigned read = 1u << (8 - who * 3);
    const unsigned write = 1u << (7 - who * 3);
    if ((mode & (read | write)) == (read | write)) return Access::ReadWrite;
    return (mode & read) ? Access::ReadOnly : Access::None;
}

unsigned modeWith(unsigned mode, int who, Access access) {
    const unsigned read = 1u << (8 - who * 3);
    const unsigned write = 1u << (7 - who * 3);
    mode &= ~(read | write);
    if (access != Access::None) mode |= read;
    if (access == Access::ReadWrite) mode |= write;
    return mode;
}

/// "rwxr-xr-x", with setuid/setgid/sticky as s/S and t/T where ls shows them.
std::string lsPermissions(unsigned mode, bool dir) {
    std::string s = dir ? "d" : "-";
    const char* rwx = "rwx";
    for (int i = 8; i >= 0; --i) s.push_back((mode & (1u << i)) ? rwx[(8 - i) % 3] : '-');
    for (int bit = 0; bit < 3; ++bit) {
        if (!(mode & (1u << (11 - bit)))) continue;
        const size_t at = 3 + static_cast<size_t>(2 - bit) * 3;
        s[at] = s[at] == 'x' ? (bit == 0 ? 't' : 's') : (bit == 0 ? 'T' : 'S');
    }
    return s;
}

std::string userLabel(uid_t uid) {
    const std::string id = std::to_string(uid);
    if (const auto* pw = ::getpwuid(uid)) return std::string(pw->pw_name) + "（" + id + "）";
    return id;
}

std::string groupLabel(gid_t gid) {
    const std::string id = std::to_string(gid);
    if (const auto* gr = ::getgrgid(gid)) return std::string(gr->gr_name) + "（" + id + "）";
    return id;
}

/// chmod and translate the usual failures into something the user can act on.
std::string applyMode(const std::string& path, unsigned mode) {
    if (::chmod(path.c_str(), static_cast<mode_t>(mode & 07777)) == 0) return {};
    switch (errno) {
        case EPERM: return "只能更改自己拥有的项目的权限。";
        case EROFS: return "这是一个只读的卷，无法更改权限。";
        case ENOENT: return "该项目已不存在。";
        case EACCES: return "没有更改此项目权限的权限。";
        default: return std::string("无法更改权限：") + std::strerror(errno) + "。";
    }
}

void showError(WidgetHost& host, const std::string& message) {
    auto alert = std::make_shared<AlertView>("无法完成此操作。", message,
                                             std::vector<AlertButton>{{"好", {}, ButtonStyle::Prominent}}, true);
    alert->show(host);
}

}  // namespace

std::shared_ptr<ImageData> applicationIconImage(const std::string& iconName) { return iconForThemeNameImpl(iconName); }

// --- applications ----------------------------------------------------------------------------------------

std::vector<AppChoice> applicationChoices(const FileEntry& entry) {
    std::vector<AppChoice> out;
    for (const auto& id : applicationsFor(entry.path)) {
        if (id.empty()) continue;
        out.push_back({id, applicationDisplayName(id), applicationIcon(id), id == defaultApplicationId(entry.path)});
    }
    if (out.empty()) {
        // Nothing declared support for the type: the file manager's own choice is content (text, image
        // thumbnails), which is exactly what "preview" is for.
        for (const auto& app : installedApplications()) {
            if (app.id.rfind("io.github.egely1337.Mayonnaise.", 0) == 0) {
                out.push_back({app.id, app.name, app.icon, false});
                break;
            }
        }
    }
    return out;
}

std::string permissionsProblem(const FileEntry& entry) {
    struct statvfs st {};
    if (::statvfs(parentPath(entry.path).c_str(), &st) == 0 && (st.f_flag & ST_RDONLY) != 0)
        return "这是一个只读的卷，无法更改权限。";
    return {};
}

std::string openEntryWith(const FileEntry& entry, const std::string& id) {
    if (std::getenv("MAYO_DEMO")) return {};  // demo runs never start other programs
    if (isInTrash(entry.path)) return "“" + entry.name + "”位于回收站中，要使用此项目，请先将其放回原处。";
    std::string error;
    if (!launchApplication(id, entry.path, &error)) {
        return error.empty() ? "无法启动“" + applicationDisplayName(id) + "”。" : error;
    }
    return {};
}

std::string rememberProblem(const FileEntry& entry, const std::string& id) {
    if (isInTrash(entry.path)) return "回收站中的项目不能设为默认打开方式。";
    return defaultApplicationProblem(entry.path, id);
}

std::string rememberApplication(const FileEntry& entry, const std::string& id) {
    if (isInTrash(entry.path)) return "回收站中的项目不能设为默认打开方式。";
    if (!setDefaultApplication(entry.path, id)) return "无法把选择写入 mimeapps.list。";
    return {};
}

// --- OpenWithPanel ---------------------------------------------------------------------------------------

OpenWithPanel::OpenWithPanel(FileEntry entry, std::function<void()> onDone)
    : CenteredPanel(Material::Popover), m_entry(std::move(entry)), m_onDone(std::move(onDone)) {
    setPadding(Insets(kPad, kPad, kPad, kPad));
    if (!m_entry.path.empty()) m_choices = applicationChoices(m_entry);
    for (const auto& choice : m_choices) {
        m_icons.push_back(iconForThemeNameImpl(choice.icon));
        if (choice.current && m_selected.empty()) m_selected = choice.id;
    }
    if (m_selected.empty() && !m_choices.empty()) m_selected = m_choices.front().id;
}

bool OpenWithPanel::canRemember() const { return !m_entry.path.empty() && !m_entry.isDir && !isInTrash(m_entry.path); }

int OpenWithPanel::visibleRows() const { return std::max(1, static_cast<int>(m_choices.size())); }

float OpenWithPanel::listHeight() const { return std::min(kMaxListHeight, static_cast<float>(visibleRows()) * kRowHeight); }

float OpenWithPanel::footerY() const {
    const float base = canRemember() ? kFooterHeight : 0.0f;
    return frame().bottom() - kPad - base;
}

Size OpenWithPanel::measureContent(Size /*available*/) {
    const Theme& t = theme();
    const Font f = t.font(TextStyle::Body);
    Size window = host() ? host()->windowSize() : Size{1000, 700};
    float w = 260;
    for (const auto& choice : m_choices) w = std::max(w, 10 + kIconSize + 8 + textWidth(choice.name, f) + 30);
    if (canRemember()) w = std::max(w, 34 + textWidth("始终以此方式打开", f) + 12);
    w = std::min(w, window.width - 48);
    float h = kPad + kHeaderHeight + listHeight() + kPad;
    if (m_choices.empty()) h += 22;
    if (canRemember()) h += kFooterHeight;
    return {std::max(w, 220.0f), h};
}

Rect OpenWithPanel::rowRect(int index) const {
    return {frame().x + kPad, frame().y + kPad + kHeaderHeight + static_cast<float>(index) * kRowHeight - m_top,
            frame().width - kPad * 2, kRowHeight};
}

int OpenWithPanel::rowAt(Point p) const {
    for (int i = 0; i < static_cast<int>(m_choices.size()); ++i) {
        const Rect row = rowRect(i);
        if (row.y < frame().y + kPad + kHeaderHeight || row.bottom() > frame().y + kPad + kHeaderHeight + listHeight()) continue;
        if (row.contains(p)) return i;
    }
    return -1;
}

Rect OpenWithPanel::rememberBox() const {
    const float top = frame().bottom() - kPad - kFooterHeight;
    return {frame().x + kPad + 2, top + 8, 16, 16};
}

Rect OpenWithPanel::okRect() const { return {frame().right() - kPad - 84, frame().bottom() - kPad - 26, 84, 26}; }

Rect OpenWithPanel::cancelRect() const {
    const Rect ok = okRect();
    return {ok.x - 8 - 76, ok.y, 76, 26};
}

void OpenWithPanel::select(const std::string& id) {
    if (std::none_of(m_choices.begin(), m_choices.end(), [&](const AppChoice& c) { return c.id == id; })) return;
    m_selected = id;
    invalidate();
}

void OpenWithPanel::setRemember(bool on) {
    m_remember = on && canRemember();
    invalidate();
}

void OpenWithPanel::drawPanel(Painter& p) {
    const Theme& t = theme();
    const auto& c = t.colors;
    const Rect r = frame();
    const Font titleFont = t.font(TextStyle::Subheadline).withWeight(FontWeight::Semibold);
    const std::string title = m_entry.path.empty() ? std::string("打开方式") : "打开方式：“" + m_entry.name + "”";
    p.drawText(truncateMiddle(p, title, titleFont, r.width - kPad * 2), {r.x + kPad, r.y + kPad, r.width - kPad * 2, kHeaderHeight},
               titleFont, c.label);

    const Rect list{r.x + kPad, r.y + kPad + kHeaderHeight, r.width - kPad * 2, listHeight()};
    p.pushClip(list);
    if (m_choices.empty()) {
        p.drawText("没有找到能打开此项目的应用。", {list.x, list.y + 2, list.width, 20}, t.font(TextStyle::Body), c.secondaryLabel);
    }
    const Font f = t.font(TextStyle::Body);
    for (int i = 0; i < static_cast<int>(m_choices.size()); ++i) {
        const Rect row = rowRect(i);
        const bool hi = i == m_hover;
        if (hi) p.fillRoundedRect(row.inset(0, 2), t.metrics.smallRadius + 1, c.accent);
        const Color text = hi ? c.onAccent : c.label;
        const Rect icon{row.x + 6, row.center().y - kIconSize * 0.5f, kIconSize, kIconSize};
        drawApplicationIcon(p, icon, m_choices[static_cast<size_t>(i)].id, m_icons[static_cast<size_t>(i)]);
        p.drawText(m_choices[static_cast<size_t>(i)].name, {icon.right() + 8, row.y, row.right() - icon.right() - 30, row.height}, f,
                   text);
        if (m_choices[static_cast<size_t>(i)].current) p.drawIcon(Icon::Checkmark, {row.right() - 20, row.center().y - 6, 12, 12},
                                                                  hi ? c.onAccent : c.tertiaryLabel, 1.4f);
    }
    p.popClip();
    // Arrow hints for the rows beyond the visible ones.
    if (m_choices.size() > static_cast<size_t>(kMaxListHeight / kRowHeight)) {
        const bool more = listHeight() > 0 && m_top + listHeight() / kRowHeight < static_cast<float>(m_choices.size());
        if (m_top > 0.5f) p.drawIcon(Icon::ChevronUp, {list.center().x - 6, list.y - 2, 12, 12}, c.tertiaryLabel, 1.2f);
        if (more) p.drawIcon(Icon::ChevronDown, {list.center().x - 6, list.bottom() - 10, 12, 12}, c.tertiaryLabel, 1.2f);
    }
    if (!canRemember()) return;

    const float top = frame().bottom() - kPad - kFooterHeight;
    const float px = 1.0f / p.scale();
    p.fillRect({r.x + kPad, p.snap(top) - px, r.width - kPad * 2, px}, c.separator);
    const Rect box = rememberBox();
    const Rect boxRow{r.x + kPad, box.y - 8, r.width - kPad * 2, box.height + 16};
    // The buttons live on their own row below the checkbox, like macOS's sheet footers.
    if (m_hoverRemember) p.fillRoundedRect(boxRow, t.metrics.smallRadius, c.hover);
    p.fillRoundedRect(box, 4, m_remember ? c.accent : c.controlBackground, c.controlBorder, 0.5f);
    if (m_remember) p.drawIcon(Icon::Checkmark, box.inset(3), c.onAccent, 1.4f);
    p.drawText("始终以此方式打开", {box.right() + 8, boxRow.y, boxRow.width - 20, boxRow.height}, t.font(TextStyle::Body), c.label);

    const Font buttonFont = t.font(TextStyle::Body);
    const Rect cancel = cancelRect();
    p.fillRoundedRect(cancel, t.metrics.controlRadius, m_hoverCancel ? c.controlHover : c.control, c.controlBorder, 0.5f);
    p.drawText("取消", cancel, buttonFont, c.label, TextAlign::Center);
    const Rect ok = okRect();
    if (m_choices.empty()) {
        p.fillRoundedRect(ok, t.metrics.controlRadius, c.fill);
        p.drawText("打开", ok, buttonFont, c.tertiaryLabel, TextAlign::Center);
    } else {
        p.fillRoundedRect(ok, t.metrics.controlRadius, m_hoverOk ? c.accentHover : c.accent);
        p.drawText("打开", ok, buttonFont, c.onAccent, TextAlign::Center);
    }
}

bool OpenWithPanel::onPointerDown(const PointerEvent& e) {
    if (cancelRect().contains(e.position)) {
        close();
        return true;
    }
    if (!m_choices.empty() && okRect().contains(e.position)) {
        int index = 0;
        for (size_t i = 0; i < m_choices.size(); ++i) {
            if (m_choices[i].id == m_selected) index = static_cast<int>(i);
        }
        open(index);
        return true;
    }
    if (canRemember() && rememberBox().outset(8).contains(e.position)) {
        m_remember = !m_remember;
        invalidate();
        return true;
    }
    const int row = rowAt(e.position);
    if (row < 0) return false;
    // One click opens, like the Finder's "Open With": tick "always" first if the choice should stick.
    m_selected = m_choices[static_cast<size_t>(row)].id;
    m_hover = row;
    invalidate();
    return true;
}

void OpenWithPanel::onPointerMove(const PointerEvent& e) {
    const int row = rowAt(e.position);
    const bool remember = canRemember() && rememberBox().outset(8).contains(e.position);
    const bool ok = !m_choices.empty() && okRect().contains(e.position);
    const bool cancel = cancelRect().contains(e.position);
    if (row != m_hover || remember != m_hoverRemember || ok != m_hoverOk || cancel != m_hoverCancel) {
        m_hover = row;
        m_hoverRemember = remember;
        m_hoverOk = ok;
        m_hoverCancel = cancel;
        invalidate();
    }
}

void OpenWithPanel::onPointerUp(const PointerEvent& e) {
    if (m_hover >= 0 && rowAt(e.position) == m_hover) open(m_hover);
}

void OpenWithPanel::onPointerLeave() {
    if (m_hover == -1 && !m_hoverRemember && !m_hoverOk && !m_hoverCancel) return;
    m_hover = -1;
    m_hoverRemember = m_hoverOk = m_hoverCancel = false;
    invalidate();
}

bool OpenWithPanel::onScroll(const ScrollEvent& e) {
    const float maxTop = std::max(0.0f, static_cast<float>(m_choices.size()) * kRowHeight - listHeight());
    const float next = std::clamp(m_top - e.delta.y * (kWheelRow / 60.0f), 0.0f, maxTop);
    if (next != m_top) {
        m_top = next;
        invalidate();
    }
    return true;
}

void OpenWithPanel::open(int index) {
    if (m_opening || index < 0 || index >= static_cast<int>(m_choices.size())) return;
    m_opening = true;
    m_selected = m_choices[static_cast<size_t>(index)].id;
    const std::string id = m_choices[static_cast<size_t>(index)].id;
    const bool remember = m_remember;
    std::string error;
    if (m_onOpen) error = m_onOpen(id, remember);
    auto keep = shared_from_this();  // the caller may drop the last outside reference from m_onDone
    if (!error.empty()) {
        m_opening = false;
        close();
        if (m_shownHost) showError(*m_shownHost, error);
        return;
    }
    close();
}

void OpenWithPanel::close() {
    auto keep = shared_from_this();
    const bool shown = isShown();
    if (shown) dismiss();
    if (m_onDone && shown) m_onDone();
}

bool OpenWithPanel::onKeyDown(const KeyEvent& e) {
    if (e.key == Key::Escape) {
        close();
        return true;
    }
    if (e.key == Key::Enter) {
        int index = 0;
        for (size_t i = 0; i < m_choices.size(); ++i) {
            if (m_choices[i].id == m_selected) index = static_cast<int>(i);
        }
        open(index);
        return true;
    }
    return false;
}

// --- Properties ------------------------------------------------------------------------------------------

namespace {

constexpr float kPropertyRow = 22;     // one row of the permission editor
constexpr float kMenuWidth = 132;    // the permission pop-up buttons

enum class Who : int { Owner, Group, Others };

const char* whoTitle(int who) { return who == 0 ? "所有者" : who == 1 ? "所在组" : "其他人"; }

const char* accessTitle(Access access) {
    switch (access) {
        case Access::ReadWrite: return "读取与写入";
        case Access::ReadOnly: return "只读";
        case Access::None: return "无访问权限";
    }
    return "";
}

/// "说明：值" row; `trailing` stays at the far right.
Element propertyRow(std::string key, const Element& value, const Element& trailing = Spacer(0)) {
    return HStack({Label(std::move(key)).secondary().width(72).textAlign(TextAlign::Trailing), value, trailing})
        .spacing(8)
        .align(Alignment::Center);
}

/// The permission menus of the properties sheet. A change applies at once (chmod), reports a failure, and calls
/// `changed` after success so the browser can redraw the item with its new look.
class PermissionEditorView : public Widget {
public:
    PermissionEditorView(FileEntry entry, std::function<void()> changed) : m_entry(std::move(entry)), m_changed(std::move(changed)) {
        struct stat st {};
        if (::lstat(m_entry.path.c_str(), &st) == 0) m_mode = st.st_mode & 07777;
    }
    std::function<void(const std::string&)> onError;

    mayo::Size measureContent(mayo::Size available) override { return {available.width, 3 * kPropertyRow}; }
    void draw(Painter& p) override {
        const Theme& t = theme();
        const auto& c = t.colors;
        const Font f = t.font(TextStyle::Body);
        for (int i = 0; i < 3; ++i) {
            const Rect row = rowRect(i);
            p.drawText(whoTitle(i), {row.x, row.y, row.width - kMenuWidth - 8, row.height}, f, c.label);
            const Rect bezel = menuRect(i);
            p.fillRoundedRect(bezel, t.metrics.controlRadius, m_open == i ? c.controlPressed : m_hover == i ? c.controlHover : c.control,
                              c.controlBorder, 0.5f);
            p.drawText(accessTitle(accessFor(m_mode, i)), bezel.inset(Insets(0, 0, 0, 26)), f, c.label, TextAlign::Center);
            p.drawIcon(Icon::ChevronUpDown, {bezel.right() - 18, bezel.center().y - 5, 10, 10}, c.secondaryLabel, 1.3f);
        }
    }
    bool onPointerDown(const PointerEvent& e) override {
        for (int i = 0; i < 3; ++i) {
            if (!menuRect(i).contains(e.position)) continue;
            openMenu(i);
            return true;
        }
        return false;
    }
    void onPointerMove(const PointerEvent& e) override {
        int hover = -1;
        for (int i = 0; i < 3; ++i) {
            if (menuRect(i).contains(e.position)) hover = i;
        }
        if (hover == m_hover) return;
        m_hover = hover;
        invalidate();
    }
    void onPointerLeave() override {
        if (m_hover == -1) return;
        m_hover = -1;
        invalidate();
    }

    /// Sets the access for one class of user; returns the error, or "" when applied.
    std::string setAccess(int who, Access access);

private:
    Rect rowRect(int i) const { return {frame().x, frame().y + static_cast<float>(i) * kPropertyRow, frame().width, kPropertyRow}; }
    Rect menuRect(int i) const {
        const Rect row = rowRect(i);
        return {row.right() - kMenuWidth, row.y + 1, kMenuWidth, kPropertyRow - 3};
    }
    void openMenu(int who);

    FileEntry m_entry;
    std::function<void()> m_changed;
    unsigned m_mode = 0;
    int m_hover = -1;
    int m_open = -1;
    std::shared_ptr<MenuView> m_menu;
    std::shared_ptr<Widget> m_anchor;  // invisible: the menu's position under the pop-up button
};

std::string PermissionEditorView::setAccess(int who, Access access) {
    const unsigned mode = modeWith(m_mode, who, access);
    if (mode == m_mode) return {};
    if (const std::string problem = permissionsProblem(m_entry); !problem.empty()) return problem;
    if (const std::string error = applyMode(m_entry.path, mode); !error.empty()) return error;
    m_mode = mode;
    invalidate();
    if (m_changed) m_changed();
    return {};
}

void PermissionEditorView::openMenu(int who) {
    if (!host()) return;
    if (!m_menu) {
        m_menu = std::make_shared<MenuView>(std::vector<MenuItem>{});
        m_menu->setCentered(true);
        m_menu->setOnDismiss([this] {
            m_open = -1;
            invalidate();
        });
    }
    std::vector<MenuItem> items;
    for (Access access : {Access::ReadWrite, Access::ReadOnly, Access::None}) {
        items.push_back(MenuItem(accessTitle(access), [this, who, access] {
                            const std::string error = setAccess(who, access);
                            if (!error.empty() && onError) onError(error);
                        }).setChecked(accessFor(m_mode, who) == access));
    }
    m_menu->setItems(std::move(items));
    m_open = who;
    invalidate();
    // Anchor the menu to the pop-up button itself, not to the whole editor.
    if (!m_anchor) {
        m_anchor = std::make_shared<Widget>();
        addChild(m_anchor);
    }
    m_anchor->arrange(menuRect(who));
    m_menu->setGap(2);
    m_menu->setMinimumWidth(kMenuWidth);
    m_menu->showFrom(*m_anchor);
    host()->setFocus(m_menu.get(), false);
}

/// An invisible anchor: menus hang from a fake widget placed anywhere.
class AnchorView : public Widget {
public:
    mayo::Size measureContent(mayo::Size /*available*/) override { return {0, 0}; }
};

class PropertiesView : public CenteredPanel {
public:
    PropertiesView(FileEntry entry, Element appButton, std::function<void()> changed)
        : CenteredPanel(Material::Popover), m_entry(std::move(entry)), m_changed(std::move(changed)) {
        setPadding(Insets(20, 20, 18, 20));
        const FileEntry e = m_entry;
        // Header: the item's icon, its name and what kind of item it is.
        auto icon = std::make_shared<CanvasView>([e](Painter& p, const Rect& r) {
            IconStyle style;
            style.folderGlyph = e.isDir ? folderGlyphFor(e.path) : Icon::None;
            drawFileIcon(p, r, e, style);
        });
        icon->setFixedWidth(60);
        icon->setFixedHeight(60);
        m_name = Label(e.name).font(TextStyle::Headline).lineLimit(2).maxWidth(242);
        auto pencil = Button(Icon::Pencil).style(ButtonStyle::Borderless).tooltip("重命名");
        pencil.onClick([this] { if (m_onRename) m_onRename(); });
        auto subtitle = Label(kindDescription(e) + (e.isDir ? "" : " · " + formatSize(e.size))).secondary();
        m_sizeLabel = subtitle;
        auto header = HStack({HStack({Element(icon)}).frame(60, 60),
                              VStack({HStack({m_name, Spacer(), pencil}).spacing(4).align(Alignment::Center), subtitle})
                                  .spacing(2)})
                          .spacing(14)
                          .align(Alignment::Center);

        std::vector<Element> rows;
        rows.push_back(propertyRow("种类", Label(kindLabel(e.kind)).font(TextStyle::Body)));
        rows.push_back(propertyRow("位置", Label(parentPath(e.path)).lineLimit(2)));
        if (e.isLink) {
            std::error_code ec;
            const std::string target = fs::read_symlink(e.path, ec).string();
            rows.push_back(propertyRow("原始项目", Label(target.empty() ? std::string("（无法读取）") : target).lineLimit(2)));
        }
        if (isInTrash(e.path)) {
            if (auto original = trashOriginalPath(e.path))
                rows.push_back(propertyRow("还原位置", Label(parentPath(*original)).lineLimit(1)));
        }
        if (e.created > 0) rows.push_back(propertyRow("创建时间", Label(formatDate(e.created))));
        rows.push_back(propertyRow("修改时间", Label(formatDate(e.modified))));
        struct stat st {};
        ::lstat(e.path.c_str(), &st);
        rows.push_back(propertyRow("所有者", Label(userLabel(st.st_uid))));
        rows.push_back(propertyRow("所有组", Label(groupLabel(st.st_gid))));
        if (e.special != 0) {
            std::string flags;
            if (e.special & S_ISUID) flags += "setuid ";
            if (e.special & S_ISGID) flags += "setgid ";
            if (e.special & S_ISVTX) flags += "sticky ";
            flags.pop_back();
            rows.push_back(propertyRow("特殊权限", Label(flags)));
        }
        rows.push_back(propertyRow("类型", Label(mimeTypeFor(e.path)).lineLimit(1)));
        const std::string appId = defaultApplicationId(e.path);
        rows.push_back(propertyRow("打开方式", Label(appId.empty() ? "无" : applicationDisplayName(appId)).lineLimit(1), appButton));
        if (e.isDir) {
            m_sizeLabel = Label("正在计算…").secondary();
            rows.push_back(propertyRow("大小", m_sizeLabel));
        }
        auto rowsStack = VStack(rows).spacing(6);

        auto permissions = std::make_shared<PermissionEditorView>(e, [this] {
            if (m_changed) m_changed();
        });
        permissions->onError = [this](const std::string& error) {
            if (m_shownHost) showError(*m_shownHost, error);
        };
        auto readout = Label("权限位：" + lsPermissions(e.mode, e.isDir)).font(TextStyle::Caption).tertiary();
        auto permissionsTitle = HStack({Label("权限").font(TextStyle::Subheadline), Spacer(), readout}).spacing(8);
        auto permissionsBox = VStack({Element(permissionsTitle), Element(permissions)}).spacing(8);
        auto done = Button("完成").style(ButtonStyle::Prominent).onClick([this] {
            if (m_shown) dismiss();
        });
        auto buttons = HStack({Spacer(), done}).spacing(8);
        addChild(VStack({header.node(), Divider().node(), rowsStack.node(), Divider().node(), permissionsBox.node(),
                         Divider().node(), buttons.node()})
                     .spacing(12)
                     .width(380)
                     .node());
    }

    std::function<void()> m_onRename;
    void setFolderSize(std::uintmax_t bytes, size_t items) {
        m_sizeLabel.setText(formatSize(bytes) + "（" + itemCount(items) + "）");
    }

protected:
    void drawPanel(Painter& p) override { paintChildren(p); }

private:
    FileEntry m_entry;
    std::function<void()> m_changed;
    Label m_name;
    Label m_sizeLabel;
    std::shared_ptr<PermissionEditorView> m_permissions;
};

std::weak_ptr<PropertiesView> latestProperties;

}  // namespace

void presentProperties(WidgetHost& host, const FileEntry& entry, const Element& appButton,
                       std::function<void()> changed) {
    auto panel = std::make_shared<PropertiesView>(entry, appButton, std::move(changed));
    panel->present(host);
    // Only the newest sheet reports its size; an older one may still be fading out behind it.
    latestProperties = panel;
}

void closeProperties(WidgetHost& /*host*/) {
    if (auto panel = latestProperties.lock(); panel && panel->isShown()) panel->dismiss();
}

void setLatestFolderSize(std::uintmax_t bytes, size_t items) {
    if (auto panel = latestProperties.lock()) panel->setFolderSize(bytes, items);
}

}  // namespace finder
