#pragma once

#include <memory>
#include <string>

#include "../platform/platform.hpp"
#include "event_dispatcher.hpp"
#include "mayonnaise/widget.hpp"
#include "mayonnaise/window.hpp"

namespace mayo {

class Painter;
class WindowRootView;

struct Window::Impl final : WidgetHost {
    Window* self = nullptr;
    std::unique_ptr<platform::PlatformWindow> platformWindow;
    EventDispatcher dispatcher;
    std::shared_ptr<WindowRootView> root;
    std::string title;
    bool needsFrame = true;
    bool needsLayout = true;
    bool closed = false;
    bool hidden = true;
    bool showOnRun = true;
    std::function<void()> closedHandler;
    std::function<bool()> closeRequestHandler;
    void requestClose() {
        if (!closeRequestHandler || closeRequestHandler()) closed = true;
    }
    std::string autosaveName;      // settings key prefix for the remembered size
    bool restoreMaximized = false;  // applied once the window is shown
    void rememberFrame();
    /// Prepares a fresh frame before mapping, then restores a remembered maximized state.
    void show();
    bool keyWindow = true;
    double frameTime = 0;
    bool inFrame = false;
    double wakeAt = -1;

    // Tooltip tracking.
    std::weak_ptr<Widget> tooltipWidget;
    double tooltipSince = 0;
    Point pointer;

    // File drag & drop.
    std::function<void(const std::vector<std::string>&)> dropHandler;
    std::vector<std::string> pendingDrops;
    double dropHoverUntil = -1;

    explicit Impl(const platform::WindowConfig& config);
    ~Impl() override;
    void handleEvent(const platform::PlatformEvent& event);
    void layout();
    void render();
    void themeChanged();
    [[nodiscard]] double nextDeadline() const;
    [[nodiscard]] platform::HitRegion hitRegion(Point p);
    void drawTooltip(Painter& painter);
    void drawDropHighlight(Painter& painter);
    void updateCursor();

    // WidgetHost
    void requestRedraw() override { needsFrame = true; }
    void requestLayout() override {
        needsLayout = true;
        needsFrame = true;
    }
    /// Frame time while rendering (so all animations agree); wall clock while handling events,
    /// so animations started by input don't begin "in the past" after an idle period.
    [[nodiscard]] double time() const override { return inFrame ? frameTime : platform::Platform::now(); }
    void scheduleFrame(double t) override;
    void setFocus(Widget* widget, bool visible) override { dispatcher.setFocus(widget, visible); }
    [[nodiscard]] Widget* focusedWidget() const override { return dispatcher.focused(); }
    [[nodiscard]] bool focusVisible() const override { return dispatcher.focusVisible(); }
    [[nodiscard]] bool isKeyWindow() const override { return keyWindow; }
    void setCursor(CursorShape shape) override;
    void showOverlay(std::shared_ptr<Widget> overlay) override;
    void dismissOverlay(Widget* overlay) override;
    [[nodiscard]] std::string clipboardText() const override;
    void setClipboardText(const std::string& text) override;
    void setTextInput(bool active, Rect caret) override;
    [[nodiscard]] Size windowSize() const override { return platformWindow->sizeInPoints(); }
    [[nodiscard]] float titleBarHeight() const override;
    bool startFileDrag(const std::vector<std::string>& paths) override { return platformWindow->startFileDrag(paths); }
};

}  // namespace mayo
