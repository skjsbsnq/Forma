#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "events.hpp"
#include "geometry.hpp"

namespace mayo {

class Element;

/// A top-level window with client-side macOS-style chrome. Created through App::createWindow.
class Window {
public:
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void setContent(const Element& content);
    /// Toolbar items shown in the unified title bar (typically a mayo::Toolbar).
    void setToolbar(const Element& toolbar);
    /// Accept files dragged onto the window. While files hover, the window shows a drop highlight.
    void onFilesDropped(std::function<void(const std::vector<std::string>&)> handler);
    /// Keys no widget handled (app-wide shortcuts). Return true if consumed.
    void onKey(std::function<bool(const KeyEvent&)> handler);
    void setTitle(const std::string& title);
    /// Hide the title drawn in the title bar (the platform title is kept), for toolbars that show their own.
    void setTitleVisible(bool visible);
    [[nodiscard]] const std::string& title() const;
    [[nodiscard]] Size size() const;
    /// Resizes the window (points), e.g. when switching to a larger mode. Ignored while maximized or full screen.
    void setSize(Size size);
    void setMinimumSize(Size size);
    void close();
    /// Unmaps the window without destroying its content or GPU resources.
    void hide();
    /// Renders fresh content before mapping, then presents and brings the window forward.
    void show();
    [[nodiscard]] bool isVisible() const;
    /// Brings the window to the front (the compositor may only flash it on Wayland).
    void raise();
    /// Asked when the user closes the window (close button, window manager); return false to keep it open,
    /// e.g. to confirm first. close() itself is never vetoed.
    void onCloseRequested(std::function<bool()> handler);
    /// Runs when the window has been closed (by the user or close()), before it is destroyed.
    void onClosed(std::function<void()> handler);
    void setFullscreen(bool fullscreen);
    [[nodiscard]] bool isFullscreen() const;
    /// Fades the title bar (traffic lights, title, toolbar) out or back in — for media playback.
    void setChromeHidden(bool hidden);

    struct Impl;
    [[nodiscard]] Impl& impl() { return *m_impl; }

private:
    friend class App;
    explicit Window(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mayo
