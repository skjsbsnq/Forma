#include "event_dispatcher.hpp"

#include <algorithm>

namespace mayo {

namespace {
std::shared_ptr<Widget> lock(Widget* w) { return w ? w->shared_from_this() : nullptr; }
}  // namespace

bool EventDispatcher::attached(const Widget* w) const {
    if (!w) return false;
    if (m_root && m_root->isAncestorOf(w)) return true;
    return std::any_of(m_overlays.begin(), m_overlays.end(), [&](const auto& o) { return o->isAncestorOf(w); });
}

void EventDispatcher::setRoot(std::shared_ptr<Widget> root) {
    if (m_root == root) return;
    setFocus(nullptr, false);
    m_root = std::move(root);
}

bool EventDispatcher::inFocusScope(const Widget* w) const {
    if (!w) return false;
    if (m_overlays.empty()) return m_root && m_root->isAncestorOf(w);
    const auto* owner = m_overlays.back()->focusScopeOwner();
    return std::any_of(m_overlays.begin(), m_overlays.end(), [&](const auto& overlay) {
        return overlay->focusScopeOwner() == owner && overlay->isAncestorOf(w);
    });
}

bool EventDispatcher::validFocus(const Widget* w) const {
    if (!attached(w) || !inFocusScope(w)) return false;
    for (auto* ancestor = w; ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->isVisible() || !ancestor->isEnabled()) return false;
    }
    return true;
}

void EventDispatcher::pushOverlay(std::shared_ptr<Widget> overlay) {
    m_overlayFocus.push_back({m_focused, m_focusVisible});
    m_overlays.push_back(std::move(overlay));
    validateFocus();
    refreshHover();
}

void EventDispatcher::removeOverlay(Widget* overlay) {
    auto it = std::find_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) { return o.get() == overlay; });
    if (it == m_overlays.end()) return;
    auto keep = *it;  // keep alive through callbacks
    const size_t index = static_cast<size_t>(it - m_overlays.begin());
    const bool top = index + 1 == m_overlays.size();
    const SavedFocus saved = m_overlayFocus[index];
    if (auto f = m_focused.lock(); f && overlay->isAncestorOf(f.get())) setFocus(nullptr, false);
    for (size_t i = index + 1; i < m_overlayFocus.size(); ++i) {
        if (auto previous = m_overlayFocus[i].widget.lock(); previous && overlay->isAncestorOf(previous.get())) {
            m_overlayFocus[i] = saved;
        }
    }
    m_overlays.erase(it);
    m_overlayFocus.erase(m_overlayFocus.begin() + static_cast<std::ptrdiff_t>(index));
    if (top) {
        if (auto previous = saved.widget.lock(); previous && validFocus(previous.get())) {
            setFocus(previous.get(), saved.visible);
        } else {
            validateFocus();
            if (!focused()) focusNext(false);
        }
    } else {
        validateFocus();
    }
    refreshHover();
}

Widget* EventDispatcher::hitTest(Point p) const {
    if (!m_overlays.empty()) {
        const auto* owner = m_overlays.back()->focusScopeOwner();
        for (auto it = m_overlays.rbegin(); it != m_overlays.rend(); ++it) {
            if ((*it)->focusScopeOwner() == owner) {
                if (auto* hit = (*it)->hitTest(p)) return hit;
            }
        }
        return nullptr;
    }
    return m_root ? m_root->hitTest(p) : nullptr;
}

Widget* EventDispatcher::hovered() const {
    if (m_hoverPath.empty()) return nullptr;
    auto w = m_hoverPath.back().lock();
    return w && attached(w.get()) ? w.get() : nullptr;
}

Widget* EventDispatcher::captured() const {
    auto w = m_captured.lock();
    return w && attached(w.get()) ? w.get() : nullptr;
}

Widget* EventDispatcher::focused() const {
    auto w = m_focused.lock();
    // Layout owners need the old focus while redirecting a newly collapsed control.
    // Input dispatch separately validates availability and the current overlay scope.
    return w && attached(w.get()) ? w.get() : nullptr;
}

CursorShape EventDispatcher::cursor() const {
    if (Widget* c = captured()) return c->cursor();
    if (Widget* h = hovered()) return h->isEnabled() ? h->cursor() : CursorShape::Arrow;
    return CursorShape::Arrow;
}

void EventDispatcher::updateHover(Widget* target) {
    std::vector<Widget*> path;
    for (Widget* w = target; w; w = w->parent()) path.push_back(w);
    std::reverse(path.begin(), path.end());

    // Leave widgets no longer on the path (deepest first), then enter new ones (outermost first).
    std::vector<std::shared_ptr<Widget>> old;
    for (auto& weak : m_hoverPath) {
        if (auto w = weak.lock()) old.push_back(std::move(w));
    }
    for (auto it = old.rbegin(); it != old.rend(); ++it) {
        if (std::find(path.begin(), path.end(), it->get()) == path.end()) {
            (*it)->setHoveredState(false);
            (*it)->onPointerLeave();
        }
    }
    for (Widget* w : path) {
        bool wasHovered = std::any_of(old.begin(), old.end(), [&](const auto& o) { return o.get() == w; });
        if (!wasHovered) {
            w->setHoveredState(true);
            w->onPointerEnter();
        }
    }
    m_hoverPath.clear();
    for (Widget* w : path) m_hoverPath.push_back(w->weak_from_this());
}

void EventDispatcher::refreshHover() {
    if (captured()) return;
    updateHover(m_pointerInside ? hitTest(m_lastPointer) : nullptr);
}

void EventDispatcher::pointerMove(const PointerEvent& e) {
    m_lastPointer = e.position;
    m_pointerInside = true;
    if (auto c = lock(captured())) {
        c->setPressedState(c->frame().contains(e.position));
        c->onPointerMove(e);
        return;
    }
    updateHover(hitTest(e.position));
    for (auto& weak : m_hoverPath) {
        if (auto w = weak.lock(); w && w->isEnabled()) w->onPointerMove(e);
    }
}

void EventDispatcher::pointerDown(const PointerEvent& e) {
    m_lastPointer = e.position;
    m_pointerInside = true;
    if (!m_overlays.empty() && !hitTest(e.position)) {
        // Outside a menu cascade dismisses the entire cascade, not only its newest submenu.
        auto top = m_overlays.back();
        const auto* owner = top->focusScopeOwner();
        auto found = std::find_if(m_overlays.begin(), m_overlays.end(), [&](const auto& o) { return o.get() == owner; });
        auto dismissed = found != m_overlays.end() ? *found : top;
        if (onOverlayDismissed) onOverlayDismissed(dismissed.get());
        removeOverlay(dismissed.get());
        return;
    }
    updateHover(hitTest(e.position));
    Widget* target = hovered();
    if (!target) return;

    for (Widget* w = target; w; w = w->parent()) {
        if (w->acceptsFocus() && w->focusOnClick() && w->isEnabled()) {
            setFocus(w, false);
            break;
        }
    }
    for (Widget* w = target; w; w = w->parent()) {
        if (!w->isEnabled()) return;
        if (w->onPointerDown(e)) {
            auto keep = lock(w);
            m_captured = keep;
            keep->setPressedState(true);
            return;
        }
    }
}

void EventDispatcher::cancelPointer() {
    if (auto c = lock(captured())) {
        m_captured.reset();
        c->setPressedState(false);
        c->onPointerUp(PointerEvent{{-1e6f, -1e6f}, MouseButton::Left, 0, Modifiers::None});
    }
}

void EventDispatcher::pointerUp(const PointerEvent& e) {
    m_lastPointer = e.position;
    if (auto c = lock(captured())) {
        m_captured.reset();
        c->setPressedState(false);
        c->onPointerUp(e);
    }
    m_captured.reset();
    refreshHover();
}

void EventDispatcher::pointerLeave() {
    m_pointerInside = false;
    if (captured()) return;
    updateHover(nullptr);
}

void EventDispatcher::scroll(const ScrollEvent& e) {
    m_lastPointer = e.position;
    // Events of one gesture arrive a few ms apart; without end events a pause this long starts a new one.
    // With them, the gap only guards against an end event that never came.
    constexpr double kLatchGap = 0.25;
    constexpr double kLatchGapWithEnds = 1.0;
    const double t = m_root && m_root->host() ? m_root->host()->time() : 0.0;
    auto latched = m_scrollLatch.lock();
    if (latched && !attached(latched.get())) latched.reset();
    if (e.precise && latched && (e.phase == ScrollPhase::Ended || t - m_scrollLatchTime < (e.reportsEnd ? kLatchGapWithEnds : kLatchGap))) {
        if (latched->isEnabled()) latched->onScroll(e);
        m_scrollLatchTime = t;
        if (e.phase == ScrollPhase::Ended) m_scrollLatch.reset();
        refreshHover();
        return;
    }
    m_scrollLatch.reset();
    if (e.phase == ScrollPhase::Ended) return;  // nobody is tracking this gesture
    for (Widget* w = hitTest(e.position); w; w = w->parent()) {
        if (w->isEnabled() && w->onScroll(e)) {
            if (e.precise) {
                m_scrollLatch = w->weak_from_this();
                m_scrollLatchTime = t;
            }
            break;
        }
    }
    refreshHover();
}

void EventDispatcher::magnify(const MagnifyEvent& e) {
    for (Widget* w = hitTest(e.position); w; w = w->parent()) {
        if (w->isEnabled() && w->onMagnify(e)) break;
    }
}

bool EventDispatcher::keyDown(const KeyEvent& e) {
    if (validateFocus()) return true;
    auto f = lock(focused());
    // SDL has already delivered this key to the input method; it must not also submit a form or rename.
    if (f && f->isComposing()) return true;
    Widget* start = f ? f.get() : (!m_overlays.empty() ? m_overlays.back().get() : m_root.get());
    for (Widget* w = start; w; w = w->parent()) {
        if (w->isEnabled() && w->onKeyDown(e)) return true;
    }
    if (e.key == Key::Tab && !e.primary() && !e.alt()) {
        focusNext(e.shift());
        return true;
    }
    if (m_overlays.empty() && onUnhandledKey && onUnhandledKey(e)) return true;
    if (e.key == Key::Escape && !m_overlays.empty()) {
        auto top = m_overlays.back();
        if (onOverlayDismissed) onOverlayDismissed(top.get());
        removeOverlay(top.get());
        return true;
    }
    return false;
}

void EventDispatcher::textInput(const TextInputEvent& e) {
    if (validateFocus()) return;
    if (auto f = lock(focused())) f->onTextInput(e);
}

void EventDispatcher::textEditing(const TextEditingEvent& e) {
    if (validateFocus()) return;
    if (auto f = lock(focused())) f->onTextEditing(e);
}

void EventDispatcher::cancelComposition() {
    if (auto f = m_focused.lock()) f->cancelComposition();
}

bool EventDispatcher::validateFocus() {
    auto old = m_focused.lock();
    if (!old || validFocus(old.get())) return false;
    setFocus(nullptr, false);
    focusNext(false);
    return true;
}

void EventDispatcher::setFocus(Widget* widget, bool visible) {
    if (widget && !validFocus(widget)) return;
    auto old = m_focused.lock();
    m_focusVisible = widget && visible;
    if (old.get() == widget) {
        if (widget) widget->invalidate();
        return;
    }
    m_focused = widget ? widget->weak_from_this() : std::weak_ptr<Widget>{};
    if (old) {
        old->cancelComposition();
        old->onFocusChanged(false);
        old->invalidate();
    }
    if (widget) {
        widget->onFocusChanged(true);
        widget->invalidate();
    }
}

void EventDispatcher::collectFocusable(Widget* w, std::vector<Widget*>& out) const {
    if (!w->isVisible() || !w->isEnabled()) return;
    if (w->acceptsFocus()) out.push_back(w);
    for (auto& c : w->children()) collectFocusable(c.get(), out);
}

void EventDispatcher::focusNext(bool backwards) {
    std::vector<Widget*> order;
    Widget* scope = !m_overlays.empty() ? m_overlays.back().get() : m_root.get();
    if (!scope) return;
    collectFocusable(scope, order);
    if (order.empty()) return;
    Widget* cur = focused();
    auto it = std::find(order.begin(), order.end(), cur);
    size_t index = 0;
    if (it == order.end()) {
        index = backwards ? order.size() - 1 : 0;
    } else {
        auto pos = static_cast<size_t>(it - order.begin());
        index = backwards ? (pos + order.size() - 1) % order.size() : (pos + 1) % order.size();
    }
    setFocus(order[index], true);
}

}  // namespace mayo
