#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "animation.hpp"
#include "color.hpp"
#include "events.hpp"
#include "geometry.hpp"
#include "theme.hpp"

namespace mayo {

class Painter;
class Widget;

enum class Axis : std::uint8_t { Horizontal, Vertical };
enum class HAlign : std::uint8_t { Leading, Center, Trailing };
enum class VAlign : std::uint8_t { Top, Center, Bottom };

/// 2D alignment. Stacks use the cross-axis component to place children and the main-axis
/// component to place the whole group when there is spare space.
struct Alignment {
    HAlign horizontal = HAlign::Leading;
    VAlign vertical = VAlign::Top;

    static const Alignment TopLeading, Top, TopTrailing, Leading, Center, Trailing, BottomLeading, Bottom,
        BottomTrailing;
    constexpr bool operator==(const Alignment&) const = default;
};
inline constexpr Alignment Alignment::TopLeading{HAlign::Leading, VAlign::Top};
inline constexpr Alignment Alignment::Top{HAlign::Center, VAlign::Top};
inline constexpr Alignment Alignment::TopTrailing{HAlign::Trailing, VAlign::Top};
inline constexpr Alignment Alignment::Leading{HAlign::Leading, VAlign::Center};
inline constexpr Alignment Alignment::Center{HAlign::Center, VAlign::Center};
inline constexpr Alignment Alignment::Trailing{HAlign::Trailing, VAlign::Center};
inline constexpr Alignment Alignment::BottomLeading{HAlign::Leading, VAlign::Bottom};
inline constexpr Alignment Alignment::Bottom{HAlign::Center, VAlign::Bottom};
inline constexpr Alignment Alignment::BottomTrailing{HAlign::Trailing, VAlign::Bottom};

/// Services a widget tree needs from its window. Implemented by the window; widgets reach it via
/// Widget::host(). Custom widgets rarely need it directly.
class WidgetHost {
public:
    virtual ~WidgetHost() = default;
    virtual void requestRedraw() = 0;
    virtual void requestLayout() = 0;
    /// Frame clock (seconds). Constant during one frame so animations stay in sync.
    [[nodiscard]] virtual double time() const = 0;
    /// Ensures a frame is rendered no later than `time` (timers: caret blink, tooltips).
    virtual void scheduleFrame(double time) = 0;
    virtual void setFocus(Widget* widget, bool visible) = 0;
    [[nodiscard]] virtual Widget* focusedWidget() const = 0;
    [[nodiscard]] virtual bool focusVisible() const = 0;
    [[nodiscard]] virtual bool isKeyWindow() const = 0;
    virtual void setCursor(CursorShape shape) = 0;
    /// Shows `overlay` above all content (menus, popovers, tooltips). It is laid out by itself.
    virtual void showOverlay(std::shared_ptr<Widget> overlay) = 0;
    virtual void dismissOverlay(Widget* overlay) = 0;
    [[nodiscard]] virtual std::string clipboardText() const = 0;
    virtual void setClipboardText(const std::string& text) = 0;
    virtual void setTextInput(bool active, Rect caret) = 0;
    [[nodiscard]] virtual Size windowSize() const = 0;
    /// Height of the window's title bar / toolbar strip. Full-size content insets by this.
    [[nodiscard]] virtual float titleBarHeight() const { return 0; }
    /// Hands a drag of files over to the system, so they can be dropped on other apps (Wayland). Call from
    /// onPointerMove while the button is still down, typically when the pointer leaves the window. The
    /// widget holding the pointer then gets onPointerUp with a position far outside the window once the
    /// drop happened elsewhere. Returns false when the platform can't do it.
    virtual bool startFileDrag(const std::vector<std::string>& /*paths*/) { return false; }
};

/// A node in the retained widget tree. Subclass to make custom widgets: override measureContent,
/// arrangeContent, draw, and the event handlers you need.
class Widget : public std::enable_shared_from_this<Widget> {
public:
    Widget();
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // Tree ------------------------------------------------------------------------------
    [[nodiscard]] Widget* parent() const { return m_parent; }
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const { return m_children; }
    void addChild(std::shared_ptr<Widget> child);
    void insertChild(size_t index, std::shared_ptr<Widget> child);
    void removeChild(Widget* child);
    void clearChildren();
    [[nodiscard]] bool isAncestorOf(const Widget* other) const;
    [[nodiscard]] WidgetHost* host() const;
    /// Only the window's root (or an overlay root) has its host set directly.
    void setHost(WidgetHost* host) { m_host = host; }

    // Layout ----------------------------------------------------------------------------
    /// Size this widget wants given the space available (kInfinity = unconstrained). Applies padding
    /// and frame modifiers around measureContent, and caches the result until invalidated.
    Size measure(Size available);
    /// Places the widget; frame is in window points.
    void arrange(const Rect& frame);
    [[nodiscard]] const Rect& frame() const { return m_frame; }
    [[nodiscard]] Rect contentRect() const { return m_frame.inset(m_padding); }
    /// Whether the widget grows to fill free space along `axis` when its parent has some.
    [[nodiscard]] bool expands(Axis axis) const;
    [[nodiscard]] float flexFactor() const { return m_flex; }
    /// Set by stacks so axis-dependent widgets (Spacer, Divider) know their orientation.
    void setParentAxis(std::optional<Axis> axis) { m_parentAxis = axis; }
    [[nodiscard]] std::optional<Axis> parentAxis() const { return m_parentAxis; }

    void invalidate();        ///< redraw
    void invalidateLayout();  ///< re-measure + redraw

    // Painting --------------------------------------------------------------------------
    /// Draws background modifiers, then draw(), handling hidden/disabled/opacity.
    void paint(Painter& painter);
    /// Draws children; called by the default draw().
    void paintChildren(Painter& painter);

    // Hit testing -----------------------------------------------------------------------
    /// Deepest widget at `p` (window points), or nullptr. Default: children in reverse order, then self.
    virtual Widget* hitTest(Point p);

    // State -----------------------------------------------------------------------------
    [[nodiscard]] bool isHovered() const { return m_hovered; }
    [[nodiscard]] bool isPressed() const { return m_pressed; }
    [[nodiscard]] bool isFocused() const;
    [[nodiscard]] bool isFocusVisible() const;
    [[nodiscard]] bool isEnabled() const;
    [[nodiscard]] bool isVisible() const { return !m_hidden && !m_layoutSuppressed; }
    /// Frame time from the host (0 when detached).
    [[nodiscard]] double now() const;
    /// Samples an animation and keeps frames coming while it runs.
    template <class T>
    T sample(const Animated<T>& a) {
        double t = now();
        if (a.isAnimating(t)) requestAnimationFrame();
        return a.value(t);
    }
    void requestAnimationFrame();

    // Modifier storage (the fluent handle API writes these) --------------------------------
    void setPadding(const Insets& padding);
    [[nodiscard]] const Insets& padding() const { return m_padding; }
    void setFixedWidth(std::optional<float> w);
    void setFixedHeight(std::optional<float> h);
    void setMinSize(std::optional<float> minW, std::optional<float> minH);
    void setMaxSize(std::optional<float> maxW, std::optional<float> maxH);
    void setFlex(float flex);
    void setBackground(std::optional<Color> color);
    void setBackgroundMaterial(std::optional<Material> material);
    void setCornerRadius(const CornerRadii& radii);
    [[nodiscard]] const CornerRadii& cornerRadius() const { return m_cornerRadius; }
    void setForeground(std::optional<Color> color);
    [[nodiscard]] std::optional<Color> foreground() const;  ///< inherited from ancestors
    void setDisabled(bool disabled);
    [[nodiscard]] bool isDisabledSelf() const { return m_disabled; }
    void setHidden(bool hidden);
    void setOpacity(float opacity);
    void setTooltip(std::string text);
    [[nodiscard]] const std::string& tooltip() const { return m_tooltip; }
    void setCursor(std::optional<CursorShape> cursor) { m_cursor = cursor; }
    /// Cursor shown while hovering this widget (inherits from ancestors; defaults to arrow).
    [[nodiscard]] CursorShape cursor() const;
    /// Marks this region as a window drag handle (title bars, toolbars).
    void setDragRegion(bool drag) { m_dragRegion = drag; }
    [[nodiscard]] bool isDragRegion() const { return m_dragRegion; }

    // Overridables -----------------------------------------------------------------------
    virtual Size measureContent(Size available);
    virtual void arrangeContent(const Rect& content);
    virtual void draw(Painter& painter);
    /// Drawn after children (focus rings, overlays within the widget).
    virtual void drawOverlay(Painter& painter);
    [[nodiscard]] virtual bool expandsContent(Axis /*axis*/) const { return false; }
    /// Root content only: draw under the title bar (sidebar layouts). The content then insets
    /// itself by host()->titleBarHeight() where needed.
    [[nodiscard]] virtual bool extendsIntoTitleBar() const { return false; }
    /// Root content only: x (window points) where the title and toolbar may start, e.g. the
    /// trailing edge of a sidebar.
    [[nodiscard]] virtual float titleBarLeadingEdge() const { return 0; }

    [[nodiscard]] virtual bool acceptsFocus() const { return false; }
    /// Controls return true so title-bar drag regions don't swallow their clicks.
    [[nodiscard]] virtual bool isInteractive() const { return acceptsFocus(); }
    /// Whether a click gives focus (text fields) or only keyboard navigation does (buttons).
    [[nodiscard]] virtual bool focusOnClick() const { return false; }

    virtual bool onPointerDown(const PointerEvent& /*e*/) { return false; }
    virtual void onPointerMove(const PointerEvent& /*e*/) {}
    virtual void onPointerUp(const PointerEvent& /*e*/) {}
    virtual void onPointerEnter() {}
    virtual void onPointerLeave() {}
    virtual bool onScroll(const ScrollEvent& /*e*/) { return false; }
    virtual bool onMagnify(const MagnifyEvent& /*e*/) { return false; }
    virtual bool onKeyDown(const KeyEvent& /*e*/) { return false; }
    virtual bool onTextInput(const TextInputEvent& /*e*/) { return false; }
    virtual void onFocusChanged(bool /*focused*/) {}
    virtual void onHoverChanged(bool /*hovered*/) {}
    virtual void onPressedChanged(bool /*pressed*/) {}
    virtual void onEnabledChanged() {}
    /// Called when the theme (appearance or accent) changes.
    virtual void onThemeChanged() {}
    /// Called after a child is attached (stacks use it to tell children their axis).
    virtual void onChildAdded(Widget& /*child*/) {}

    // Called by the event dispatcher.
    void setHoveredState(bool hovered);
    void setPressedState(bool pressed);
    void notifyThemeChanged();

private:
    friend class ToolbarView;
    // Container-owned visibility, separate from the application's hidden state.
    bool m_layoutSuppressed = false;
    Widget* m_parent = nullptr;
    WidgetHost* m_host = nullptr;
    std::vector<std::shared_ptr<Widget>> m_children;
    Rect m_frame;

    Insets m_padding;
    std::optional<float> m_width, m_height;
    float m_minW = 0, m_minH = 0;
    std::optional<float> m_maxW, m_maxH;  // unset: the widget decides (expandsContent)
    float m_flex = 1;
    std::optional<Axis> m_parentAxis;

    std::optional<Color> m_background;
    std::optional<Material> m_material;
    CornerRadii m_cornerRadius;
    std::optional<Color> m_foreground;
    std::optional<CursorShape> m_cursor;
    std::string m_tooltip;
    float m_opacity = 1;
    bool m_disabled = false;
    bool m_hidden = false;
    bool m_dragRegion = false;

    bool m_hovered = false;
    bool m_pressed = false;

    bool m_layoutDirty = true;
    bool m_hasCache = false;
    Size m_cacheAvailable;
    Size m_cacheResult;
};

/// Type-erased, copyable reference to a widget; what containers take as children.
class Element {
public:
    Element() = default;
    Element(std::shared_ptr<Widget> widget) : m_widget(std::move(widget)) {}  // NOLINT(google-explicit-constructor)
    template <class W>
        requires std::derived_from<W, Widget>
    Element(std::shared_ptr<W> widget) : m_widget(std::move(widget)) {}  // NOLINT(google-explicit-constructor)
    template <class H>
        requires requires(const H& h) {
            { h.node() } -> std::convertible_to<std::shared_ptr<Widget>>;
        }
    Element(const H& handle) : m_widget(handle.node()) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] const std::shared_ptr<Widget>& widget() const { return m_widget; }
    explicit operator bool() const { return m_widget != nullptr; }

private:
    std::shared_ptr<Widget> m_widget;
};

/// CRTP base for widget handles: shared ownership of a node plus the fluent modifier API.
template <class Derived, class Node>
class WidgetHandle {
public:
    [[nodiscard]] const std::shared_ptr<Node>& node() const { return m_node; }
    Node* operator->() const { return m_node.get(); }

    Derived& padding(const Insets& insets) { return apply([&](Node& n) { n.setPadding(insets); }); }
    Derived& padding(float vertical, float horizontal) { return padding(Insets(vertical, horizontal)); }
    Derived& frame(float width, float height) {
        return apply([&](Node& n) {
            n.setFixedWidth(width);
            n.setFixedHeight(height);
        });
    }
    Derived& width(float w) { return apply([&](Node& n) { n.setFixedWidth(w); }); }
    Derived& height(float h) { return apply([&](Node& n) { n.setFixedHeight(h); }); }
    Derived& minWidth(float w) { return apply([&](Node& n) { n.setMinSize(w, std::nullopt); }); }
    Derived& minHeight(float h) { return apply([&](Node& n) { n.setMinSize(std::nullopt, h); }); }
    Derived& maxWidth(float w) { return apply([&](Node& n) { n.setMaxSize(w, std::nullopt); }); }
    Derived& maxHeight(float h) { return apply([&](Node& n) { n.setMaxSize(std::nullopt, h); }); }
    /// Grow to fill free horizontal / vertical space.
    Derived& fillWidth() { return maxWidth(kInfinity); }
    Derived& fillHeight() { return maxHeight(kInfinity); }
    /// Relative share of free space among expanding siblings (default 1).
    Derived& flex(float f) { return apply([&](Node& n) { n.setFlex(f); }); }
    Derived& foreground(const Color& c) { return apply([&](Node& n) { n.setForeground(c); }); }
    Derived& background(const Color& c) { return apply([&](Node& n) { n.setBackground(c); }); }
    Derived& background(Material m) { return apply([&](Node& n) { n.setBackgroundMaterial(m); }); }
    Derived& cornerRadius(const CornerRadii& r) { return apply([&](Node& n) { n.setCornerRadius(r); }); }
    Derived& disabled(bool d = true) { return apply([&](Node& n) { n.setDisabled(d); }); }
    Derived& hidden(bool h = true) { return apply([&](Node& n) { n.setHidden(h); }); }
    Derived& opacity(float o) { return apply([&](Node& n) { n.setOpacity(o); }); }
    Derived& tooltip(std::string text) { return apply([&](Node& n) { n.setTooltip(std::move(text)); }); }

    void setDisabled(bool d) { m_node->setDisabled(d); }
    void setHidden(bool h) { m_node->setHidden(h); }

protected:
    explicit WidgetHandle(std::shared_ptr<Node> node) : m_node(std::move(node)) {}

    template <class F>
    Derived& apply(F&& f) {
        f(*m_node);
        return static_cast<Derived&>(*this);
    }

    std::shared_ptr<Node> m_node;
};

/// Wraps a user-defined Widget subclass in a handle so it gets the modifier API:
/// `mayo::Custom<MyGraph>(args...).frame(200, 100)`.
template <class W>
class Custom : public WidgetHandle<Custom<W>, W> {
public:
    template <class... Args>
        requires std::constructible_from<W, Args...>
    explicit Custom(Args&&... args) : WidgetHandle<Custom<W>, W>(std::make_shared<W>(std::forward<Args>(args)...)) {}
};

}  // namespace mayo
