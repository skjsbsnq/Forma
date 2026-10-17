#include "mayonnaise/ui_translation.hpp"
#include "mayonnaise/widget.hpp"

#include <algorithm>

#include "mayonnaise/painter.hpp"

namespace mayo {

Widget::Widget() = default;
Widget::~Widget() {
    for (auto& c : m_children) c->m_parent = nullptr;
}

// --- tree ------------------------------------------------------------------------------------

void Widget::addChild(std::shared_ptr<Widget> child) { insertChild(m_children.size(), std::move(child)); }

void Widget::insertChild(size_t index, std::shared_ptr<Widget> child) {
    if (!child) return;
    if (child->m_parent) child->m_parent->removeChild(child.get());
    child->m_parent = this;
    index = std::min(index, m_children.size());
    Widget& ref = *child;
    m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
    onChildAdded(ref);
    invalidateLayout();
}

void Widget::removeChild(Widget* child) {
    auto it = std::find_if(m_children.begin(), m_children.end(), [&](const auto& c) { return c.get() == child; });
    if (it == m_children.end()) return;
    (*it)->m_parent = nullptr;
    m_children.erase(it);
    invalidateLayout();
}

void Widget::clearChildren() {
    for (auto& c : m_children) c->m_parent = nullptr;
    m_children.clear();
    invalidateLayout();
}

bool Widget::isAncestorOf(const Widget* other) const {
    for (const Widget* w = other; w; w = w->m_parent) {
        if (w == this) return true;
    }
    return false;
}

WidgetHost* Widget::host() const {
    const Widget* w = this;
    while (w->m_parent) w = w->m_parent;
    return w->m_host;
}

// --- layout ----------------------------------------------------------------------------------

Size Widget::measure(Size available) {
    if (m_hidden) return {0, 0};
    if (!m_layoutDirty && m_hasCache && m_cacheAvailable == available) return m_cacheResult;

    const float padH = m_padding.horizontal();
    const float padV = m_padding.vertical();
    Size inner = available;
    if (m_width) inner.width = *m_width;
    else if (m_maxW) inner.width = std::min(inner.width, *m_maxW);
    if (m_height) inner.height = *m_height;
    else if (m_maxH) inner.height = std::min(inner.height, *m_maxH);
    inner.width = std::max(0.0f, inner.width - padH);
    inner.height = std::max(0.0f, inner.height - padV);

    Size c = measureContent(inner);
    float w = m_width ? *m_width : c.width + padH;
    float h = m_height ? *m_height : c.height + padV;
    w = std::max(w, m_minW);
    h = std::max(h, m_minH);
    if (m_maxW) w = std::min(w, *m_maxW);
    if (m_maxH) h = std::min(h, *m_maxH);

    m_layoutDirty = false;
    m_hasCache = true;
    m_cacheAvailable = available;
    m_cacheResult = {w, h};
    return m_cacheResult;
}

void Widget::arrange(const Rect& frame) {
    m_frame = frame;
    if (m_hidden) return;
    arrangeContent(contentRect());
}

bool Widget::expands(Axis axis) const {
    if (m_hidden) return false;
    const auto& fixed = axis == Axis::Horizontal ? m_width : m_height;
    if (fixed) return false;
    const auto& max = axis == Axis::Horizontal ? m_maxW : m_maxH;
    if (max) return std::isinf(*max);
    return expandsContent(axis);
}

Size Widget::measureContent(Size available) {
    Size out;
    for (auto& c : m_children) {
        Size s = c->measure(available);
        out.width = std::max(out.width, s.width);
        out.height = std::max(out.height, s.height);
    }
    return out;
}

void Widget::arrangeContent(const Rect& content) {
    for (auto& c : m_children) c->arrange(content);
}

void Widget::invalidate() {
    if (auto* h = host()) h->requestRedraw();
}

void Widget::invalidateLayout() {
    for (Widget* w = this; w; w = w->m_parent) w->m_layoutDirty = true;
    if (auto* h = host()) h->requestLayout();
}

// --- painting --------------------------------------------------------------------------------

void Widget::paint(Painter& p) {
    if (m_hidden || m_opacity <= 0.0f) return;
    // Cull subtrees entirely outside the clip (generous margin for focus rings and shadows).
    if (!m_frame.outset(24).intersects(p.clipBounds())) return;
    float opacity = m_opacity * (m_disabled ? 0.45f : 1.0f);
    if (opacity < 1.0f) p.pushOpacity(opacity);
    if (m_material) p.drawMaterial(m_frame, m_cornerRadius, *m_material);
    if (m_background) p.fillRoundedRect(m_frame, m_cornerRadius, *m_background);
    draw(p);
    drawOverlay(p);
    if (opacity < 1.0f) p.popOpacity();
}

void Widget::paintChildren(Painter& p) {
    for (auto& c : m_children) c->paint(p);
}

void Widget::draw(Painter& p) { paintChildren(p); }
void Widget::drawOverlay(Painter& /*p*/) {}

Widget* Widget::hitTest(Point p) {
    if (m_hidden || !m_frame.contains(p)) return nullptr;
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        if (Widget* w = (*it)->hitTest(p)) return w;
    }
    return this;
}

// --- state -----------------------------------------------------------------------------------

bool Widget::isFocused() const {
    auto* h = host();
    return h && h->focusedWidget() == this;
}

bool Widget::isFocusVisible() const {
    auto* h = host();
    return h && h->focusedWidget() == this && h->focusVisible() && h->isKeyWindow();
}

bool Widget::isEnabled() const {
    for (const Widget* w = this; w; w = w->m_parent) {
        if (w->m_disabled) return false;
    }
    return true;
}

double Widget::now() const {
    auto* h = host();
    return h ? h->time() : 0.0;
}

void Widget::requestAnimationFrame() {
    if (auto* h = host()) h->requestRedraw();
}

void Widget::setHoveredState(bool hovered) {
    if (m_hovered == hovered) return;
    m_hovered = hovered;
    onHoverChanged(hovered);
    invalidate();
}

void Widget::setPressedState(bool pressed) {
    if (m_pressed == pressed) return;
    m_pressed = pressed;
    onPressedChanged(pressed);
    invalidate();
}

void Widget::notifyThemeChanged() {
    onThemeChanged();
    m_layoutDirty = true;
    for (auto& c : m_children) c->notifyThemeChanged();
}

// --- modifiers -------------------------------------------------------------------------------

void Widget::setPadding(const Insets& padding) {
    m_padding = padding;
    invalidateLayout();
}
void Widget::setFixedWidth(std::optional<float> w) {
    m_width = w;
    invalidateLayout();
}
void Widget::setFixedHeight(std::optional<float> h) {
    m_height = h;
    invalidateLayout();
}
void Widget::setMinSize(std::optional<float> minW, std::optional<float> minH) {
    if (minW) m_minW = *minW;
    if (minH) m_minH = *minH;
    invalidateLayout();
}
void Widget::setMaxSize(std::optional<float> maxW, std::optional<float> maxH) {
    if (maxW) m_maxW = *maxW;
    if (maxH) m_maxH = *maxH;
    invalidateLayout();
}
void Widget::setFlex(float flex) {
    m_flex = std::max(0.0f, flex);
    invalidateLayout();
}
void Widget::setBackground(std::optional<Color> color) {
    m_background = color;
    invalidate();
}
void Widget::setBackgroundMaterial(std::optional<Material> material) {
    m_material = material;
    invalidate();
}
void Widget::setCornerRadius(const CornerRadii& radii) {
    m_cornerRadius = radii;
    invalidate();
}
void Widget::setForeground(std::optional<Color> color) {
    m_foreground = color;
    invalidate();
}
std::optional<Color> Widget::foreground() const {
    for (const Widget* w = this; w; w = w->m_parent) {
        if (w->m_foreground) return w->m_foreground;
    }
    return std::nullopt;
}
void Widget::setDisabled(bool disabled) {
    if (m_disabled == disabled) return;
    m_disabled = disabled;
    onEnabledChanged();
    invalidate();
}
void Widget::setHidden(bool hidden) {
    if (m_hidden == hidden) return;
    m_hidden = hidden;
    invalidateLayout();
}
void Widget::setOpacity(float opacity) {
    opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity == m_opacity) return;  // called per frame by fades; don't schedule redundant frames
    m_opacity = opacity;
    invalidate();
}
void Widget::setTooltip(std::string text) { m_tooltip = uiText(text); }

CursorShape Widget::cursor() const {
    for (const Widget* w = this; w; w = w->m_parent) {
        if (w->m_cursor) return *w->m_cursor;
    }
    return CursorShape::Arrow;
}

}  // namespace mayo
