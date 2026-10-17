#include "mayonnaise/popup.hpp"

#include <algorithm>

#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "mayonnaise/widgets/popover.hpp"

namespace mayo {

PopupView::PopupView(Material material) : m_material(material) {}

void PopupView::showFrom(Widget& anchor, PopupEdge edge) {
    WidgetHost* host = anchor.host();
    if (!host) return;
    if (m_shown) dismiss();
    m_anchor = m_anchorOverride.has_value() ? *m_anchorOverride : anchor.frame();
    m_edge = edge;
    m_shown = true;
    m_shownHost = host;
    m_appear.set(0.0f);
    m_appear.animateTo(1.0f, host->time(), Transition{0.16f, Curve::EaseOut});
    host->showOverlay(shared_from_this());
}

void PopupView::dismiss() {
    if (!m_shown) return;
    m_shown = false;
    if (m_shownHost) m_shownHost->dismissOverlay(this);
    m_shownHost = nullptr;
    if (m_onDismiss) m_onDismiss();
}

void PopupView::popupDismissed() {
    m_shown = false;
    m_shownHost = nullptr;
    if (m_onDismiss) m_onDismiss();
}

float PopupView::cornerRadiusValue() const {
    const auto& m = theme().metrics;
    return m_material == Material::Menu ? m.menuRadius : m.popoverRadius;
}

Rect PopupView::placePopup(Size window, Size measured) {
    const float margin = 8;
    float w = std::max(measured.width, m_minWidth);
    float h = std::min(measured.height, window.height - margin * 2);
    float x = m_centered ? m_anchor.center().x - w * 0.5f : m_anchor.x;
    float below = m_anchor.bottom() + m_gap;
    float above = m_anchor.y - m_gap - h;
    float y = m_edge == PopupEdge::Below ? below : above;
    if (m_edge == PopupEdge::Below && y + h > window.height - margin && above >= margin) y = above;
    if (m_edge == PopupEdge::Above && y < margin && below + h <= window.height - margin) y = below;
    x = std::clamp(x, margin, std::max(margin, window.width - w - margin));
    y = std::clamp(y, margin, std::max(margin, window.height - h - margin));
    return {x, y, w, h};
}

void PopupView::draw(Painter& p) {
    const Theme& t = theme();
    const float a = std::clamp(sample(m_appear), 0.0f, 1.0f);
    // The panel grows out of its anchor while it fades in, instead of appearing in place.
    if (m_appear.isAnimating(now())) requestAnimationFrame();
    const float fromAnchor = m_edge == PopupEdge::Below ? -6.0f : 6.0f;
    const Rect r = frame().translated({0, (1.0f - a) * fromAnchor});
    const float radius = cornerRadiusValue();
    p.pushOpacity(a);
    // Two shadows: a wide soft one for depth and a tight one to define the edge.
    p.drawShadow(r, radius, Color::black(t.isDark() ? 0.45f : 0.18f), 30, {0, 10});
    p.drawShadow(r, radius, Color::black(t.isDark() ? 0.35f : 0.10f), 3, {0, 1});
    p.drawMaterial(r, radius, m_material);
    p.pushClip(r, radius);
    drawPanel(p);
    p.popClip();
    p.popOpacity();
}

PopoverView::PopoverView(std::shared_ptr<Widget> content) : PopupView(Material::Popover) {
    setPadding(Insets(14));
    setCentered(true);
    if (content) addChild(std::move(content));
}

}  // namespace mayo
