#include "mayonnaise/ui_translation.hpp"
#include "window_root.hpp"

#include <algorithm>
#include <cmath>

#include "../render/font.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {

/// Close / minimize / zoom. One widget so hovering anywhere over the group reveals all glyphs.
class TrafficLightsView final : public Widget {
public:
    std::function<void()> onClose, onMinimize, onZoom;
    bool key = true;

    Size measureContent(Size /*available*/) override {
        const auto& m = theme().metrics;
        return {m.trafficLightSize * 3 + m.trafficLightSpacing * 2, m.trafficLightSize};
    }
    [[nodiscard]] bool isInteractive() const override { return true; }

    [[nodiscard]] Rect light(int i) const {
        const auto& m = theme().metrics;
        Rect f = frame();
        return {f.x + static_cast<float>(i) * (m.trafficLightSize + m.trafficLightSpacing),
                f.center().y - m.trafficLightSize * 0.5f, m.trafficLightSize, m.trafficLightSize};
    }
    [[nodiscard]] int lightAt(Point p) const {
        for (int i = 0; i < 3; ++i) {
            if (light(i).outset(2).contains(p)) return i;
        }
        return -1;
    }

    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        m_pressedLight = lightAt(e.position);
        return m_pressedLight >= 0;
    }
    void onPointerMove(const PointerEvent& e) override {
        int over = lightAt(e.position);
        if (over != m_overLight) {
            m_overLight = over;
            invalidate();
        }
    }
    void onPointerUp(const PointerEvent& e) override {
        int which = m_pressedLight;
        m_pressedLight = -1;
        invalidate();
        if (which < 0 || lightAt(e.position) != which) return;
        if (which == 0 && onClose) onClose();
        if (which == 1 && onMinimize) onMinimize();
        if (which == 2 && onZoom) onZoom();
    }
    void onHoverChanged(bool /*hovered*/) override { m_overLight = -1; }

    void draw(Painter& p) override {
        const Theme& t = theme();
        const bool dark = t.isDark();
        static constexpr Color kFill[3] = {Color::hex(0xFF5F57), Color::hex(0xFEBC2E), Color::hex(0x28C840)};
        static constexpr Color kGlyph[3] = {Color::hex(0x4D0000, 0.75f), Color::hex(0x995700, 0.8f),
                                            Color::hex(0x006500, 0.8f)};
        const bool showGlyphs = isHovered();
        for (int i = 0; i < 3; ++i) {
            Rect r = light(i);
            Color fill = key || showGlyphs ? kFill[i] : (dark ? Color::white(0.18f) : Color::black(0.12f));
            if (isPressed() && m_pressedLight == i && m_overLight == i) fill = fill.lighter(-0.2f);
            p.fillRoundedRect(r, r.width * 0.5f, fill, Color::black(dark ? 0.2f : 0.1f), 0.5f);
            if (!showGlyphs) continue;
            const Point c = r.center();
            const float k = r.width * 0.22f;
            const float w = 1.1f;
            if (i == 0) {
                p.drawLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, kGlyph[i], w);
                p.drawLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, kGlyph[i], w);
            } else if (i == 1) {
                p.drawLine({c.x - k * 1.25f, c.y}, {c.x + k * 1.25f, c.y}, kGlyph[i], w * 1.1f);
            } else {
                p.drawLine({c.x - k * 1.25f, c.y}, {c.x + k * 1.25f, c.y}, kGlyph[i], w * 1.1f);
                p.drawLine({c.x, c.y - k * 1.25f}, {c.x, c.y + k * 1.25f}, kGlyph[i], w * 1.1f);
            }
        }
    }

private:
    int m_pressedLight = -1;
    int m_overLight = -1;
};

}  // namespace

/// Title bar strip: draggable, traffic lights on the left, centred title or a leading title
/// followed by toolbar items.
class TitleBarView final : public Widget {
public:
    TitleBarView() : m_lights(std::make_shared<TrafficLightsView>()) {
        setDragRegion(true);
        addChild(m_lights);
    }
    std::string title;
    bool titleVisible = true;
    bool key = true;
    bool fullSize = false;
    float leadingEdge = 0;
    std::shared_ptr<TrafficLightsView> m_lights;
    std::shared_ptr<Widget> toolbar;

    void setToolbar(std::shared_ptr<Widget> t) {
        if (toolbar) removeChild(toolbar.get());
        toolbar = std::move(t);
        if (toolbar) addChild(toolbar);
    }
    [[nodiscard]] float height() const { return toolbar ? theme().metrics.titlebarHeight : 32.0f; }
    [[nodiscard]] Font titleFont() const {
        return toolbar ? theme().font(TextStyle::Title3) : theme().font(TextStyle::Headline);
    }
    [[nodiscard]] float titleStart() const {
        float start = m_lights->frame().right() + 16;
        if (fullSize && leadingEdge > 0) start = std::max(start, leadingEdge + 20);
        return start;
    }

    Size measureContent(Size available) override {
        m_lights->measure(available);
        if (toolbar) toolbar->measure({available.width, height()});
        return {available.width, height()};
    }
    void arrangeContent(const Rect& content) override {
        const auto& m = theme().metrics;
        Size s = m_lights->measure(content.size());
        // Traffic lights centre in the strip but never sink below a 32pt bar's centre line
        // when there's a tall toolbar (macOS keeps them aligned with toolbar items).
        m_lights->arrange({content.x + m.trafficLightInset - m.trafficLightSize * 0.5f + 2,
                           content.center().y - s.height * 0.5f, s.width, s.height});
        if (toolbar) {
            float x = titleStart();
            if (showsTitle()) x += std::ceil(measureTitle()) + 20;
            float right = content.right() - 12;
            toolbar->measure({std::max(0.0f, right - x), content.height});
            toolbar->arrange({x, content.y, std::max(0.0f, right - x), content.height});
        }
    }
    [[nodiscard]] float measureTitle() const;
    [[nodiscard]] bool showsTitle() const { return titleVisible && !title.empty(); }

    void draw(Painter& p) override {
        const Theme& t = theme();
        Rect r = frame();
        if (fullSize) {
            // Scroll-edge glass over content that slides under the toolbar; fades out at the bottom.
            float x = leadingEdge > 0 ? leadingEdge : r.x;
            MaterialStyle m = t.material(Material::Titlebar);
            m.fadeBottom = 0.45f;
            p.drawMaterial({x, r.y, r.right() - x, r.height + 10}, {}, m);
        }
        if (showsTitle()) {
            Font f = titleFont();
            Color c = key ? t.colors.label : t.colors.tertiaryLabel;
            float w = p.measureText(title, f);
            float left = titleStart();
            float x = toolbar ? left : std::max(left, r.center().x - w * 0.5f);
            float limit = toolbar ? toolbar->frame().x - 8 : r.right() - 12;
            p.pushClip({left, r.y, std::max(0.0f, limit - left), r.height});
            p.drawText(title, {x, r.y, w, r.height}, f, c);
            p.popClip();
        }
        paintChildren(p);
    }
};

float TitleBarView::measureTitle() const { return title.empty() ? 0.0f : render::measureText(title, titleFont()); }

WindowRootView::WindowRootView() : m_titleBar(std::make_shared<TitleBarView>()) {
    addChild(m_titleBar);
    m_titleBar->m_lights->onClose = [this] {
        if (onClose) onClose();
    };
    m_titleBar->m_lights->onMinimize = [this] {
        if (onMinimize) onMinimize();
    };
    m_titleBar->m_lights->onZoom = [this] {
        if (onZoom) onZoom();
    };
}

void WindowRootView::setContent(std::shared_ptr<Widget> content) {
    if (m_content) removeChild(m_content.get());
    m_content = std::move(content);
    // Content goes below the title bar in z-order so the traffic lights stay clickable.
    if (m_content) insertChild(0, m_content);
}

void WindowRootView::setToolbar(std::shared_ptr<Widget> toolbar) {
    m_titleBar->setToolbar(std::move(toolbar));
    invalidateLayout();
}

void WindowRootView::setTitle(const std::string& title) {
    m_titleBar->title = uiText(title);
    invalidateLayout();
}

void WindowRootView::setTitleVisible(bool visible) {
    m_titleBar->titleVisible = visible;
    invalidateLayout();
}

void WindowRootView::setChromeHidden(bool hidden) {
    m_chrome.animateTo(hidden ? 0.0f : 1.0f, now(), Transition{0.3f, Curve::EaseInOut});
    invalidate();
}

void WindowRootView::setWindowState(bool maximized, bool key, bool transparent, bool fullscreen) {
    m_fullscreen = fullscreen;
    m_maximized = maximized || fullscreen;
    m_key = key;
    m_transparent = transparent;
    m_titleBar->key = key;
    m_titleBar->m_lights->key = key;
}

void WindowRootView::configure(bool dialog, float cornerRadius, float backgroundOpacity, bool compositorFrame) {
    m_dialog = dialog;
    m_compositorFrame = compositorFrame;
    m_radius = cornerRadius;
    m_backgroundOpacity = std::clamp(backgroundOpacity, 0.0f, 1.0f);
}

float WindowRootView::windowRadius() const {
    if (m_compositorFrame) return 0.0f;
    return m_transparent && !m_maximized ? (m_radius >= 0 ? m_radius : theme().metrics.windowRadius) : 0.0f;
}

float WindowRootView::titleBarHeight() const { return m_dialog ? 0.0f : m_titleBar->height(); }

Size WindowRootView::measureContent(Size available) { return available; }

void WindowRootView::arrangeContent(const Rect& content) {
    const float barH = titleBarHeight();
    const bool fullSize = m_content && m_content->extendsIntoTitleBar();
    if (m_content) {
        Rect area = fullSize ? content : Rect{content.x, content.y + barH, content.width, content.height - barH};
        m_content->measure(area.size());
        m_content->arrange(area);
    }
    m_titleBar->fullSize = fullSize;
    m_titleBar->leadingEdge = fullSize ? m_content->titleBarLeadingEdge() : 0.0f;
    m_titleBar->measure({content.width, barH});
    m_titleBar->arrange({content.x, content.y, content.width, barH});
}
void WindowRootView::draw(Painter& p) {
    const Theme& t = theme();
    const float radius = windowRadius();
    Rect bounds = frame();
    p.fillRoundedRect(bounds, radius, t.colors.windowBackground.withAlpha(m_backgroundOpacity));
    p.pushClip(bounds, radius);
    // Chrome can be faded out (media playback); fully hidden in fullscreen, like macOS.
    const float chrome = (m_fullscreen || m_dialog) ? 0.0f : sample(m_chrome);
    m_titleBar->setOpacity(chrome);
    paintChildren(p);
    p.popClip();
}

void WindowRootView::paintWindowFrame(Painter& p) {
    const float radius = windowRadius();
    if (radius <= 0) return;
    const Theme& t = theme();
    const float px = 1.0f / p.scale();
    Rect bounds = frame();
    if (m_dialog) {
        p.strokeRoundedRect(bounds, radius, t.isDark() ? Color::white(0.16f) : Color::black(0.16f), px);
        return;
    }
    p.strokeRoundedRect(bounds, radius, t.colors.windowBorder, px);
    p.strokeRoundedRect(bounds.inset(px), radius - px, t.colors.windowHighlight, px);
}

}  // namespace mayo
