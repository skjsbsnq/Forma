// Video Player — a QuickTime / TV app-style player on Mayonnaise, decoding with FFmpeg.
//
//   video_player [file...]
//
// Without files it opens the library: a gallery of the videos in ~/Videos (and any folders you add), with
// Continue Watching, Recently Played and per-folder views. Subtitles (.srt / .vtt) next to a video load by
// themselves; drop more onto the window or pick them from the captions button.
//
// Space / K play-pause · ← → seek 5 s · J L seek 10 s · ↑ ↓ volume · M mute · F full screen · Esc library ·
// C subtitles on/off · V next subtitle track · G / H subtitle delay −/+ 0.1 s · Ctrl+O open · Ctrl+F search.
#include "../common/zh_cn.hpp"
#include <mayonnaise/mayonnaise.hpp>

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "gallery.hpp"
#include "library.hpp"
#include "media_player.hpp"
#include "subtitles.hpp"

extern char** environ;

using namespace mayo;
namespace fs = std::filesystem;

namespace {

/// Round icon button for the HUD: white glyph, soft circular hover.
class HudButton : public Widget {
public:
    HudButton(Icon icon, float size) : m_icon(icon), m_size(size) {}
    std::function<void()> onClick;
    void setIcon(Icon i) {
        if (i != m_icon) {
            m_icon = i;
            invalidate();
        }
    }
    void setActive(bool a) {
        if (a != m_active) {
            m_active = a;
            invalidate();
        }
    }
    [[nodiscard]] bool isInteractive() const override { return true; }
    Size measureContent(Size /*available*/) override { return {m_size + 14, m_size + 14}; }
    void draw(Painter& p) override {
        Rect r = frame();
        float wash = isPressed() ? 0.22f : isHovered() ? 0.12f : 0.0f;
        if (wash > 0) p.fillCircle(r.center(), r.width * 0.5f, Color::white(wash));
        Color c = m_active ? theme().colors.accent.lighter(0.25f) : Color::white(0.95f);
        p.drawIcon(m_icon, {r.center().x - m_size * 0.5f, r.center().y - m_size * 0.5f, m_size, m_size}, c, 1.15f);
    }
    bool onPointerDown(const PointerEvent& e) override { return e.button == MouseButton::Left; }
    void onPointerUp(const PointerEvent& e) override {
        if (frame().contains(e.position) && onClick) onClick();
    }

private:
    Icon m_icon;
    float m_size;
    bool m_active = false;
};

/// Thin white track used for the timeline and the volume. Grows while hovered, like QuickTime.
class TrackBar : public Widget {
public:
    std::function<void(float, bool)> onChange;  // value, final (pointer released)
    std::function<std::string(float)> hoverLabel;
    void setValue(float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (std::abs(v - m_value) > 1e-4f && !m_dragging) {
            m_value = v;
            invalidate();
        }
    }
    [[nodiscard]] float value() const { return m_value; }
    [[nodiscard]] bool isDragging() const { return m_dragging; }
    [[nodiscard]] bool isInteractive() const override { return true; }
    Size measureContent(Size available) override {
        return {std::isfinite(available.width) ? available.width : 120.0f, 18};
    }
    [[nodiscard]] bool expandsContent(Axis a) const override { return a == Axis::Horizontal; }
    void onHoverChanged(bool h) override { m_grow.animateTo(h ? 1.0f : 0.0f, now(), Transition::fast()); }
    void draw(Painter& p) override {
        const Rect r = frame();
        const float g = std::max(sample(m_grow), m_dragging ? 1.0f : 0.0f);
        const float h = 4 + 3 * g;
        Rect track{r.x, r.center().y - h * 0.5f, r.width, h};
        p.fillRoundedRect(track, h * 0.5f, Color::white(0.22f));
        p.fillRoundedRect({track.x, track.y, std::max(h, track.width * m_value), h}, h * 0.5f, Color::white(0.92f));
        if (hoverLabel && (isHovered() || m_dragging) && m_hoverX >= 0) {
            // Time bubble above the pointer.
            float f = std::clamp((m_hoverX - r.x) / std::max(1.0f, r.width), 0.0f, 1.0f);
            std::string text = hoverLabel(m_dragging ? m_value : f);
            Font font = theme().font(TextStyle::Subheadline).withWeight(FontWeight::Medium);
            float w = p.measureText(text, font) + 14;
            Rect bubble{std::clamp(m_hoverX - w * 0.5f, r.x - 20, r.right() + 20 - w), r.y - 30, w, 22};
            p.drawShadow(bubble, 8, Color::black(0.4f), 8, {0, 2});
            p.fillRoundedRect(bubble, 8, Color::rgb8(40, 40, 44, 0.95f));
            p.drawText(text, bubble, font, Color::white(), TextAlign::Center);
        }
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        m_dragging = true;
        update(e.position.x, false);
        return true;
    }
    void onPointerMove(const PointerEvent& e) override {
        m_hoverX = e.position.x;
        if (m_dragging) update(e.position.x, false);
        if (hoverLabel) invalidate();
    }
    void onPointerUp(const PointerEvent& e) override {
        update(e.position.x, true);
        m_dragging = false;
    }
    void onPointerLeave() override { m_hoverX = -1; }

private:
    void update(float x, bool final) {
        const Rect r = frame();
        m_value = std::clamp((x - r.x) / std::max(1.0f, r.width), 0.0f, 1.0f);
        invalidate();
        if (onChange) onChange(m_value, final);
    }
    float m_value = 0;
    float m_hoverX = -1;
    bool m_dragging = false;
    Animated<float> m_grow{0.0f};
};

/// Dark Liquid Glass panel holding the transport controls.
class HudPanel : public Widget {
public:
    explicit HudPanel(const Element& content) {
        setPadding(Insets(12, 18, 14, 18));
        addChild(content.widget());
    }
    void draw(Painter& p) override {
        const Rect r = frame();
        const float radius = 22;
        p.drawShadow(r, radius, Color::black(0.45f), 30, {0, 10});
        MaterialStyle m = theme().material(Material::Popover);
        m.tint = Color::rgb8(28, 28, 32, 0.55f);
        m.saturation = 1.4f;
        m.highlight = Color::white(0.18f);
        m.border = Color::white(0.10f);
        p.drawMaterial(r, radius, m);
        paintChildren(p);
    }
};

struct SubtitleStyle {
    float scale = 1;
    bool background = true;
};

/// Draws the current video frame letterboxed on black, the subtitles over it, and short on-screen messages.
class VideoSurface : public Widget {
public:
    explicit VideoSurface(MediaPlayer& player) : m_player(player) {}
    std::function<void()> onClick;
    std::function<void()> onDoubleClick;
    std::function<void()> onTick;  // every rendered frame while playing (HUD time, end detection)
    std::string audioTitle;
    const SubtitleTrack* subtitles = nullptr;
    double subtitleDelay = 0;  ///< seconds; positive shows subtitles later
    SubtitleStyle style;
    float subtitleBottom = 0;  ///< window y the subtitles stay above (the HUD's top while it's shown)

    void reset() {
        m_texture.reset();
        m_shown = {};
    }
    /// A message in the corner for a moment ("Subtitle delay +0.2 s").
    void showMessage(std::string text) {
        m_osd = std::move(text);
        m_osdUntil = now() + 1.8;
        invalidate();
        if (host()) host()->scheduleFrame(m_osdUntil + 0.05);
    }
    [[nodiscard]] bool isInteractive() const override { return true; }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }

    void draw(Painter& p) override {
        const Rect r = frame();
        p.fillRect(r, Color::black());
        if (m_player.isOpen()) {
            MediaPlayer::Frame f;
            if (m_player.takeFrame(f)) {
                if (!m_texture) m_texture = VideoTexture::create();
                if (f.uploadTo(*m_texture)) {
                    // Keep the frame (and with it the decoder surface) alive while it's on screen.
                    m_shown = std::move(f);
                } else {
                    m_player.disableZeroCopy();  // import failed: later frames come through system memory
                }
            }
            Rect video = r;
            if (m_player.hasVideo() && m_texture && !m_texture->isEmpty()) {
                const auto aspect = static_cast<float>(m_player.aspect());
                float w = r.width;
                float h = w / aspect;
                if (h > r.height) {
                    h = r.height;
                    w = h * aspect;
                }
                video = {r.center().x - w * 0.5f, r.center().y - h * 0.5f, w, h};
                p.drawVideo(video, *m_texture);
            } else if (!m_player.hasVideo()) {
                // Audio only: a big note-like glyph and the title.
                const Theme& t = theme();
                Rect icon{r.center().x - 48, r.center().y - 90, 96, 96};
                p.fillRoundedRect(icon.outset(18), 30, Color::white(0.08f));
                p.drawIcon(Icon::MusicNote, icon, Color::white(0.7f), 0.8f);
                p.drawText(audioTitle, {r.x, icon.bottom() + 34, r.width, 30}, t.font(TextStyle::Title2), Color::white(0.9f),
                           TextAlign::Center);
            }
            drawSubtitles(p, video);
            if (m_player.isPlaying() || m_player.isSeeking()) requestAnimationFrame();
        }
        drawMessage(p, r);
        if (onTick) onTick();
    }
    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        if (e.clickCount == 2) {
            if (onDoubleClick) onDoubleClick();
        } else if (onClick) {
            onClick();
        }
        return true;
    }

private:
    /// Greedy word wrap to `maxWidth`.
    static std::vector<std::string> wrap(Painter& p, const std::string& text, const Font& font, float maxWidth) {
        std::vector<std::string> out;
        std::string line;
        size_t i = 0;
        while (i <= text.size()) {
            size_t sp = text.find(' ', i);
            if (sp == std::string::npos) sp = text.size();
            const std::string word = text.substr(i, sp - i);
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && p.measureText(candidate, font) > maxWidth) {
                out.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
            i = sp + 1;
        }
        if (!line.empty()) out.push_back(line);
        return out;
    }

    void drawSubtitles(Painter& p, const Rect& video) {
        if (!subtitles) return;
        const auto cues = subtitles->at(m_player.position() - subtitleDelay);
        if (cues.empty()) return;
        const float size = std::clamp(video.height * 0.046f * style.scale, 14.0f, 72.0f);
        const Font base = theme().font(TextStyle::Title2).withSize(size).withWeight(FontWeight::Medium);
        const float maxWidth = video.width * 0.86f;
        struct Line {
            std::string text;
            Font font;
        };
        std::vector<Line> lines;
        for (const SubtitleCue* cue : cues) {
            const Font f = cue->italic ? base.withItalic() : base;
            for (const auto& l : cue->lines) {
                for (auto& w : wrap(p, l, f, maxWidth)) lines.push_back({std::move(w), f});
            }
        }
        const float lineHeight = std::round(size * 1.34f);
        float bottom = video.bottom() - std::max(16.0f, video.height * 0.055f);
        if (subtitleBottom > 0) bottom = std::min(bottom, subtitleBottom - 14);
        bottom = std::min(bottom, frame().bottom() - 16);
        float y = bottom - lineHeight * static_cast<float>(lines.size());
        const float pad = size * 0.38f;
        for (const auto& l : lines) {
            const float w = p.measureText(l.text, l.font);
            const Rect box{std::round(video.center().x - w * 0.5f - pad), y, std::round(w + pad * 2), lineHeight};
            if (style.background) {
                p.fillRoundedRect(box, size * 0.24f, Color::black(0.6f));
            } else {
                // Outline for legibility on bright scenes.
                const float o = std::max(1.0f, size * 0.06f);
                for (const Vec2 d : {Vec2{-o, 0}, {o, 0}, {0, -o}, {0, o}, {-o, -o}, {o, o}, {-o, o}, {o, -o}})
                    p.drawText(l.text, {box.x + d.x, box.y + d.y, box.width, box.height}, l.font, Color::black(0.85f), TextAlign::Center);
            }
            p.drawText(l.text, box, l.font, Color::white(), TextAlign::Center);
            y += lineHeight;
        }
    }

    void drawMessage(Painter& p, const Rect& r) {
        if (m_osd.empty() || now() > m_osdUntil) return;
        const float a = std::clamp(static_cast<float>(m_osdUntil - now()) / 0.3f, 0.0f, 1.0f);
        const Font f = theme().font(TextStyle::Headline);
        const float w = p.measureText(m_osd, f) + 28;
        const Rect box{r.x + 24, r.y + 64, w, 34};
        p.pushOpacity(a);
        p.drawShadow(box, 12, Color::black(0.4f), 14, {0, 4});
        p.fillRoundedRect(box, 12, Color::rgb8(30, 30, 34, 0.86f));
        p.drawText(m_osd, box, f, Color::white(), TextAlign::Center);
        p.popOpacity();
        if (a < 1) requestAnimationFrame();
    }

    MediaPlayer& m_player;
    std::shared_ptr<VideoTexture> m_texture;
    MediaPlayer::Frame m_shown;
    std::string m_osd;
    double m_osdUntil = 0;
};

/// Root of the player: video under everything (including the title bar), HUD floating at the bottom, error state.
class PlayerPage : public Widget {
public:
    std::shared_ptr<VideoSurface> surface;
    std::shared_ptr<HudPanel> hud;
    std::shared_ptr<Widget> empty;
    std::function<void()> onActivity;

    [[nodiscard]] bool extendsIntoTitleBar() const override { return true; }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }
    void setHudVisible(bool visible) {
        m_hud.animateTo(visible ? 1.0f : 0.0f, now(), Transition{visible ? 0.18f : 0.4f, Curve::EaseInOut});
        invalidate();
    }
    void arrangeContent(const Rect& c) override {
        surface->measure(c.size());
        surface->arrange(c);
        const float w = std::min(660.0f, c.width - 40);
        Size s = hud->measure({w, c.height});
        hud->arrange({c.center().x - w * 0.5f, c.bottom() - s.height - 24, w, s.height});
        const float bar = host() ? host()->titleBarHeight() : 0.0f;
        Rect below{c.x, c.y + bar, c.width, c.height - bar};
        empty->measure(below.size());
        empty->arrange(below);
    }
    void draw(Painter& p) override {
        const float o = sample(m_hud);
        hud->setOpacity(o);
        // Subtitles move up while the controls are showing, like the TV app.
        surface->subtitleBottom = hud->isVisible() && o > 0.5f ? hud->frame().y : 0.0f;
        paintChildren(p);
    }
    void onPointerMove(const PointerEvent& /*e*/) override {
        if (onActivity) onActivity();
    }

private:
    Animated<float> m_hud{1.0f};
};

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : "\n") + s;
    return out;
}

/// Opens a folder in the desktop's file manager.
void showInFiles(const std::string& folder) {
    pid_t pid = 0;
    const char* argv[] = {"xdg-open", folder.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) {
        // Reap it in the background so it doesn't linger as a zombie.
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    }
}

/// Anchor for context menus at the pointer (menus open from a widget's frame).
class PointAnchor : public Widget {
public:
    Size measureContent(Size /*available*/) override { return {0, 0}; }
};

}  // namespace

int main(int argc, char** argv) {
    mayo::setUiTranslator(forma::zhCN);
    App app("video_player", MAYO_DESKTOP_ID);
    app.setDefaultAppearanceMode(AppearanceMode::Dark);  // a media app: dark chrome reads best around video
    auto& window = app.createWindow(
        {.title = "视频", .width = 1100, .height = 700, .minWidth = 560, .minHeight = 380, .autosaveName = "main"});
    window.setTitleVisible(false);
    Settings& prefs = app.settings();
    const bool demo = std::getenv("MAYO_DEMO") != nullptr;
    MediaPlayer player;
    player.setHardwareDecoding(prefs.getBool("hardwareDecoding", true));
    Prober prober(app, std::clamp(std::thread::hardware_concurrency() / 2, 1u, 3u));

    std::deque<std::string> playlist;
    bool looping = prefs.getBool("loop");
    bool muted = prefs.getBool("muted");
    float volume = std::clamp(prefs.getFloat("volume", 1.0f), 0.0f, 1.0f);
    std::string currentPath;
    bool playerMode = false;

    // Library state ---------------------------------------------------------------------------------
    std::vector<std::string> folders = splitLines(prefs.getString("libraryFolders"));
    if (!prefs.contains("libraryFolders")) folders = {videosDirectory()};
    std::vector<std::string> history = splitLines(prefs.getString("history"));  // newest first
    std::map<std::string, VideoPtr> items;  // every item we know, by path
    std::vector<VideoPtr> library;          // items found in the library folders
    std::set<const VideoItem*> queued;
    std::string section = prefs.getString("librarySection", "home");
    std::string query;
    bool scanning = false;
    std::thread scanner;

    // Where each recently played file was left, newest first.
    std::vector<std::pair<double, std::string>> positions;
    for (const auto& line : splitLines(prefs.getString("positions"))) {
        if (const size_t tab = line.find('\t'); tab != std::string::npos)
            positions.emplace_back(std::atof(line.substr(0, tab).c_str()), line.substr(tab + 1));
    }
    auto savePositions = [&] {
        std::string text;
        char buf[32];
        for (const auto& [seconds, path] : positions) {
            std::snprintf(buf, sizeof buf, "%.1f", seconds);
            text += (text.empty() ? "" : "\n") + std::string(buf) + "\t" + path;
        }
        prefs.set("positions", text);
    };
    auto positionOf = [&](const std::string& path) -> double {
        for (const auto& [seconds, p] : positions) {
            if (p == path) return seconds;
        }
        return -1;
    };
    auto rememberPosition = [&] {
        if (currentPath.empty() || !player.isOpen()) return;
        std::erase_if(positions, [&](const auto& e) { return e.second == currentPath; });
        const double pos = player.position();
        const double dur = player.duration();
        // Nothing to resume at the very start or end.
        if (prefs.getBool("resumePlayback", true) && pos > 5 && (dur <= 0 || pos < dur - 5)) positions.emplace(positions.begin(), pos, currentPath);
        if (positions.size() > 50) positions.resize(50);
        savePositions();
    };
    auto itemFor = [&](const std::string& path) {
        auto it = items.find(path);
        if (it != items.end()) return it->second;
        return items.emplace(path, makeItem(path)).first->second;
    };
    int activity = 0;

    // Player controls ----------------------------------------------------------------------------------
    auto play = std::make_shared<HudButton>(Icon::Play, 26);
    auto back = std::make_shared<HudButton>(Icon::SkipBack, 20);
    auto forward = std::make_shared<HudButton>(Icon::SkipForward, 20);
    auto speaker = std::make_shared<HudButton>(Icon::Speaker, 16);
    auto loop = std::make_shared<HudButton>(Icon::Repeat, 16);
    auto fullscreen = std::make_shared<HudButton>(Icon::Expand, 16);
    auto infoButton = std::make_shared<HudButton>(Icon::Info, 16);
    auto captions = std::make_shared<HudButton>(Icon::Captions, 17);
    auto settingsButton = std::make_shared<HudButton>(Icon::Gear, 16);
    auto volumeBar = std::make_shared<TrackBar>();
    volumeBar->setValue(1);
    volumeBar->setFixedWidth(84);
    auto timeline = std::make_shared<TrackBar>();
    auto elapsed = Label("0:00").font(TextStyle::Subheadline).foreground(Color::white(0.85f)).width(56);
    auto remaining = Label("-0:00").font(TextStyle::Subheadline).foreground(Color::white(0.85f)).width(56).textAlign(TextAlign::Trailing);
    captions->setTooltip("Subtitles (C)");

    auto hudContent = VStack({
                                 HStack({speaker, Element(volumeBar), Spacer(), back, play, forward, Spacer(), Element(captions),
                                         Element(infoButton), Element(settingsButton), loop, fullscreen})
                                     .spacing(6)
                                     .align(Alignment::Center),
                                 HStack({elapsed, Element(timeline), remaining}).spacing(10).align(Alignment::Center),
                             })
                          .spacing(6);
    auto hud = std::make_shared<HudPanel>(hudContent);
    auto surface = std::make_shared<VideoSurface>(player);
    surface->style.scale = std::clamp(prefs.getFloat("subtitleSize", 1.0f), 0.6f, 2.0f);
    surface->style.background = prefs.getBool("subtitleBackground", true);

    auto emptyIcon = Canvas([](Painter& p, const Rect& r) { p.drawIcon(Icon::Film, r, Color::white(0.45f), 0.7f); });
    emptyIcon.frame(64, 64);
    auto emptyError = Label("").foreground(theme().colors.red).maxWidth(460).textAlign(TextAlign::Center);
    auto backToLibrary = Button(Icon::ChevronLeft, "Library").style(ButtonStyle::Prominent);
    auto empty = VStack({emptyIcon, Label("This file can't be played").font(TextStyle::Title2), emptyError, backToLibrary})
                     .spacing(10)
                     .align(Alignment::Center);
    empty.setHidden(true);

    auto page = std::make_shared<PlayerPage>();
    page->surface = surface;
    page->hud = hud;
    page->empty = empty.node();
    page->addChild(surface);
    page->addChild(hud);
    page->addChild(empty.node());

    // Library widgets ----------------------------------------------------------------------------------
    auto grid = std::make_shared<VideoGridView>();
    auto gridScroll = ScrollView(Element(grid));
    grid->setScroller(gridScroll.node().get());
    auto libEmptyTitle = Label("No Videos").font(TextStyle::Title2);
    auto libEmptyText = Label("").secondary().textAlign(TextAlign::Center).maxWidth(420);
    auto libOpen = Button(Icon::Import, "Open Video…").style(ButtonStyle::Prominent);
    auto libAddFolder = Button(Icon::FolderPlus, "Add Folder…");
    auto libEmptyIcon = Canvas([](Painter& p, const Rect& r) { p.drawIcon(Icon::Film, r, theme().colors.tertiaryLabel, 0.7f); });
    libEmptyIcon.frame(64, 64);
    auto libEmpty = VStack({libEmptyIcon, libEmptyTitle, libEmptyText, HStack({libAddFolder, libOpen}).spacing(10)})
                        .spacing(10)
                        .align(Alignment::Center);
    auto libraryPage = std::make_shared<LibraryPage>();
    libraryPage->scroll = gridScroll.node();
    libraryPage->empty = libEmpty.node();
    libraryPage->addChild(gridScroll.node());
    libraryPage->addChild(libEmpty.node());
    auto menuAnchor = std::make_shared<PointAnchor>();
    libraryPage->addChild(menuAnchor);
    auto contextMenu = std::make_shared<MenuView>(std::vector<MenuItem>{});

    auto sidebar = Sidebar(std::vector<SidebarItem>{});
    auto split = SplitView(sidebar, Element(libraryPage)).sidebarWidth(220).sidebarLimits(170, 320);
    auto titleLabel = Label("").font(TextStyle::Title3).lineLimit(1);
    auto addFolderButton = Button(Icon::FolderPlus).style(ButtonStyle::Borderless).tooltip("Add Folder to Library…");
    auto openButton = Button(Icon::Import).style(ButtonStyle::Borderless).tooltip("Open Video… (Ctrl+O)");
    auto searchField = SearchField("Search").width(190);
    auto libraryToolbar = Toolbar({titleLabel, Spacer(), ToolbarGroup({addFolderButton, openButton}), searchField});
    auto libraryButton = Button(Icon::ChevronLeft).style(ButtonStyle::Borderless).tooltip("Library (Esc)");
    auto playerToolbar = Toolbar({ToolbarGroup({libraryButton})});

    // Subtitles ------------------------------------------------------------------------------------------
    std::vector<std::unique_ptr<SubtitleTrack>> tracks;
    int activeTrack = -1;
    int lastTrack = 0;
    auto setTrack = [&](int i) {
        activeTrack = i >= 0 && i < static_cast<int>(tracks.size()) ? i : -1;
        if (activeTrack >= 0) lastTrack = activeTrack;
        surface->subtitles = activeTrack >= 0 ? tracks[static_cast<size_t>(activeTrack)].get() : nullptr;
        captions->setActive(activeTrack >= 0);
        surface->invalidate();
    };
    auto nameTracks = [&] {
        std::map<std::string, int> seen;
        for (auto& t : tracks) {
            const int n = ++seen[t->name()];
            if (n > 1) t->setName(t->name() + " (" + std::to_string(n) + ")");
        }
    };
    auto loadSubtitles = [&](const std::string& video) {
        tracks.clear();
        surface->subtitleDelay = 0;
        std::string error;
        for (const auto& f : findSubtitlesFor(video)) {
            auto t = std::make_unique<SubtitleTrack>();
            if (t->load(f, error)) tracks.push_back(std::move(t));
        }
        nameTracks();
        int pick = -1;
        if (prefs.getBool("subtitlesAuto", true) && !tracks.empty()) {
            // Prefer the user's language, then whatever there is.
            pick = 0;
            const std::string mine = userLanguageName();
            for (size_t i = 0; i < tracks.size(); ++i) {
                if (tracks[i]->name().rfind(mine, 0) == 0) {
                    pick = static_cast<int>(i);
                    break;
                }
            }
        }
        setTrack(pick);
    };
    auto addSubtitleFile = [&](const std::string& path) {
        auto t = std::make_unique<SubtitleTrack>();
        std::string error;
        if (!t->load(path, error)) {
            surface->showMessage(error);
            return;
        }
        tracks.push_back(std::move(t));
        nameTracks();
        setTrack(static_cast<int>(tracks.size()) - 1);
        surface->showMessage("Subtitles: " + tracks.back()->name());
    };
    auto shiftSubtitles = [&](double delta) {
        if (!surface->subtitles) return;
        surface->subtitleDelay = std::round((surface->subtitleDelay + delta) * 10) / 10;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Subtitle delay %+.1f s", surface->subtitleDelay);
        surface->showMessage(buf);
    };
    auto chooseSubtitleFile = [&] {
        app.openFiles({{"Subtitles", "srt;vtt"}}, false, [&](std::vector<std::string> paths) {
            if (!paths.empty()) addSubtitleFile(paths[0]);
        }, &window);
    };
    auto captionMenu = std::make_shared<MenuView>(std::vector<MenuItem>{});
    captions->onClick = [&] {
        std::vector<MenuItem> entries{MenuItem("Off", [&] { setTrack(-1); }).setChecked(activeTrack < 0)};
        for (size_t i = 0; i < tracks.size(); ++i)
            entries.push_back(MenuItem(tracks[i]->name(), [&, i] { setTrack(static_cast<int>(i)); }).setChecked(static_cast<int>(i) == activeTrack));
        entries.push_back(MenuItem::separator());
        entries.push_back(MenuItem("Add Subtitle File…", chooseSubtitleFile).withIcon(Icon::Document));
        if (activeTrack >= 0) {
            entries.push_back(MenuItem("Show Earlier", [&] { shiftSubtitles(-0.1); }).withShortcut("G"));
            entries.push_back(MenuItem("Show Later", [&] { shiftSubtitles(0.1); }).withShortcut("H"));
            if (surface->subtitleDelay != 0) entries.push_back(MenuItem("Reset Delay", [&] { shiftSubtitles(-surface->subtitleDelay); }));
        }
        captionMenu->setItems(std::move(entries));
        captionMenu->showFrom(*captions, PopupEdge::Above);
    };

    // Behaviour ----------------------------------------------------------------------------------
    auto showChrome = [&](bool visible) {
        page->setHudVisible(visible);
        window.setChromeHidden(!visible);
    };
    // Any activity shows the controls; they fade out again after a few idle seconds while playing.
    auto poke = [&] {
        if (!playerMode) return;
        const int mine = ++activity;
        showChrome(true);
        app.after(2.8, [&, mine] {
            if (mine == activity && playerMode && player.isPlaying() && !timeline->isDragging() && !captionMenu->isShown())
                showChrome(false);
        });
    };
    page->onActivity = poke;

    auto updatePlayButton = [&] { play->setIcon(player.isPlaying() ? Icon::Pause : Icon::Play); };
    auto togglePlay = [&] {
        if (!player.isOpen()) return;
        player.isPlaying() ? player.pause() : player.play();
        updatePlayButton();
        poke();
    };
    auto seekBy = [&](double delta) {
        if (!player.isOpen()) return;
        player.seek(player.position() + delta);
        poke();
        surface->invalidate();
    };
    auto applyVolume = [&] {
        prefs.set("volume", volume);
        prefs.set("muted", muted);
        player.setVolume(muted ? 0.0f : volume * volume);  // perceptual curve
        speaker->setIcon(muted || volume <= 0.001f ? Icon::SpeakerSlash : Icon::Speaker);
        volumeBar->setValue(muted ? 0.0f : volume);
    };
    // Track the state ourselves: the compositor applies full screen asynchronously.
    bool isFull = false;
    auto toggleFullscreen = [&] {
        isFull = !isFull;
        window.setFullscreen(isFull);
        fullscreen->setIcon(isFull ? Icon::Contract : Icon::Expand);
    };

    // Library ------------------------------------------------------------------------------------------
    auto sectionTitle = [&]() -> std::string {
        if (!query.empty()) return "Search";
        if (section == "all") return "All Videos";
        if (section == "recent") return "Recently Played";
        if (section.rfind("folder:", 0) == 0) return fs::path(section.substr(7)).filename().string();
        return "Home";
    };
    auto probeItems = [&](const std::vector<VideoPtr>& list) {
        for (const auto& v : list) {
            if (v->probed || queued.contains(v.get())) continue;
            queued.insert(v.get());
            prober.probe(v, [&, raw = v.get()] {
                queued.erase(raw);
                grid->invalidate();
            });
        }
    };
    auto existing = [](const std::string& path) {
        std::error_code ec;
        return fs::exists(path, ec);
    };
    std::function<void()> rebuildGallery = [&] {
        std::vector<GallerySection> sections;
        auto byName = [](std::vector<VideoPtr> v) {
            std::sort(v.begin(), v.end(), [](const VideoPtr& a, const VideoPtr& b) { return a->title < b->title; });
            return v;
        };
        if (!query.empty()) {
            std::string q = query;
            std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::vector<VideoPtr> hits;
            std::set<std::string> seen;
            auto consider = [&](const VideoPtr& v) {
                std::string t = v->title;
                std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (t.find(q) != std::string::npos && seen.insert(v->path).second) hits.push_back(v);
            };
            for (const auto& v : library) consider(v);
            for (const auto& h : history) {
                if (existing(h)) consider(itemFor(h));
            }
            sections.push_back({std::to_string(hits.size()) + (hits.size() == 1 ? " Result" : " Results"), byName(hits)});
        } else if (section == "home") {
            std::vector<VideoPtr> resume;
            for (const auto& [_, path] : positions) {
                if (resume.size() < 12 && existing(path)) resume.push_back(itemFor(path));
            }
            std::vector<VideoPtr> recent = library;
            std::sort(recent.begin(), recent.end(), [](const VideoPtr& a, const VideoPtr& b) { return a->modified > b->modified; });
            if (recent.size() > 12) recent.resize(12);
            std::vector<VideoPtr> played;
            for (const auto& h : history) {
                if (played.size() >= 12) break;
                if (positionOf(h) < 0 && existing(h)) played.push_back(itemFor(h));
            }
            sections.push_back({"Continue Watching", resume});
            sections.push_back({"Recently Added", recent});
            sections.push_back({"Recently Played", played});
        } else if (section == "all") {
            sections.push_back({{}, byName(library)});
        } else if (section == "recent") {
            std::vector<VideoPtr> played;
            for (const auto& h : history) {
                if (existing(h)) played.push_back(itemFor(h));
            }
            sections.push_back({{}, played});
        } else if (section.rfind("folder:", 0) == 0) {
            const std::string dir = section.substr(7) + "/";
            std::vector<VideoPtr> inFolder;
            for (const auto& v : library) {
                if (v->path.rfind(dir, 0) == 0) inFolder.push_back(v);
            }
            sections.push_back({{}, byName(inFolder)});
        }
        for (const auto& s : sections) probeItems(s.items);
        grid->setSections(std::move(sections));
        titleLabel.setText(sectionTitle());
        const bool none = grid->isEmpty();
        gridScroll.setHidden(none);
        libEmpty.setHidden(!none);
        if (none) {
            libEmptyTitle.setText(scanning ? "Looking for Videos…" : !query.empty() ? "No Results" : "No Videos");
            libEmptyText.setText(!query.empty()    ? "Nothing in your library matches “" + query + "”."
                                 : section == "recent" ? "Videos you play appear here."
                                                       : "Add a folder with videos, or open a file. Your Videos folder is included automatically.");
        }
    };
    auto rebuildSidebar = [&] {
        std::vector<SidebarItem> s{
            SidebarItem::header("Library"),
            SidebarItem("home", Icon::House, "Home"),
            SidebarItem("all", Icon::Film, "All Videos").withBadge(library.empty() ? "" : std::to_string(library.size())),
            SidebarItem("recent", Icon::Clock, "Recently Played"),
        };
        if (!folders.empty()) s.push_back(SidebarItem::header("Folders"));
        for (const auto& f : folders) {
            const std::string prefix = f + "/";
            const auto n = std::count_if(library.begin(), library.end(), [&](const VideoPtr& v) { return v->path.rfind(prefix, 0) == 0; });
            s.push_back(SidebarItem("folder:" + f, Icon::Folder, fs::path(f).filename().string()).withBadge(n ? std::to_string(n) : ""));
        }
        sidebar.setItems(std::move(s));
        sidebar.selection(query.empty() ? section : "");
    };
    // Folder scans run on a thread (big trees take a while); results replace the library on the UI thread.
    auto rescan = [&] {
        if (scanner.joinable()) scanner.join();
        scanning = true;
        rebuildGallery();
        scanner = std::thread([&app, dirs = folders, &items, &library, &scanning, &rebuildSidebar, &rebuildGallery] {
            std::vector<VideoPtr> found;
            for (const auto& d : dirs) {
                auto part = scanFolder(d);
                found.insert(found.end(), part.begin(), part.end());
            }
            app.post([&, found = std::move(found)] {
                library.clear();
                std::set<std::string> seen;
                for (const auto& v : found) {
                    if (!seen.insert(v->path).second) continue;
                    // Keep already probed items (thumbnails) when their file is unchanged.
                    auto it = items.find(v->path);
                    if (it != items.end() && it->second->size == v->size && it->second->modified == v->modified) {
                        library.push_back(it->second);
                    } else {
                        items[v->path] = v;
                        library.push_back(v);
                    }
                }
                scanning = false;
                rebuildSidebar();
                rebuildGallery();
            });
        });
    };
    auto addFolder = [&] {
        app.openFolder([&](std::vector<std::string> paths) {
            if (paths.empty()) return;
            if (std::find(folders.begin(), folders.end(), paths[0]) == folders.end()) folders.push_back(paths[0]);
            prefs.set("libraryFolders", joinLines(folders));
            section = "folder:" + paths[0];
            rebuildSidebar();
            rescan();
        }, &window);
    };

    auto showLibrary = [&] {
        rememberPosition();
        if (player.isOpen()) player.pause();
        updatePlayButton();
        if (isFull) toggleFullscreen();
        playerMode = false;
        ++activity;
        window.setChromeHidden(false);
        window.setContent(split);
        window.setToolbar(libraryToolbar);
        window.setTitle("Video Player");
        rebuildSidebar();
        rebuildGallery();
        if (auto* h = grid->host()) h->setFocus(grid.get(), false);
    };
    auto showPlayer = [&] {
        if (playerMode) return;
        playerMode = true;
        window.setContent(Element(page));
        window.setToolbar(playerToolbar);
    };

    std::function<bool(const std::string&)> openFile = [&](const std::string& path) {
        rememberPosition();
        showPlayer();
        std::string error;
        surface->reset();
        if (!player.open(path, error)) {
            emptyError.setText(fs::path(path).filename().string() + ": " + error);
            empty.setHidden(false);
            hud->setHidden(true);
            tracks.clear();
            setTrack(-1);
            window.setTitle("Video Player");
            return false;
        }
        empty.setHidden(true);
        hud->setHidden(false);
        surface->audioTitle = player.title();
        if (std::getenv("MAYO_VERBOSE")) {
            // After a second of decoding, so a hardware → software fallback is visible too.
            app.after(1.0, [&] {
                std::fprintf(stderr, "video_player: %s — %s, %d dropped\n", player.codecDescription().c_str(),
                             player.decoderDescription().c_str(), player.droppedFrames());
            });
        }
        window.setTitle(player.title());
        timeline->setValue(0);
        applyVolume();
        currentPath = path;
        std::erase(history, path);
        history.insert(history.begin(), path);
        if (history.size() > 100) history.resize(100);
        prefs.set("history", joinLines(history));
        loadSubtitles(path);
        if (prefs.getBool("resumePlayback", true)) {
            if (const double at = positionOf(path); at > 0) player.seek(at);
        }
        player.play();
        updatePlayButton();
        poke();
        surface->invalidate();
        return true;
    };
    auto openPaths = [&](const std::vector<std::string>& paths) {
        std::vector<std::string> media;
        std::vector<std::string> subs;
        for (const auto& p : paths) (isSubtitleFile(p) ? subs : media).push_back(p);
        if (!media.empty()) {
            playlist.assign(media.begin() + 1, media.end());
            openFile(media.front());
        }
        if (player.isOpen() && playerMode) {
            for (const auto& s : subs) addSubtitleFile(s);
        }
    };
    const std::vector<FileFilter> filters{
        {"Movies & audio", "mp4;m4v;mov;mkv;webm;avi;ogv;mpg;mpeg;ts;m2ts;wmv;flv;3gp;mp3;m4a;aac;flac;wav;ogg;opus"},
        {"Subtitles", "srt;vtt"},
        {"All files", "*"}};
    auto chooseFile = [&] { app.openFiles(filters, true, openPaths, &window); };

    grid->progress = [&](const VideoPtr& v) {
        const double at = positionOf(v->path);
        return at > 0 && v->duration > 0 ? at / v->duration : -1.0;
    };
    grid->onOpen = [&](const VideoPtr& v) {
        playlist.clear();
        openFile(v->path);
    };
    grid->onContextMenu = [&](const VideoPtr& v, Point at) {
        const std::string path = v->path;
        std::vector<MenuItem> menu{MenuItem("Play", [&, path] { openFile(path); }).withIcon(Icon::Play)};
        if (positionOf(path) > 0) {
            menu.push_back(MenuItem("Play from Beginning", [&, path] {
                std::erase_if(positions, [&](const auto& e) { return e.second == path; });
                savePositions();
                openFile(path);
            }));
        }
        menu.push_back(MenuItem::separator());
        menu.push_back(MenuItem("Show in Files", [path, demo] {
                           if (!demo) showInFiles(fs::path(path).parent_path().string());
                       }).withIcon(Icon::Folder));
        if (std::find(history.begin(), history.end(), path) != history.end()) {
            menu.push_back(MenuItem("Remove from Recently Played", [&, path] {
                std::erase(history, path);
                std::erase_if(positions, [&](const auto& e) { return e.second == path; });
                prefs.set("history", joinLines(history));
                savePositions();
                rebuildGallery();
            }));
        }
        contextMenu->setItems(std::move(menu));
        menuAnchor->arrange({at.x, at.y, 1, 1});
        contextMenu->showFrom(*menuAnchor);
    };
    sidebar.onSelect([&](const std::string& id) {
        section = id;
        prefs.set("librarySection", id);
        if (!query.empty()) {
            query.clear();
            searchField.setText("");
        }
        rebuildGallery();
    });
    searchField.onChange([&](const std::string& q) {
        query = q;
        sidebar.selection(q.empty() ? section : "");
        rebuildGallery();
    });
    addFolderButton.onClick(addFolder);
    libAddFolder.onClick(addFolder);
    openButton.onClick(chooseFile);
    libOpen.onClick(chooseFile);
    libraryButton.onClick(showLibrary);
    backToLibrary.onClick(showLibrary);

    play->onClick = togglePlay;
    back->onClick = [&] { seekBy(-10); };
    forward->onClick = [&] { seekBy(10); };
    speaker->onClick = [&] {
        muted = !muted;
        applyVolume();
    };
    volumeBar->onChange = [&](float v, bool) {
        volume = v;
        muted = false;
        applyVolume();
    };
    loop->setActive(looping);
    loop->onClick = [&] {
        looping = !looping;
        prefs.set("loop", looping);
        loop->setActive(looping);
    };
    fullscreen->onClick = toggleFullscreen;

    // Playback info: what decodes the video, and whether frames are being dropped.
    auto infoTitle = Label("").font(TextStyle::Headline).lineLimit(1).maxWidth(320);
    auto infoCodec = Label("").secondary();
    auto infoDecoder = Label("");
    auto infoStats = Label("").secondary();
    auto infoSubs = Label("").secondary();
    auto info = Popover(VStack({infoTitle, infoCodec, infoDecoder, infoStats, infoSubs}).spacing(4));
    auto refreshInfo = [&] {
        infoTitle.setText(player.title());
        std::string codec = player.codecDescription();
        if (player.frameRate() > 0) {
            char fps[32];
            std::snprintf(fps, sizeof fps, " · %.3g fps", player.frameRate());
            codec += fps;
        }
        infoCodec.setText(codec);
        infoDecoder.setText(player.decoderDescription());
        infoStats.setText(std::to_string(player.droppedFrames()) + " dropped frames");
        infoSubs.setText(surface->subtitles ? "Subtitles: " + surface->subtitles->name() + " · " +
                                                  std::to_string(surface->subtitles->cues().size()) + " lines"
                                            : "No subtitles");
    };
    infoButton->onClick = [&] {
        refreshInfo();
        info.show(Element(infoButton), PopupEdge::Above);
    };
    timeline->hoverLabel = [&](float f) { return formatTime(f * player.duration()); };
    timeline->onChange = [&](float v, bool /*final*/) {
        player.seek(v * player.duration());
        surface->invalidate();
    };
    surface->onClick = togglePlay;
    surface->onDoubleClick = [&] {
        togglePlay();  // undo the first click's toggle
        toggleFullscreen();
    };
    surface->onTick = [&] {
        const double pos = player.position();
        const double dur = player.duration();
        if (!timeline->isDragging() && dur > 0) timeline->setValue(static_cast<float>(pos / dur));
        std::string e = formatTime(pos);
        std::string r = "-" + formatTime(std::max(0.0, dur - pos));
        if (e != elapsed.text()) elapsed.setText(e);
        if (r != remaining.text()) remaining.setText(r);
        if (info.isShown()) refreshInfo();
        if (player.isPlaying() && player.ended()) {
            // Defer: switching files from inside a draw would re-enter layout.
            app.post([&] {
                if (!player.ended()) return;
                if (looping) {
                    player.seek(0);
                } else if (!playlist.empty()) {
                    std::string next = playlist.front();
                    playlist.pop_front();
                    openFile(next);
                } else {
                    player.pause();
                    // Finished: nothing to resume any more.
                    std::erase_if(positions, [&](const auto& e2) { return e2.second == currentPath; });
                    savePositions();
                    updatePlayButton();
                    showChrome(true);
                }
            });
        }
    };

    // Settings window (Ctrl+,) ------------------------------------------------------------------------
    Window* settingsWindow = nullptr;
    auto openSettings = [&] {
        if (settingsWindow) {
            settingsWindow->raise();
            return;
        }
        auto& w = app.createWindow({.title = "Video Player Settings", .width = 560, .height = 600, .minWidth = 460, .minHeight = 320});
        auto resume = Toggle("", prefs.getBool("resumePlayback", true)).onChange([&](bool on) {
            prefs.set("resumePlayback", on);
            if (!on) {
                positions.clear();  // forget where files were left
                savePositions();
            }
        });
        auto hardware = Toggle("", prefs.getBool("hardwareDecoding", true)).onChange([&](bool on) {
            prefs.set("hardwareDecoding", on);
            player.setHardwareDecoding(on);
        });
        auto autoSubs = Toggle("", prefs.getBool("subtitlesAuto", true)).onChange([&](bool on) { prefs.set("subtitlesAuto", on); });
        const float sizes[] = {0.8f, 1.0f, 1.25f, 1.5f};
        int sizeIndex = 1;
        for (int i = 0; i < 4; ++i) {
            if (std::abs(sizes[i] - surface->style.scale) < 0.01f) sizeIndex = i;
        }
        auto subSize = Picker({"Small", "Medium", "Large", "Extra Large"}, sizeIndex).onChange([&, sizes](int i) {
            surface->style.scale = sizes[i];
            prefs.set("subtitleSize", sizes[i]);
            surface->invalidate();
        });
        auto subBackground = Toggle("", surface->style.background).onChange([&](bool on) {
            surface->style.background = on;
            prefs.set("subtitleBackground", on);
            surface->invalidate();
        });
        auto folderList = VStack({}).spacing(6);
        std::function<void()> fillFolders = [&, folderList]() mutable {
            folderList.clear();
            for (const auto& f : folders) {
                auto remove = Button(Icon::Minus).style(ButtonStyle::Borderless).tooltip("Remove from Library");
                remove.onClick([&, f] {
                    std::erase(folders, f);
                    prefs.set("libraryFolders", joinLines(folders));
                    if (section == "folder:" + f) section = "home";
                    rebuildSidebar();
                    rescan();
                    app.post([&] { if (settingsWindow) settingsWindow->close(); });
                });
                folderList.add(HStack({Label(f).lineLimit(1), Spacer(), remove}).spacing(8).align(Alignment::Center));
            }
            if (folders.empty()) folderList.add(Label("No folders").secondary());
        };
        fillFolders();
        auto form = Form({
            AppearanceSettings(app),
            FormSection("Playback",
                        {FormRow("Resume where you left off", resume, "Reopened videos continue from the last position"),
                         FormRow("Hardware decoding", hardware, "Takes effect with the next video")}),
            FormSection("Subtitles",
                        {FormRow("Load automatically", autoSubs, "Subtitle files named like the video (Movie.srt, Movie.en.srt)"),
                         FormRow("Size", subSize), FormRow("Background", subBackground, "A dark box behind each line")}),
            FormSection("Library", {FormRow(folderList), FormRow(HStack({Spacer(), Button(Icon::FolderPlus, "Add Folder…").onClick(addFolder)}))},
                        "Folders are searched for videos, four levels deep."),
        });
        w.setContent(ScrollView(VStack({form}).padding(Insets(12, 24, 24, 24))));
        w.onKey([&w](const KeyEvent& e) {
            if (e.key == Key::Escape || (e.primary() && e.key == Key::W)) {
                w.close();
                return true;
            }
            return false;
        });
        w.onClosed([&] { settingsWindow = nullptr; });
        settingsWindow = &w;
    };
    settingsButton->onClick = openSettings;

    window.onFilesDropped(openPaths);
    window.onKey([&](const KeyEvent& e) {
        poke();
        if (e.primary() && e.key == Key::O) return chooseFile(), true;
        if (e.primary() && e.key == Key::Comma) return openSettings(), true;
        if (!playerMode) {
            if (e.primary() && e.key == Key::F) {
                if (auto* h = searchField->host()) h->setFocus(searchField.node().get(), true);
                return true;
            }
            if (e.key == Key::Escape && !query.empty()) {
                searchField.setText("");
                query.clear();
                rebuildGallery();
                return true;
            }
            return false;
        }
        switch (e.key) {
            case Key::Space:
            case Key::K: togglePlay(); return true;
            case Key::Left: seekBy(-5); return true;
            case Key::Right: seekBy(5); return true;
            case Key::J: seekBy(-10); return true;
            case Key::L: seekBy(10); return true;
            case Key::Home: seekBy(-1e9); return true;
            case Key::Up:
                volume = std::min(1.0f, volume + 0.1f);
                muted = false;
                applyVolume();
                return true;
            case Key::Down:
                volume = std::max(0.0f, volume - 0.1f);
                applyVolume();
                return true;
            case Key::M:
                muted = !muted;
                applyVolume();
                return true;
            case Key::F: toggleFullscreen(); return true;
            case Key::C:
                if (tracks.empty()) {
                    surface->showMessage("No subtitles — drop an .srt file here");
                } else {
                    setTrack(activeTrack >= 0 ? -1 : lastTrack);
                    surface->showMessage(activeTrack >= 0 ? "Subtitles: " + tracks[static_cast<size_t>(activeTrack)]->name()
                                                          : "Subtitles off");
                }
                return true;
            case Key::V:
                if (!tracks.empty()) {
                    setTrack(activeTrack + 1 >= static_cast<int>(tracks.size()) ? -1 : activeTrack + 1);
                    surface->showMessage(activeTrack >= 0 ? "Subtitles: " + tracks[static_cast<size_t>(activeTrack)]->name() : "Subtitles off");
                }
                return true;
            case Key::G: shiftSubtitles(-0.1); return true;
            case Key::H: shiftSubtitles(0.1); return true;
            case Key::Escape:
                if (isFull) toggleFullscreen();
                else showLibrary();
                return true;
            default: return false;
        }
    });

    hud->setHidden(true);
    applyVolume();
    window.setContent(split);
    window.setToolbar(libraryToolbar);
    rebuildSidebar();
    rescan();

    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty()) openPaths(args);

    // MAYO_DEMO=seek:<seconds> jumps there and pauses (screenshots, with a file argument);
    // MAYO_DEMO=settings opens the Settings window; MAYO_DEMO=all|recent|home picks a library view.
    if (const char* env = std::getenv("MAYO_DEMO")) {
        std::string d = env;
        if (d == "settings") app.after(0.3, openSettings);
        if (d == "all" || d == "recent" || d == "home") {
            section = d;
            rebuildSidebar();
            rebuildGallery();
        }
        if (d == "captions") app.after(0.6, [&] { captions->onClick(); });
        if (d.rfind("seek:", 0) == 0) {
            double t = std::atof(d.c_str() + 5);
            app.after(0.3, [&, t] {
                player.pause();
                player.seek(t);
                updatePlayButton();
                surface->invalidate();
            });
        }
    }
    const int code = app.run();
    rememberPosition();  // saved with the other settings when the App goes away
    if (scanner.joinable()) scanner.join();
    return code;
}
