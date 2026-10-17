// Photos — an image library and viewer built on Mayonnaise.
//
//   photos [files or folders...]
//
// Import with the toolbar's Import menu (Ctrl+O / Ctrl+Shift+O), by dropping files onto the window, or
// from the command line. Double-click (or Enter / Space) opens a photo; arrows move, Escape goes back,
// pinch or Ctrl+scroll zooms, F marks a favourite.
#include "../common/zh_cn.hpp"
#include <mayonnaise/mayonnaise.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace mayo;

namespace {

constexpr int kThumbnailSize = 480;   // pixels; big enough for the largest grid tiles on HiDPI
constexpr int kFullSize = 8192;       // cap for the viewer (GPU limits, memory)

// --- Model ---------------------------------------------------------------------------------------

struct Photo {
    std::string path;
    std::string name;
    std::string folder;
    std::uintmax_t bytes = 0;
    std::shared_ptr<ImageData> thumb;
    std::shared_ptr<ImageData> full;
    int width = 0;   // original, upright
    int height = 0;
    bool favorite = false;
    bool failed = false;
    bool fullRequested = false;
};
using PhotoPtr = std::shared_ptr<Photo>;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isImageFile(const fs::path& p) {
    static const std::set<std::string> kExt = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tga"};
    return kExt.count(lower(p.extension().string())) > 0;
}

/// Settings values are strings; lists of paths are stored one per line.
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

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += (out.empty() ? "" : "\n") + l;
    return out;
}

std::string formatBytes(std::uintmax_t b) {
    char buf[32];
    if (b >= 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / 1e6);
    else std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(b) / 1e3);
    return buf;
}

// --- Background decoding -------------------------------------------------------------------------

/// Decodes images on worker threads; results are applied on the UI thread via App::post.
class Loader {
public:
    Loader(App& app, unsigned threads) : m_app(app) {
        for (unsigned i = 0; i < threads; ++i) m_threads.emplace_back([this] { run(); });
    }
    ~Loader() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) t.join();
    }
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    void thumbnail(PhotoPtr photo, std::function<void()> done) { enqueue({std::move(photo), false, std::move(done)}, false); }
    /// Full-resolution images jump the queue: the user is looking at them.
    void full(PhotoPtr photo, std::function<void()> done) { enqueue({std::move(photo), true, std::move(done)}, true); }

private:
    struct Job {
        PhotoPtr photo;
        bool full;
        std::function<void()> done;
    };

    void enqueue(Job job, bool front) {
        {
            std::lock_guard lock(m_mutex);
            if (front) m_queue.push_front(std::move(job));
            else m_queue.push_back(std::move(job));
        }
        m_cv.notify_one();
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            // Only the immutable path is read here; the photo itself is updated on the UI thread.
            auto image = ImageData::load(job.photo->path, job.full ? kFullSize : kThumbnailSize);
            m_app.post([job = std::move(job), image] {
                if (!image) {
                    job.photo->failed = true;
                } else {
                    job.photo->width = image->originalWidth();
                    job.photo->height = image->originalHeight();
                    (job.full ? job.photo->full : job.photo->thumb) = image;
                }
                if (job.done) job.done();
            });
        }
    }

    App& m_app;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
};

/// Source rect (normalised) that crops an image to a square-ish target like object-fit: cover.
Rect coverSource(float imageW, float imageH, float targetW, float targetH) {
    if (imageW <= 0 || imageH <= 0) return {0, 0, 1, 1};
    float k = std::max(targetW / imageW, targetH / imageH);
    float sw = std::min(1.0f, targetW / (imageW * k));
    float sh = std::min(1.0f, targetH / (imageH * k));
    return {(1 - sw) * 0.5f, (1 - sh) * 0.5f, sw, sh};
}

// --- Grid ----------------------------------------------------------------------------------------

class PhotoGridView : public Widget {
public:
    std::function<void(int)> onOpen;
    std::function<void(int)> onRemove;
    std::function<void(int)> onToggleFavorite;
    std::function<void(float)> onTileSizeChanged;

    void setScroller(ScrollViewView* s) { m_scroller = s; }
    void setPhotos(std::vector<PhotoPtr> photos) {
        m_photos = std::move(photos);
        m_selected = m_photos.empty() ? -1 : std::clamp(m_selected, 0, static_cast<int>(m_photos.size()) - 1);
        invalidateLayout();
    }
    [[nodiscard]] const std::vector<PhotoPtr>& photos() const { return m_photos; }
    void setTileSize(float s) {
        m_tile = std::clamp(s, 80.0f, 320.0f);
        invalidateLayout();
    }
    [[nodiscard]] float tileSize() const { return m_tile; }
    void select(int i) {
        if (m_photos.empty()) return;
        m_selected = std::clamp(i, 0, static_cast<int>(m_photos.size()) - 1);
        ensureVisible(m_selected);
        invalidate();
    }
    [[nodiscard]] int selected() const { return m_selected; }

    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }

    Size measureContent(Size available) override {
        float w = std::isfinite(available.width) ? available.width : 800.0f;
        computeColumns(w);
        auto rows = static_cast<float>((m_photos.size() + static_cast<size_t>(m_cols) - 1) / static_cast<size_t>(m_cols));
        float h = kPad * 2 + rows * m_cell + std::max(0.0f, rows - 1) * kGap + kFooter;
        return {w, h};
    }

    void draw(Painter& p) override {
        const Theme& t = theme();
        computeColumns(frame().width);
        const Rect clip = p.clipBounds();
        const float radius = std::clamp(m_cell * 0.06f, 4.0f, 10.0f);
        for (int i = 0; i < static_cast<int>(m_photos.size()); ++i) {
            Rect r = tileRect(i);
            if (!r.outset(6).intersects(clip)) continue;
            const Photo& ph = *m_photos[static_cast<size_t>(i)];
            if (ph.thumb) {
                Rect src = coverSource(static_cast<float>(ph.thumb->width()), static_cast<float>(ph.thumb->height()),
                                       r.width, r.height);
                p.drawImage(r, *ph.thumb, radius, 1.0f, src);
            } else {
                p.fillRoundedRect(r, radius, t.colors.fill);
                if (ph.failed) p.drawIcon(Icon::Xmark, r.inset(r.width * 0.4f), t.colors.tertiaryLabel);
            }
            if (i == m_hovered && i != m_selected) p.fillRoundedRect(r, radius, Color::white(0.12f));
            if (ph.favorite) {
                Rect heart{r.x + 7, r.bottom() - 21, 14, 14};
                p.drawShadow(heart.inset(2), 6, Color::black(0.5f), 6);
                p.drawIcon(Icon::HeartFill, heart, Color::white());
            }
            if (i == m_selected) {
                Color ring = isFocused() ? t.colors.accent : t.colors.accent.withAlpha(0.7f);
                p.strokeRoundedRect(r.outset(3.5f), radius + 3.5f, ring, 3.0f);
            }
        }
        if (!m_photos.empty()) {
            const Font f = t.font(TextStyle::Callout).withWeight(FontWeight::Medium);
            std::string n = std::to_string(m_photos.size()) + (m_photos.size() == 1 ? " Photo" : " Photos");
            Rect footer{frame().x, frame().bottom() - kPad - kFooter + 10, frame().width, kFooter - 10};
            p.drawText(n, footer, f, t.colors.secondaryLabel, TextAlign::Center);
        }
    }

    bool onPointerDown(const PointerEvent& e) override {
        if (auto* h = host()) h->setFocus(this, false);
        int i = tileAt(e.position);
        if (i < 0) return true;
        select(i);
        if (e.clickCount >= 2 && onOpen) onOpen(i);
        return true;
    }
    void onPointerMove(const PointerEvent& e) override {
        int i = tileAt(e.position);
        if (i != m_hovered) {
            m_hovered = i;
            invalidate();
        }
    }
    void onPointerLeave() override {
        m_hovered = -1;
        invalidate();
    }
    bool onMagnify(const MagnifyEvent& e) override {
        resize(m_tile * e.scale);
        return true;
    }
    bool onScroll(const ScrollEvent& e) override {
        // Plain scrolling belongs to the ScrollView; Ctrl+scroll resizes tiles.
        if (!hasModifier(e.modifiers, Modifiers::Ctrl)) return false;
        resize(m_tile * std::pow(1.0015f, -e.delta.y));
        return true;
    }
    bool onKeyDown(const KeyEvent& e) override {
        if (m_photos.empty()) return false;
        const int n = static_cast<int>(m_photos.size());
        switch (e.key) {
            case Key::Left: select(m_selected - 1); return true;
            case Key::Right: select(m_selected + 1); return true;
            case Key::Up: select(m_selected - m_cols); return true;
            case Key::Down: select(std::min(n - 1, m_selected + m_cols)); return true;
            case Key::Home: select(0); return true;
            case Key::End: select(n - 1); return true;
            case Key::Enter:
            case Key::Space:
                if (m_selected >= 0 && onOpen) onOpen(m_selected);
                return true;
            case Key::F:
                if (m_selected >= 0 && onToggleFavorite) onToggleFavorite(m_selected);
                return true;
            case Key::Backspace:
            case Key::Delete:
                if (m_selected >= 0 && onRemove) onRemove(m_selected);
                return true;
            default: return false;
        }
    }

private:
    static constexpr float kPad = 18;
    static constexpr float kGap = 6;
    static constexpr float kFooter = 44;

    void resize(float size) {
        setTileSize(size);
        if (onTileSizeChanged) onTileSizeChanged(m_tile);
    }

    void computeColumns(float width) {
        float inner = std::max(1.0f, width - kPad * 2);
        m_cols = std::max(1, static_cast<int>((inner + kGap) / (m_tile + kGap)));
        m_cell = (inner - kGap * static_cast<float>(m_cols - 1)) / static_cast<float>(m_cols);
    }

    [[nodiscard]] Rect tileRect(int i) const {
        int col = i % m_cols;
        int row = i / m_cols;
        return {frame().x + kPad + static_cast<float>(col) * (m_cell + kGap),
                frame().y + kPad + static_cast<float>(row) * (m_cell + kGap), m_cell, m_cell};
    }

    [[nodiscard]] int tileAt(Point pt) const {
        for (int i = 0; i < static_cast<int>(m_photos.size()); ++i) {
            if (tileRect(i).contains(pt)) return i;
        }
        return -1;
    }

    void ensureVisible(int i) {
        if (!m_scroller || i < 0) return;
        const float inset = host() ? host()->titleBarHeight() : 0.0f;
        const Rect view = m_scroller->frame();
        const float top = tileRect(i).y - frame().y - kGap;  // in content coordinates
        const float bottom = top + m_cell + kGap * 2;
        float offset = m_scroller->offset().y;
        const float visible = view.height - inset;
        if (top < offset) offset = top;
        if (bottom > offset + visible) offset = bottom - visible;
        if (offset != m_scroller->offset().y) m_scroller->scrollTo({0, offset}, true);
    }

    std::vector<PhotoPtr> m_photos;
    ScrollViewView* m_scroller = nullptr;
    float m_tile = 170;
    float m_cell = 170;
    int m_cols = 1;
    int m_selected = 0;
    int m_hovered = -1;
};

// --- Viewer --------------------------------------------------------------------------------------

class PhotoViewerView : public Widget {
public:
    std::function<void(int)> onNavigate;  // asks the app to show another index
    std::function<void()> onClose;
    std::function<void()> onToggleFavorite;

    PhotoViewerView() {
        auto button = [](Icon icon, const char* tip) { return Button(icon).style(ButtonStyle::Borderless).tooltip(tip); };
        auto prev = button(Icon::ChevronLeft, "Previous (←)").onClick([this] { step(-1); });
        auto next = button(Icon::ChevronRight, "Next (→)").onClick([this] { step(1); });
        auto out = button(Icon::ZoomOut, "Zoom Out (−)").onClick([this] { zoomBy(1 / 1.5f, true); });
        auto in = button(Icon::ZoomIn, "Zoom In (+)").onClick([this] { zoomBy(1.5f, true); });
        m_zoomLabel = Label("Fit").font(TextStyle::Callout).textAlign(TextAlign::Center).width(52);
        m_controls = ToolbarGroup({prev, out, m_zoomLabel, in, next}).node();
        m_controls->setPadding(Insets(4, 8));
        addChild(m_controls);
        m_spinner = ProgressSpinner().node();
        addChild(m_spinner);
    }

    void show(std::vector<PhotoPtr> photos, int index) {
        m_photos = std::move(photos);
        m_index = std::clamp(index, 0, std::max(0, static_cast<int>(m_photos.size()) - 1));
        resetZoom(false);
        m_fade.set(0.0f);
        m_fade.animateTo(1.0f, now(), Transition{0.18f, Curve::EaseOut});
        invalidateLayout();
    }
    [[nodiscard]] PhotoPtr current() const {
        return m_index >= 0 && m_index < static_cast<int>(m_photos.size()) ? m_photos[static_cast<size_t>(m_index)] : nullptr;
    }
    [[nodiscard]] int index() const { return m_index; }
    void photoUpdated() { invalidate(); }

    [[nodiscard]] bool acceptsFocus() const override { return true; }
    [[nodiscard]] bool isInteractive() const override { return true; }

    void arrangeContent(const Rect& c) override {
        Size s = m_controls->measure(c.size());
        m_controls->arrange({c.center().x - s.width * 0.5f, c.bottom() - s.height - 22, s.width, s.height});
        Size sp = m_spinner->measure(c.size());
        float top = host() ? host()->titleBarHeight() : 0.0f;
        m_spinner->arrange({c.right() - sp.width - 18, c.y + top + 12, sp.width, sp.height});
    }

    void draw(Painter& p) override {
        m_deviceScale = p.scale();
        auto ph = current();
        m_spinner->setHidden(!ph || ph->full || ph->failed);
        if (ph) {
            const Rect view = imageArea();
            const float s = sample(m_scaleAnim);
            const Vec2 c = sample(m_centerAnim);
            if (!m_fitted && ph->width > 0) clampCenter();
            const auto image = ph->full ? ph->full : ph->thumb;
            if (image && ph->width > 0) {
                Rect r{view.center().x - c.x * s, view.center().y - c.y * s, static_cast<float>(ph->width) * s,
                       static_cast<float>(ph->height) * s};
                p.pushClip(frame());
                p.pushOpacity(std::clamp(sample(m_fade), 0.0f, 1.0f));
                p.drawShadow(r, 0, Color::black(theme().isDark() ? 0.5f : 0.18f), 24, {0, 6});
                p.drawImage(r, *image);
                p.popOpacity();
                p.popClip();
            } else if (ph->failed) {
                p.drawText("Can't open this image", view, theme().font(TextStyle::Title3), theme().colors.secondaryLabel,
                           TextAlign::Center);
            }
            updateZoomLabel(s);
        }
        paintChildren(p);
    }

    bool onPointerDown(const PointerEvent& e) override {
        if (auto* h = host()) h->setFocus(this, false);
        if (e.clickCount == 2) {
            // Toggle between fitting and 100% at the clicked point.
            if (m_fitted) zoomTo(actualScale(), e.position, true);
            else resetZoom(true);
            return true;
        }
        m_lastDrag = e.position;
        return true;
    }
    void onPointerMove(const PointerEvent& e) override {
        if (!isPressed() && !m_dragging) return;
        m_dragging = true;
        Vec2 d = e.position - m_lastDrag;
        m_lastDrag = e.position;
        panBy(d);
    }
    void onPointerUp(const PointerEvent& /*e*/) override { m_dragging = false; }

    bool onMagnify(const MagnifyEvent& e) override {
        zoomTo(currentScale() * e.scale, e.position, false);
        return true;
    }

    bool onScroll(const ScrollEvent& e) override {
        if (!e.precise || hasModifier(e.modifiers, Modifiers::Ctrl)) {
            // Mouse wheel (or Ctrl + touchpad) zooms around the pointer; 40pt = one wheel notch.
            zoomTo(currentScale() * std::pow(1.25f, -e.delta.y / 40.0f), e.position, !e.precise);
            return true;
        }
        if (!m_fitted) {
            panBy(-e.delta);
            return true;
        }
        // Two-finger horizontal swipe at fit size flips through photos, once per gesture.
        double t = now();
        if (t - m_swipeTime > 0.35) {
            m_swipe = 0;
            m_swipeFired = false;
        }
        m_swipeTime = t;
        m_swipe += e.delta.x;
        if (std::abs(m_swipe) > 90 && !m_swipeFired) {
            m_swipeFired = true;
            step(m_swipe > 0 ? 1 : -1);
        }
        return true;
    }

    bool onKeyDown(const KeyEvent& e) override {
        switch (e.key) {
            case Key::Left: step(-1); return true;
            case Key::Right: step(1); return true;
            case Key::Escape:
                if (onClose) onClose();
                return true;
            case Key::Num0: resetZoom(true); return true;
            case Key::Num1: zoomTo(actualScale(), imageArea().center(), true); return true;
            case Key::F:
                if (onToggleFavorite) onToggleFavorite();
                return true;
            default: break;
        }
        // '+' arrives as Shift+'=' on most layouts; accept both and the minus key via text below.
        return false;
    }
    bool onTextInput(const TextInputEvent& e) override {
        if (e.text == "+" || e.text == "=") zoomBy(1.5f, true);
        else if (e.text == "-") zoomBy(1 / 1.5f, true);
        else return false;
        return true;
    }

private:
    [[nodiscard]] Rect imageArea() const {
        // Leave room for the toolbar above and the control bar below.
        float top = host() ? host()->titleBarHeight() : 0.0f;
        return frame().inset(Insets(top + 12, 24, 76, 24));
    }
    [[nodiscard]] float fitScale() const {
        auto ph = current();
        if (!ph || ph->width <= 0) return 1;
        Rect v = imageArea();
        float s = std::min(v.width / static_cast<float>(ph->width), v.height / static_cast<float>(ph->height));
        // Small images aren't blown up when fitting, like Preview.
        return std::min(s, actualScale());
    }
    /// One image pixel per device pixel ("100%").
    [[nodiscard]] float actualScale() const { return 1.0f / m_deviceScale; }
    [[nodiscard]] float currentScale() const { return m_fitted ? fitScale() : m_scaleAnim.target(); }

    void step(int delta) {
        if (m_photos.empty()) return;
        int i = std::clamp(m_index + delta, 0, static_cast<int>(m_photos.size()) - 1);
        if (i != m_index && onNavigate) onNavigate(i);
    }

    void resetZoom(bool animated) {
        m_fitted = true;
        auto ph = current();
        Vec2 center = ph ? Vec2{static_cast<float>(ph->width) * 0.5f, static_cast<float>(ph->height) * 0.5f} : Vec2{};
        setView(fitScale(), center, animated);
    }

    void zoomBy(float factor, bool animated) { zoomTo(currentScale() * factor, imageArea().center(), animated); }

    void zoomTo(float scale, Point anchor, bool animated) {
        auto ph = current();
        if (!ph || ph->width <= 0) return;
        const float fit = fitScale();
        scale = std::clamp(scale, fit, std::max(fit, actualScale() * 8));
        if (scale <= fit * 1.001f) {
            resetZoom(animated);
            return;
        }
        // Keep the image point under the anchor fixed.
        const Rect view = imageArea();
        const float s = currentScale();
        const Vec2 c = m_fitted ? Vec2{static_cast<float>(ph->width) * 0.5f, static_cast<float>(ph->height) * 0.5f}
                                : m_centerAnim.target();
        const Vec2 under = c + (anchor - view.center()) / s;
        m_fitted = false;
        setView(scale, under - (anchor - view.center()) / scale, animated);
    }

    void panBy(Vec2 delta) {
        if (m_fitted) return;
        setView(m_scaleAnim.target(), m_centerAnim.target() - delta / m_scaleAnim.target(), false);
    }

    void setView(float scale, Vec2 center, bool animated) {
        center = clamped(scale, center);
        if (animated) {
            m_scaleAnim.animateTo(scale, now(), Transition{0.25f, Curve::EaseOut});
            m_centerAnim.animateTo(center, now(), Transition{0.25f, Curve::EaseOut});
        } else {
            m_scaleAnim.set(scale);
            m_centerAnim.set(center);
        }
        invalidate();
    }

    [[nodiscard]] Vec2 clamped(float s, Vec2 c) const {
        auto ph = current();
        if (!ph) return c;
        const Rect v = imageArea();
        auto axis = [&](float value, float size, float viewLen) {
            float half = viewLen / (2 * s);
            if (size * s <= viewLen) return size * 0.5f;
            return std::clamp(value, half, size - half);
        };
        return {axis(c.x, static_cast<float>(ph->width), v.width), axis(c.y, static_cast<float>(ph->height), v.height)};
    }

    void clampCenter() {
        if (m_scaleAnim.isAnimating(now())) return;
        Vec2 c = clamped(m_scaleAnim.target(), m_centerAnim.target());
        if (c != m_centerAnim.target()) m_centerAnim.set(c);
    }

    void updateZoomLabel(float s) {
        if (m_fitted && !m_scaleAnim.isAnimating(now())) {
            // Fit scale depends on the window size; keep it current when resizing.
            float fit = fitScale();
            if (std::abs(fit - m_scaleAnim.target()) > 1e-4f) setView(fit, m_centerAnim.target(), false);
        }
        std::string text = m_fitted ? "Fit" : std::to_string(static_cast<int>(std::lround(s / actualScale() * 100))) + "%";
        if (text != m_zoomLabel.text()) m_zoomLabel.setText(text);
    }

    std::vector<PhotoPtr> m_photos;
    int m_index = 0;
    bool m_fitted = true;
    bool m_dragging = false;
    Point m_lastDrag;
    Animated<float> m_scaleAnim{1.0f};
    Animated<Vec2> m_centerAnim;
    Animated<float> m_fade{1.0f};
    float m_swipe = 0;
    double m_swipeTime = 0;
    bool m_swipeFired = false;
    float m_deviceScale = 1;
    std::shared_ptr<Widget> m_controls;
    std::shared_ptr<Widget> m_spinner;
    Label m_zoomLabel;
};

/// Detail area: grid, viewer, or empty state. Runs under the toolbar so content slides beneath it.
class PhotosPage : public Widget {
public:
    std::shared_ptr<ScrollViewView> gridScroll;
    std::shared_ptr<PhotoViewerView> viewer;
    std::shared_ptr<Widget> empty;

    [[nodiscard]] bool extendsIntoTitleBar() const override { return true; }
    void arrangeContent(const Rect& c) override {
        const float bar = host() ? host()->titleBarHeight() : 0.0f;
        gridScroll->setContentInsetTop(bar);
        gridScroll->measure(c.size());
        gridScroll->arrange(c);
        viewer->measure(c.size());
        viewer->arrange(c);
        Rect below{c.x, c.y + bar, c.width, c.height - bar};
        empty->measure(below.size());
        empty->arrange(below);
    }
    [[nodiscard]] bool expandsContent(Axis /*axis*/) const override { return true; }
};

}  // namespace

int main(int argc, char** argv) {
    mayo::setUiTranslator(forma::zhCN);
    App app("photos", MAYO_DESKTOP_ID);
    auto& window = app.createWindow(
        {.title = "照片", .width = 1120, .height = 760, .minWidth = 700, .minHeight = 460, .autosaveName = "main"});
    Settings& prefs = app.settings();
    // Favourites outlive the library: a re-imported photo is still a favourite.
    std::set<std::string> favoritePaths;
    for (auto& p : splitLines(prefs.getString("favorites"))) favoritePaths.insert(std::move(p));
    Loader loader(app, std::clamp(std::thread::hardware_concurrency(), 2u, 6u) - 1);

    std::vector<PhotoPtr> library;
    std::vector<std::string> folders;
    std::string section = "all";
    std::string query;

    // Widgets ---------------------------------------------------------------------------------
    auto grid = std::make_shared<PhotoGridView>();
    auto gridScroll = ScrollView(Element(grid));
    grid->setScroller(gridScroll.node().get());
    auto viewer = std::make_shared<PhotoViewerView>();
    viewer->setHidden(true);

    auto emptyTitle = Label("No Photos").font(TextStyle::Title2);
    auto emptyText = Label("Drop images or folders here, or import them.").secondary();
    auto openImagesButton = Button(Icon::Photo, "Open Images…");
    auto openFolderButton = Button(Icon::Folder, "Open Folder…").style(ButtonStyle::Prominent);
    auto emptyButtons = HStack({openImagesButton, openFolderButton}).spacing(10);
    auto emptyIcon = Canvas([](Painter& p, const Rect& r) { p.drawIcon(Icon::Photo, r, theme().colors.tertiaryLabel, 0.7f); });
    emptyIcon.frame(64, 64);
    auto empty = VStack({emptyIcon, emptyTitle, emptyText, emptyButtons}).spacing(10).align(Alignment::Center);

    auto page = std::make_shared<PhotosPage>();
    page->gridScroll = gridScroll.node();
    page->viewer = viewer;
    page->empty = empty.node();
    page->addChild(gridScroll.node());
    page->addChild(viewer);
    page->addChild(empty.node());

    auto sidebar = Sidebar(std::vector<SidebarItem>{});
    auto split = SplitView(sidebar, Element(page)).sidebarWidth(220);

    // Info popover.
    auto infoName = Label("").font(TextStyle::Headline).lineLimit(1).maxWidth(280);
    auto infoSize = Label("").secondary();
    auto infoFile = Label("").secondary();
    auto infoPath = Label("").font(TextStyle::Subheadline).tertiary().maxWidth(280);
    auto info = Popover(VStack({infoName, infoSize, infoFile, infoPath}).spacing(4));

    // Behaviour -------------------------------------------------------------------------------------
    auto visiblePhotos = [&] {
        std::vector<PhotoPtr> out;
        std::string q = lower(query);
        for (const auto& ph : library) {
            if (section == "favorites" && !ph->favorite) continue;
            if (section.rfind("folder:", 0) == 0 && ph->folder != section.substr(7)) continue;
            if (!q.empty() && lower(ph->name).find(q) == std::string::npos) continue;
            out.push_back(ph);
        }
        return out;
    };

    auto sectionTitle = [&]() -> std::string {
        if (section == "favorites") return "Favorites";
        if (section.rfind("folder:", 0) == 0) return fs::path(section.substr(7)).filename().string();
        return "Library";
    };

    auto rebuildSidebar = [&] {
        size_t favorites = 0;
        for (const auto& p : library) favorites += p->favorite ? 1 : 0;
        std::vector<SidebarItem> items{
            SidebarItem::header("Photos"),
            SidebarItem("all", Icon::Photo, "Library").withBadge(library.empty() ? "" : std::to_string(library.size())),
            SidebarItem("favorites", Icon::Heart, "Favorites").withBadge(favorites ? std::to_string(favorites) : ""),
        };
        if (!folders.empty()) items.push_back(SidebarItem::header("Folders"));
        for (const auto& f : folders) {
            size_t n = std::count_if(library.begin(), library.end(), [&](const PhotoPtr& p) { return p->folder == f; });
            items.push_back(SidebarItem("folder:" + f, Icon::Folder, fs::path(f).filename().string())
                                .withBadge(std::to_string(n)));
        }
        sidebar.setItems(std::move(items));
        sidebar.selection(section);
    };

    // Toolbars swap between library and viewer modes.
    Toolbar libraryToolbar;
    Toolbar viewerToolbar;
    auto favoriteButton = Button(Icon::Heart).style(ButtonStyle::Borderless).tooltip("Favorite (F)");
    auto infoButton = Button(Icon::Info).style(ButtonStyle::Borderless).tooltip("Info");
    auto sizeSlider = Slider(170, 80, 320);

    std::function<void()> refresh;
    std::function<void(int)> openViewer;
    std::function<void()> closeViewer;

    auto updateViewerChrome = [&] {
        auto ph = viewer->current();
        if (!ph) return;
        window.setTitle(ph->name);
        favoriteButton.icon(ph->favorite ? Icon::HeartFill : Icon::Heart);
        if (ph->favorite) favoriteButton.foreground(theme().colors.pink);
        else favoriteButton->setForeground(std::nullopt);
    };

    auto loadFull = [&](int index) {
        const auto& list = grid->photos();
        if (index < 0 || index >= static_cast<int>(list.size())) return;
        PhotoPtr ph = list[static_cast<size_t>(index)];
        if (ph->full || ph->fullRequested || ph->failed) return;
        ph->fullRequested = true;
        loader.full(ph, [&, ph] {
            ph->fullRequested = false;
            viewer->photoUpdated();
        });
    };

    auto trimFullImages = [&](int keepCenter) {
        // Only the current photo and its neighbours keep their full-resolution textures.
        const auto& list = grid->photos();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (std::abs(i - keepCenter) > 1) list[static_cast<size_t>(i)]->full.reset();
        }
    };

    openViewer = [&](int index) {
        const auto& list = grid->photos();
        if (index < 0 || index >= static_cast<int>(list.size())) return;
        grid->select(index);
        viewer->show(list, index);
        viewer->setHidden(false);
        gridScroll.setHidden(true);
        window.setToolbar(viewerToolbar);
        updateViewerChrome();
        loadFull(index);
        loadFull(index + 1);
        loadFull(index - 1);
        trimFullImages(index);
        if (auto* h = viewer->host()) h->setFocus(viewer.get(), false);
    };

    closeViewer = [&] {
        info.dismiss();
        viewer->setHidden(true);
        gridScroll.setHidden(false);
        trimFullImages(-10);
        window.setToolbar(libraryToolbar);
        window.setTitle(sectionTitle());
        if (auto* h = grid->host()) h->setFocus(grid.get(), false);
        grid->select(grid->selected());
    };

    auto saveLibrary = [&] {
        if (!prefs.getBool("rememberLibrary", true)) {
            prefs.remove("library");
            return;
        }
        std::vector<std::string> paths;
        for (const auto& p : library) paths.push_back(p->path);
        prefs.set("library", joinLines(paths));
    };

    auto toggleFavorite = [&](const PhotoPtr& ph) {
        ph->favorite = !ph->favorite;
        if (ph->favorite) favoritePaths.insert(ph->path);
        else favoritePaths.erase(ph->path);
        prefs.set("favorites", joinLines({favoritePaths.begin(), favoritePaths.end()}));
        rebuildSidebar();
        if (!viewer->isVisible() && section == "favorites") refresh();
        grid->invalidate();
        updateViewerChrome();
    };

    refresh = [&] {
        auto photos = visiblePhotos();
        grid->setPhotos(photos);
        bool none = photos.empty();
        empty.setHidden(!none);
        gridScroll.setHidden(none || viewer->isVisible());
        if (library.empty()) {
            emptyTitle.setText("No Photos");
            emptyText.setText("Drop images or folders here, or import them.");
            emptyButtons.setHidden(false);
        } else {
            emptyTitle.setText(!query.empty() ? "No Results" : section == "favorites" ? "No Favorites" : "No Photos");
            emptyText.setText(!query.empty() ? "No photo names contain “" + query + "”."
                                             : "Mark photos with the heart button or press F.");
            emptyButtons.setHidden(true);
        }
        if (!viewer->isVisible()) window.setTitle(sectionTitle());
    };

    auto addPaths = [&](const std::vector<std::string>& paths) {
        std::set<std::string> known;
        for (const auto& p : library) known.insert(p->path);
        std::vector<PhotoPtr> added;
        auto addFile = [&](const fs::path& file) {
            std::error_code ec;
            fs::path abs = fs::weakly_canonical(file, ec);
            if (ec || !isImageFile(abs) || !known.insert(abs.string()).second) return;
            auto ph = std::make_shared<Photo>();
            ph->path = abs.string();
            ph->name = abs.filename().string();
            ph->folder = abs.parent_path().string();
            ph->bytes = fs::file_size(abs, ec);
            added.push_back(ph);
            if (std::find(folders.begin(), folders.end(), ph->folder) == folders.end()) folders.push_back(ph->folder);
        };
        for (const auto& p : paths) {
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                std::vector<fs::path> files;
                for (const auto& entry : fs::directory_iterator(p, ec)) {
                    if (entry.is_regular_file(ec)) files.push_back(entry.path());
                }
                std::sort(files.begin(), files.end());
                for (const auto& f : files) addFile(f);
            } else {
                addFile(p);
            }
        }
        if (added.empty()) return;
        for (const auto& ph : added) {
            ph->favorite = favoritePaths.count(ph->path) > 0;
            library.push_back(ph);
            loader.thumbnail(ph, [&] { grid->invalidate(); });
        }
        std::sort(folders.begin(), folders.end());
        saveLibrary();
        rebuildSidebar();
        refresh();
    };

    const std::vector<FileFilter> imageFilters{{"Images", "jpg;jpeg;png;gif;bmp;tga"}, {"All files", "*"}};
    auto openImages = [&] { app.openFiles(imageFilters, true, addPaths, &window); };
    auto openFolder = [&] { app.openFolder(addPaths, &window); };
    openImagesButton.onClick(openImages);
    openFolderButton.onClick(openFolder);

    std::function<void()> openSettings;
    libraryToolbar = Toolbar({
        Menu("Import", {MenuItem("Open Images…", openImages).withIcon(Icon::Photo).withShortcut("Ctrl+O"),
                        MenuItem("Open Folder…", openFolder).withIcon(Icon::Folder).withShortcut("Ctrl+Shift+O")})
            .icon(Icon::Plus),
        Spacer(),
        HStack({Canvas([](Painter& p, const Rect& r) { p.drawIcon(Icon::Grid, r, theme().colors.secondaryLabel); }).frame(12, 12),
                sizeSlider.width(110),
                Canvas([](Painter& p, const Rect& r) { p.drawIcon(Icon::Square, r, theme().colors.secondaryLabel); }).frame(16, 16)})
            .spacing(8)
            .tooltip("Thumbnail size (or pinch)"),
        SearchField("Search").width(180).onChange([&](const std::string& q) {
            query = q;
            refresh();
        }),
        Button(Icon::Gear).style(ButtonStyle::Borderless).tooltip("Settings (Ctrl+,)").onClick([&] { openSettings(); }),
    });
    viewerToolbar = Toolbar({
        ToolbarGroup({Button(Icon::ChevronLeft).style(ButtonStyle::Borderless).tooltip("Back to Library (Esc)").onClick([&] {
            closeViewer();
        })}),
        Spacer(),
        ToolbarGroup({favoriteButton, infoButton}),
    });

    std::function<void()> syncSettings;  // keeps an open Settings window in step
    sizeSlider.onChange([&](float v) {
        grid->setTileSize(v);
        prefs.set("tileSize", grid->tileSize());
        if (syncSettings) syncSettings();
    });
    grid->onTileSizeChanged = [&](float v) {
        sizeSlider.setValue(v);
        prefs.set("tileSize", v);
        if (syncSettings) syncSettings();
    };
    grid->onOpen = [&](int i) { openViewer(i); };
    grid->onToggleFavorite = [&](int i) { toggleFavorite(grid->photos()[static_cast<size_t>(i)]); };
    grid->onRemove = [&](int i) {
        PhotoPtr ph = grid->photos()[static_cast<size_t>(i)];
        std::erase(library, ph);  // removes from the library only, never from disk
        std::erase_if(folders, [&](const std::string& f) {
            return std::none_of(library.begin(), library.end(), [&](const PhotoPtr& p) { return p->folder == f; });
        });
        if (section.rfind("folder:", 0) == 0 && std::find(folders.begin(), folders.end(), section.substr(7)) == folders.end())
            section = "all";
        saveLibrary();
        rebuildSidebar();
        refresh();
        grid->select(i);
    };

    // Settings window (Ctrl+,) ------------------------------------------------------------------------
    Window* settingsWindow = nullptr;
    openSettings = [&] {
        if (settingsWindow) {
            settingsWindow->raise();
            return;
        }
        auto& w = app.createWindow({.title = "Photos Settings", .width = 540, .height = 520, .minWidth = 460, .minHeight = 340});
        auto remember = Toggle("", prefs.getBool("rememberLibrary", true)).onChange([&](bool on) {
            prefs.set("rememberLibrary", on);
            saveLibrary();
        });
        auto tiles = Slider(grid->tileSize(), 80, 320).width(180).onChange([&](float v) { grid->setTileSize(v); grid->onTileSizeChanged(grid->tileSize()); });
        auto count = Label("").secondary();
        auto clear = Button("Clear Library").style(ButtonStyle::Destructive);
        auto updateCount = [&, count, clear]() mutable {
            count.setText(library.size() == 1 ? "1 photo" : std::to_string(library.size()) + " photos");
            clear.setDisabled(library.empty());
        };
        clear.onClick([&, updateCount]() mutable {
            // Only forgets the photos; nothing is deleted from disk.
            if (viewer->isVisible()) closeViewer();
            library.clear();
            folders.clear();
            section = "all";
            saveLibrary();
            rebuildSidebar();
            refresh();
            updateCount();
        });
        updateCount();
        auto form = Form({
            AppearanceSettings(app),
            FormSection("Library",
                        {FormRow("Reopen library at launch", remember, "Imported photos come back next time"),
                         FormRow("Thumbnail size", tiles), FormRow("Library", HStack({count, clear}).spacing(12).align(Alignment::Center))},
                        "Clearing the library never deletes files. Favorites are remembered either way."),
        });
        w.setContent(ScrollView(VStack({form}).padding(Insets(12, 24, 24, 24))));
        w.onKey([&w](const KeyEvent& e) {
            if (e.key == Key::Escape || (e.primary() && e.key == Key::W)) {
                w.close();
                return true;
            }
            return false;
        });
        syncSettings = [&, tiles, updateCount]() mutable {
            tiles.setValue(grid->tileSize());
            updateCount();
        };
        w.onClosed([&] {
            settingsWindow = nullptr;
            syncSettings = nullptr;
        });
        settingsWindow = &w;
    };
    viewer->onNavigate = [&](int i) { openViewer(i); };
    viewer->onClose = [&] { closeViewer(); };
    viewer->onToggleFavorite = [&] {
        if (auto ph = viewer->current()) toggleFavorite(ph);
    };
    favoriteButton.onClick([&] {
        if (auto ph = viewer->current()) toggleFavorite(ph);
    });
    infoButton.onClick([&] {
        auto ph = viewer->current();
        if (!ph) return;
        infoName.setText(ph->name);
        infoSize.setText(std::to_string(ph->width) + " × " + std::to_string(ph->height) + " pixels");
        std::string ext = lower(fs::path(ph->path).extension().string());
        infoFile.setText((ext.size() > 1 ? lower(ext.substr(1)) : "image") + " · " + formatBytes(ph->bytes));
        infoPath.setText(ph->folder);
        info.show(infoButton);
    });
    sidebar.onSelect([&](const std::string& id) {
        section = id;
        if (viewer->isVisible()) closeViewer();
        refresh();
        grid->select(0);
        gridScroll.scrollTo({0, 0}, false);
    });

    window.onFilesDropped(addPaths);
    window.onKey([&](const KeyEvent& e) {
        if (e.primary() && e.key == Key::O) {
            e.shift() ? openFolder() : openImages();
            return true;
        }
        if (e.primary() && e.key == Key::Comma) {
            openSettings();
            return true;
        }
        return false;
    });

    window.setToolbar(libraryToolbar);
    window.setContent(split);
    rebuildSidebar();
    refresh();

    {
        const float tile = std::clamp(prefs.getFloat("tileSize", 170), 80.0f, 320.0f);
        grid->setTileSize(tile);
        sizeSlider.setValue(tile);
    }
    if (prefs.getBool("rememberLibrary", true)) {
        std::vector<std::string> saved;
        std::error_code ec;
        for (auto& p : splitLines(prefs.getString("library"))) {
            if (fs::exists(p, ec)) saved.push_back(std::move(p));  // moved or deleted photos drop out
        }
        if (!saved.empty()) addPaths(saved);
    }
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty()) {
        addPaths(args);
        // An image passed on the command line ("Open With Photos") opens in the viewer; folders only import.
        std::string first;
        for (const auto& a : args) {
            std::error_code ec;
            if (fs::is_directory(a, ec)) continue;
            const fs::path abs = fs::weakly_canonical(a, ec);
            if (!ec && isImageFile(abs)) {
                first = abs.string();
                break;
            }
        }
        if (!first.empty()) {
            section = "all";
            query.clear();
            rebuildSidebar();
            refresh();
            // After the first layout, so the viewer has a host to take keyboard focus from.
            app.post([&, first] {
                const auto& list = grid->photos();
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i]->path == first) return openViewer(static_cast<int>(i));
                }
            });
        }
    }

    // MAYO_DEMO=viewer[:index] | zoom[:index] | open | favorite | settings, for screenshots and smoke tests.
    if (const char* env = std::getenv("MAYO_DEMO")) {
        std::string demo = env;
        int index = demo.find(':') != std::string::npos ? std::atoi(demo.c_str() + demo.find(':') + 1) : 0;
        demo = demo.substr(0, demo.find(':'));
        if (demo == "viewer" || demo == "zoom") app.after(0.6, [&, index] { openViewer(index); });
        if (demo == "zoom") app.after(1.2, [&] { viewer->onKeyDown(KeyEvent{Key::Num1}); });
        if (demo == "open") app.after(0.3, openImages);
        if (demo == "settings") app.after(0.3, [&] { openSettings(); });
        if (demo == "favorite") app.after(0.6, [&] {
            for (int i : {1, 4, 6}) {
                if (i < static_cast<int>(grid->photos().size())) toggleFavorite(grid->photos()[static_cast<size_t>(i)]);
            }
            grid->select(4);
        });
    }
    return app.run();
}
