#include "mayonnaise/widgets/split_view.hpp"

#include <algorithm>

#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "mayonnaise/widgets/scroll_view.hpp"

namespace mayo {

namespace {

/// Invisible strip over the sidebar's trailing edge that resizes the split.
class SplitHandle final : public Widget {
public:
    explicit SplitHandle(SplitViewView& owner) : m_owner(owner) { setCursor(CursorShape::ResizeHorizontal); }
    [[nodiscard]] bool isInteractive() const override { return true; }
    bool onPointerDown(const PointerEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        m_startX = e.position.x;
        m_owner.beginResize();
        return true;
    }
    void onPointerMove(const PointerEvent& e) override {
        if (m_startX >= 0) m_owner.resizeBy(e.position.x - m_startX);
    }
    void onPointerUp(const PointerEvent& /*e*/) override { m_startX = -1; }

private:
    SplitViewView& m_owner;
    float m_startX = -1;
};

}  // namespace

SplitViewView::SplitViewView(std::shared_ptr<Widget> sidebar, std::shared_ptr<Widget> detail)
    : m_sidebar(std::move(sidebar)), m_detail(std::move(detail)) {
    if (m_detail) addChild(m_detail);
    if (m_sidebar) addChild(m_sidebar);
    m_handle = std::make_shared<SplitHandle>(*this);
    addChild(m_handle);
}

void SplitViewView::setDetail(std::shared_ptr<Widget> detail) {
    if (m_detail) removeChild(m_detail.get());
    m_detail = std::move(detail);
    if (m_detail) insertChild(0, m_detail);
}

void SplitViewView::setSidebarWidth(float w) {
    m_width = w;
    invalidateLayout();
}

void SplitViewView::setSidebarLimits(float minW, float maxW) {
    m_minWidth = minW;
    m_maxWidth = std::max(minW, maxW);
    invalidateLayout();
}

void SplitViewView::setSidebarVisible(bool visible, bool animated) {
    if (visible == m_visible) return;
    m_visible = visible;
    if (animated)
        m_reveal.animateTo(visible ? 1.0f : 0.0f, now(), Transition{0.3f, Curve::EaseInOut});
    else
        m_reveal.set(visible ? 1.0f : 0.0f);
    invalidateLayout();
}

void SplitViewView::beginResize() { m_resizeStart = m_width > 0 ? m_width : theme().metrics.sidebarWidth; }

void SplitViewView::resizeBy(float dx) {
    m_width = std::clamp(m_resizeStart + dx, m_minWidth, m_maxWidth);
    invalidateLayout();
}

Rect SplitViewView::sidebarPanel() const { return m_sidebar ? m_sidebar->frame() : Rect{}; }

float SplitViewView::titleBarLeadingEdge() const {
    const float inset = theme().metrics.sidebarInset;
    return m_visible || m_reveal.isAnimating(now()) ? std::max(0.0f, sidebarPanel().right()) : inset;
}

Size SplitViewView::measureContent(Size available) { return available; }

void SplitViewView::arrangeContent(const Rect& content) {
    const auto& m = theme().metrics;
    const float inset = m.sidebarInset;
    const float reveal = std::clamp(m_reveal.value(now()), 0.0f, 1.0f);
    if (m_reveal.isAnimating(now())) {
        // Keep laying out every frame while the sidebar slides.
        invalidateLayout();
    }
    const float width = std::clamp(m_width > 0 ? m_width : m.sidebarWidth, m_minWidth, m_maxWidth);
    const float slide = (width + inset) * (1 - reveal);
    Rect panel{content.x + inset - slide, content.y + inset, width, content.height - inset * 2};
    if (m_sidebar) {
        m_sidebar->setHidden(reveal <= 0.0f);
        m_sidebar->measure(panel.size());
        m_sidebar->arrange(panel);
    }
    const float barH = host() ? host()->titleBarHeight() : 0.0f;
    const float detailX = panel.right() + inset * 0.5f;
    // A scrolling detail runs under the toolbar (its content starts below it) so the title bar's
    // scroll-edge glass has something to blur; other content simply starts below the bar.
    auto* scroller = dynamic_cast<ScrollViewView*>(m_detail.get());
    if (scroller) scroller->setContentInsetTop(barH);
    // Custom detail containers can opt in via extendsIntoTitleBar() and inset themselves.
    const bool underBar = scroller || (m_detail && m_detail->extendsIntoTitleBar());
    Rect detail = Rect::fromLTRB(std::max(content.x, detailX), content.y + (underBar ? 0.0f : barH), content.right(),
                                 content.bottom());
    if (m_detail) {
        m_detail->measure(detail.size());
        m_detail->arrange(detail);
    }
    m_handle->setHidden(reveal < 1.0f);
    m_handle->arrange({panel.right() - 2, panel.y, inset + 2, panel.height});
}

void SplitViewView::draw(Painter& p) {
    const Theme& t = theme();
    if (m_reveal.isAnimating(now())) requestAnimationFrame();
    if (m_detail) m_detail->paint(p);
    if (m_sidebar && m_sidebar->isVisible()) {
        // Floating Liquid Glass panel.
        Rect panel = sidebarPanel();
        const float radius = std::max(t.metrics.sidebarRadius, t.metrics.windowRadius - t.metrics.sidebarInset);
        p.drawMaterial(panel, radius, Material::Sidebar);
        p.drawShadow(panel, radius, Color::black(t.isDark() ? 0.45f : 0.16f), 20, {0, 4}, 0, true);
        p.pushClip(panel, radius);
        m_sidebar->paint(p);
        p.popClip();
    }
}

}  // namespace mayo
