#include "mayonnaise/widgets/scroll_view.hpp"

#include <algorithm>
#include <cmath>

#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"

namespace mayo {

namespace {
constexpr double kBarLinger = 0.9;  // scrollbars stay visible after activity
constexpr double kBarFade = 0.25;

float component(Vec2 v, int i) { return i == 0 ? v.x : v.y; }
}  // namespace

ScrollViewView::ScrollViewView(std::shared_ptr<Widget> content) { setContent(std::move(content)); }

void ScrollViewView::setContent(std::shared_ptr<Widget> content) {
    if (m_content) removeChild(m_content.get());
    m_content = std::move(content);
    if (m_content) addChild(m_content);
}

void ScrollViewView::setAxes(ScrollAxes axes) {
    m_axes = axes;
    m_physics.setAxes(scrolls(Axis::Horizontal), scrolls(Axis::Vertical));
    invalidateLayout();
}

void ScrollViewView::setContentInsetTop(float inset) {
    if (inset == m_insetTop) return;
    m_insetTop = inset;
    invalidateLayout();
}

bool ScrollViewView::scrolls(Axis axis) const {
    if (m_axes == ScrollAxes::Both) return true;
    return axis == Axis::Vertical ? m_axes == ScrollAxes::Vertical : m_axes == ScrollAxes::Horizontal;
}

bool ScrollViewView::hasScrollingAncestor(Axis axis) const {
    for (const Widget* w = parent(); w; w = w->parent()) {
        if (const auto* sv = dynamic_cast<const ScrollViewView*>(w); sv && sv->scrolls(axis)) return true;
    }
    return false;
}

Vec2 ScrollViewView::maxOffset() const {
    Rect v = contentRect();
    return {scrolls(Axis::Horizontal) ? std::max(0.0f, m_contentSize.width - v.width) : 0.0f,
            scrolls(Axis::Vertical) ? std::max(0.0f, m_contentSize.height + m_insetTop - v.height) : 0.0f};
}

void ScrollViewView::updateLimits() {
    Rect v = contentRect();
    m_physics.setAxes(scrolls(Axis::Horizontal), scrolls(Axis::Vertical));
    m_physics.setBouncesWhenFitting(false, scrolls(Axis::Vertical) && !hasScrollingAncestor(Axis::Vertical));
    m_physics.setLimits(maxOffset(), {v.width, v.height - m_insetTop});
}

Size ScrollViewView::measureContent(Size available) {
    if (!m_content) return {};
    Size proposal{scrolls(Axis::Horizontal) ? kInfinity : available.width,
                  scrolls(Axis::Vertical) ? kInfinity : available.height};
    Size s = m_content->measure(proposal);
    return {std::isfinite(available.width) ? available.width : s.width,
            std::isfinite(available.height) ? available.height : s.height};
}

void ScrollViewView::arrangeContent(const Rect& viewport) {
    if (!m_content) return;
    Size proposal{scrolls(Axis::Horizontal) ? kInfinity : viewport.width,
                  scrolls(Axis::Vertical) ? kInfinity : viewport.height};
    Size s = m_content->measure(proposal);
    // Content is at least as large as the viewport so expanding content (spacers) still fills it.
    m_contentSize = {std::max(s.width, viewport.width), std::max(s.height, viewport.height - m_insetTop)};
    updateLimits();
    place();
}

void ScrollViewView::place() {
    if (!m_content) return;
    Rect v = contentRect();
    Vec2 o = m_physics.offset();
    m_content->arrange({v.x - o.x, v.y + m_insetTop - o.y, m_contentSize.width, m_contentSize.height});
}

void ScrollViewView::touch() {
    m_activity = now();
    if (auto* h = host()) h->scheduleFrame(m_activity + kBarLinger);
    invalidate();
}

void ScrollViewView::scrollTo(Vec2 offset, bool animated) {
    updateLimits();
    m_physics.scrollTo(offset, animated, now());
    if (animated) requestAnimationFrame();
    place();
    touch();
}

bool ScrollViewView::onScroll(const ScrollEvent& e) {
    updateLimits();
    const double t = now();
    if (!m_physics.handle(e, t)) return false;
    if (auto* h = host(); h && m_physics.deadline() >= 0) h->scheduleFrame(m_physics.deadline());
    if (m_physics.isAnimating()) requestAnimationFrame();
    place();
    touch();
    return true;
}

void ScrollViewView::step(double t) {
    if (!m_physics.isAnimating()) return;
    const bool moving = m_physics.step(t);
    place();
    if (moving) {
        touch();
        requestAnimationFrame();
    } else if (auto* h = host(); h && m_physics.deadline() >= 0) {
        h->scheduleFrame(m_physics.deadline());
    }
}

Rect ScrollViewView::trackRect(Axis axis) const {
    Rect v = contentRect();
    const float w = m_hoveringBar || m_dragAxis >= 0 ? 11.0f : theme().metrics.scrollbarWidth + 2;
    if (axis == Axis::Vertical) return {v.right() - w - 1, v.y + m_insetTop + 2, w, v.height - m_insetTop - 4};
    return {v.x + 2, v.bottom() - w - 1, v.width - 4, w};
}

Rect ScrollViewView::thumbRect(Axis axis) const {
    Rect track = trackRect(axis);
    Rect v = contentRect();
    const int i = axis == Axis::Horizontal ? 0 : 1;
    float viewLen = i == 0 ? v.width : v.height - m_insetTop;
    float contentLen = i == 0 ? m_contentSize.width : m_contentSize.height;
    float trackLen = i == 0 ? track.width : track.height;
    float m = component(maxOffset(), i);
    float o = component(m_physics.offset(), i);
    float over = std::abs(component(m_physics.overscroll(), i));
    float len = std::max(20.0f, trackLen * viewLen / std::max(contentLen, 1.0f) - over * 0.5f);
    len = std::min(len, trackLen);
    float frac = m > 0 ? std::clamp(o / m, 0.0f, 1.0f) : 0.0f;
    float pos = (trackLen - len) * frac;
    Rect thumb = i == 0 ? Rect{track.x + pos, track.y, len, track.height} : Rect{track.x, track.y + pos, track.width, len};
    return thumb.inset(2.0f);
}

void ScrollViewView::setScrollEdge(ScrollEdgeStyle style, float height) {
    m_edgeStyle = style;
    m_edgeHeight = std::isfinite(height) ? std::max(0.0f, height) : 32.0f;
    invalidate();
}

void ScrollViewView::draw(Painter& p) {
    const double t = now();
    step(t);
    Rect v = contentRect();
    p.pushClip(v, cornerRadius().max());
    paintChildren(p);
    if (scrolls(Axis::Vertical) && maxOffset().y > 0 && m_edgeStyle != ScrollEdgeStyle::None) {
        const float height = std::min(m_edgeHeight, v.height * 0.5f);
        if (height > 0) {
            const float top = std::clamp(offset().y / height, 0.0f, 1.0f);
            const float bottom = std::clamp((maxOffset().y - offset().y) / height, 0.0f, 1.0f);
            p.drawScrollEdge({v.x, v.y, v.width, height}, m_edgeStyle, false, top);
            p.drawScrollEdge({v.x, v.bottom() - height, v.width, height}, m_edgeStyle, true, bottom);
        }
    }
    p.popClip();

    // Overlay scrollbars.
    double idle = t - m_activity;
    float alpha = m_hoveringBar || m_dragAxis >= 0
                      ? 1.0f
                      : static_cast<float>(std::clamp(1.0 - (idle - kBarLinger) / kBarFade, 0.0, 1.0));
    if (idle > kBarLinger && alpha > 0) requestAnimationFrame();
    if (alpha <= 0) return;
    const Theme& th = theme();
    Color thumbColor = th.isDark() ? Color::white(0.45f) : Color::black(0.38f);
    Vec2 mx = maxOffset();
    for (int i = 0; i < 2; ++i) {
        Axis axis = i == 0 ? Axis::Horizontal : Axis::Vertical;
        if (!scrolls(axis) || component(mx, i) <= 0) continue;
        if (m_hoveringBar || m_dragAxis == i) {
            Rect track = trackRect(axis);
            p.fillRoundedRect(track, track.width * 0.5f, th.colors.tertiaryFill.multipliedAlpha(alpha));
        }
        Rect thumb = thumbRect(axis);
        float r = std::min(thumb.width, thumb.height) * 0.5f;
        p.fillRoundedRect(thumb, r, thumbColor.multipliedAlpha(alpha));
    }
}

Widget* ScrollViewView::hitTest(Point pt) {
    if (!isVisible() || !frame().contains(pt)) return nullptr;
    Vec2 mx = maxOffset();
    if (mx.y > 0 && scrolls(Axis::Vertical) && trackRect(Axis::Vertical).outset(2).contains(pt)) return this;
    if (mx.x > 0 && scrolls(Axis::Horizontal) && trackRect(Axis::Horizontal).outset(2).contains(pt)) return this;
    return Widget::hitTest(pt);
}

bool ScrollViewView::onPointerDown(const PointerEvent& e) {
    for (int i = 0; i < 2; ++i) {
        Axis axis = i == 0 ? Axis::Horizontal : Axis::Vertical;
        if (!scrolls(axis) || component(maxOffset(), i) <= 0) continue;
        Rect track = trackRect(axis);
        if (!track.outset(2).contains(e.position)) continue;
        Rect thumb = thumbRect(axis);
        float pos = component(e.position, i);
        float start = i == 0 ? thumb.x : thumb.y;
        float len = i == 0 ? thumb.width : thumb.height;
        m_dragAxis = i;
        m_dragGrab = (pos >= start && pos <= start + len) ? pos - start : len * 0.5f;
        m_physics.setOffset(m_physics.offset());  // grabbing the bar stops momentum
        onPointerMove(e);
        return true;
    }
    return false;
}

void ScrollViewView::onPointerMove(const PointerEvent& e) {
    if (m_dragAxis >= 0) {
        const int i = m_dragAxis;
        Axis axis = i == 0 ? Axis::Horizontal : Axis::Vertical;
        Rect track = trackRect(axis);
        Rect thumb = thumbRect(axis);
        float trackStart = i == 0 ? track.x : track.y;
        float trackLen = i == 0 ? track.width : track.height;
        float len = (i == 0 ? thumb.width : thumb.height) + 4;
        float frac = (component(e.position, i) - m_dragGrab - trackStart) / std::max(1.0f, trackLen - len);
        Vec2 o = m_physics.offset();
        (i == 0 ? o.x : o.y) = std::clamp(frac, 0.0f, 1.0f) * component(maxOffset(), i);
        m_physics.setOffset(o);
        place();
        touch();
        return;
    }
    bool over = false;
    Vec2 mx = maxOffset();
    if (mx.y > 0 && scrolls(Axis::Vertical)) over |= trackRect(Axis::Vertical).outset(2).contains(e.position);
    if (mx.x > 0 && scrolls(Axis::Horizontal)) over |= trackRect(Axis::Horizontal).outset(2).contains(e.position);
    if (over != m_hoveringBar) {
        m_hoveringBar = over;
        touch();
    }
}

void ScrollViewView::onPointerUp(const PointerEvent& /*e*/) {
    m_dragAxis = -1;
    touch();
}

void ScrollViewView::onHoverChanged(bool hovered) {
    if (!hovered && m_hoveringBar) {
        m_hoveringBar = false;
        invalidate();
    }
}

}  // namespace mayo
