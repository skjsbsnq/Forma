#pragma once

#include <functional>
#include <memory>

#include <optional>

#include "widget.hpp"

namespace mayo {

/// Mixin for overlay widgets that position themselves (menus, popovers) and want to know when the
/// window dismissed them (outside click, Escape).
class PopupHost {
public:
    virtual ~PopupHost() = default;
    [[nodiscard]] virtual Rect placePopup(Size window, Size measured) = 0;
    virtual void popupDismissed() {}
};

enum class PopupEdge : std::uint8_t { Below, Above };

/// Base for floating glass panels anchored to a widget: positioning, clamping to the window,
/// appear/dismiss animation, shadow and material.
class PopupView : public Widget, public PopupHost {
public:
    explicit PopupView(Material material = Material::Popover);

    /// Shows the popup anchored to `anchor` (its frame at the time of the call).
    void showFrom(Widget& anchor, PopupEdge edge = PopupEdge::Below);
    /// Anchors the popup to this rect (window points) on the next showFrom().
    void setAnchorRect(Rect rect) { m_anchorOverride = rect; }
    void dismiss();
    [[nodiscard]] bool isShown() const { return m_shown; }
    void setOnDismiss(std::function<void()> fn) { m_onDismiss = std::move(fn); }
    /// Gap between anchor and panel; negative values overlap (pop-up menus).
    void setGap(float gap) { m_gap = gap; }
    void setMinimumWidth(float w) { m_minWidth = w; }
    /// Centre horizontally on the anchor (popovers) instead of aligning leading edges (menus).
    void setCentered(bool c) { m_centered = c; }

    Rect placePopup(Size window, Size measured) override;
    void popupDismissed() override;
    void draw(Painter& painter) override;
    [[nodiscard]] float cornerRadiusValue() const;

protected:
    /// Draws the panel content (default: children).
    virtual void drawPanel(Painter& painter) { paintChildren(painter); }

    Rect m_anchor;
    std::optional<Rect> m_anchorOverride;
    PopupEdge m_edge = PopupEdge::Below;
    Material m_material;
    float m_gap = 6;
    float m_minWidth = 0;
    bool m_shown = false;
    bool m_centered = false;
    Animated<float> m_appear{0.0f};
    std::function<void()> m_onDismiss;
    WidgetHost* m_shownHost = nullptr;
};

}  // namespace mayo
