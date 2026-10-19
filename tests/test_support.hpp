#pragma once

#include "core/event_dispatcher.hpp"
#include "mayonnaise/layout.hpp"
#include "mayonnaise/widget.hpp"

namespace mayo::test {

/// Fixed-size leaf; optionally expanding.
class Box : public Widget {
public:
    explicit Box(Size s, bool expandH = false, bool expandV = false) : m_size(s), m_h(expandH), m_v(expandV) {}
    Size measureContent(Size /*available*/) override { return m_size; }
    [[nodiscard]] bool expandsContent(Axis a) const override { return a == Axis::Horizontal ? m_h : m_v; }

private:
    Size m_size;
    bool m_h, m_v;
};

/// Records events; claims pointer presses so it gets captured.
class Probe : public Box {
public:
    using Box::Box;
    int downs = 0, ups = 0, moves = 0, enters = 0, leaves = 0, keys = 0, scrolls = 0;
    bool focusable = false;
    bool clickFocus = false;
    bool focused = false;
    bool onPointerDown(const PointerEvent&) override {
        ++downs;
        return true;
    }
    void onPointerUp(const PointerEvent&) override { ++ups; }
    void onPointerMove(const PointerEvent&) override { ++moves; }
    void onPointerEnter() override { ++enters; }
    void onPointerLeave() override { ++leaves; }
    bool onKeyDown(const KeyEvent&) override {
        ++keys;
        return true;
    }
    bool onScroll(const ScrollEvent&) override {
        ++scrolls;
        return true;
    }
    [[nodiscard]] bool acceptsFocus() const override { return focusable; }
    [[nodiscard]] bool focusOnClick() const override { return clickFocus; }
    void onFocusChanged(bool f) override { focused = f; }
};

class TestHost : public WidgetHost {
public:
    EventDispatcher dispatcher;
    int redraws = 0;
    int layouts = 0;
    double clock = 0;
    void requestRedraw() override { ++redraws; }
    void requestLayout() override { ++layouts; }
    [[nodiscard]] double time() const override { return clock; }
    void scheduleFrame(double) override {}
    void setFocus(Widget* w, bool visible) override { dispatcher.setFocus(w, visible); }
    [[nodiscard]] Widget* focusedWidget() const override { return dispatcher.focused(); }
    [[nodiscard]] bool focusVisible() const override { return dispatcher.focusVisible(); }
    [[nodiscard]] bool isKeyWindow() const override { return true; }
    void setCursor(CursorShape) override {}
    void showOverlay(std::shared_ptr<Widget> o) override {
        o->setHost(this);
        dispatcher.pushOverlay(std::move(o));
    }
    void dismissOverlay(Widget* o) override { dispatcher.removeOverlay(o); }
    [[nodiscard]] std::string clipboardText() const override { return clip; }
    void setClipboardText(const std::string& t) override { clip = t; }
    bool textInputActive = false;
    bool secureTextInput = false;
    bool secureWhenStarted = false;
    void setTextInput(bool active, Rect) override {
        textInputActive = active;
        if (active) secureWhenStarted = secureTextInput;
    }
    void setSecureTextInput(bool secure) override { secureTextInput = secure; }
    [[nodiscard]] Size windowSize() const override { return {800, 600}; }
    std::string clip;

    void attach(const std::shared_ptr<Widget>& root, Size size = {800, 600}) {
        root->setHost(this);
        dispatcher.setRoot(root);
        root->measure(size);
        root->arrange({0, 0, size.width, size.height});
    }
};

}  // namespace mayo::test
