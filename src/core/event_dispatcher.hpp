#pragma once

// Routes pointer/keyboard input through a widget tree: hit testing, hover enter/leave along the
// path, pointer capture while a button is held, focus and Tab navigation, modal overlays.
// Free of platform code so it can be unit tested with synthetic events.

#include <memory>
#include <vector>

#include "mayonnaise/events.hpp"
#include "mayonnaise/widget.hpp"

namespace mayo {

class EventDispatcher {
public:
    void setRoot(std::shared_ptr<Widget> root);
    [[nodiscard]] Widget* root() const { return m_root.get(); }

    /// Overlays are modal: they get input first, and a press outside the topmost dismisses it.
    void pushOverlay(std::shared_ptr<Widget> overlay);
    void removeOverlay(Widget* overlay);
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& overlays() const { return m_overlays; }

    void pointerMove(const PointerEvent& e);
    void pointerDown(const PointerEvent& e);
    void pointerUp(const PointerEvent& e);
    /// Ends a press without a release from the platform (the system took the pointer for a drag).
    void cancelPointer();
    void pointerLeave();
    void scroll(const ScrollEvent& e);
    void magnify(const MagnifyEvent& e);
    /// Returns whether anything (widget, Tab navigation, the unhandled-key hook, overlay Escape) used it.
    bool keyDown(const KeyEvent& e);
    void textInput(const TextInputEvent& e);
    void textEditing(const TextEditingEvent& e);
    void cancelComposition();
    /// Clears unavailable focus and advances safely; returns whether the old focus was invalid.
    bool validateFocus();

    void setFocus(Widget* widget, bool visible);
    [[nodiscard]] Widget* focused() const;
    [[nodiscard]] bool focusVisible() const { return m_focusVisible; }
    /// Moves focus to the next/previous focusable widget in tree order.
    void focusNext(bool backwards);

    [[nodiscard]] Widget* hovered() const;
    [[nodiscard]] Widget* captured() const;
    [[nodiscard]] CursorShape cursor() const;
    [[nodiscard]] Widget* hitTest(Point p) const;
    /// Re-evaluates hover after layout changes moved things under a stationary pointer.
    void refreshHover();

    /// Called before an overlay is dismissed so callers can run its dismissal callback.
    std::function<void(Widget*)> onOverlayDismissed;
    /// Keys nobody in the tree handled.
    std::function<bool(const KeyEvent&)> onUnhandledKey;

private:
    [[nodiscard]] bool attached(const Widget* w) const;
    [[nodiscard]] bool inFocusScope(const Widget* w) const;
    [[nodiscard]] bool validFocus(const Widget* w) const;
    void updateHover(Widget* target);
    void collectFocusable(Widget* w, std::vector<Widget*>& out) const;

    std::shared_ptr<Widget> m_root;
    std::vector<std::shared_ptr<Widget>> m_overlays;
    struct SavedFocus {
        std::weak_ptr<Widget> widget;
        bool visible = false;
    };
    std::vector<SavedFocus> m_overlayFocus;
    std::vector<std::weak_ptr<Widget>> m_hoverPath;  // root ... deepest
    std::weak_ptr<Widget> m_captured;
    std::weak_ptr<Widget> m_focused;
    // A touchpad gesture stays with the widget that took its first event (macOS latching): content moving
    // under the pointer, or reaching an edge, must not hand the rest of the swipe to another scroller.
    std::weak_ptr<Widget> m_scrollLatch;
    double m_scrollLatchTime = -1;
    bool m_focusVisible = false;
    Point m_lastPointer{-1e6f, -1e6f};
    bool m_pointerInside = false;
};

}  // namespace mayo
