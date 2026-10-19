#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mayonnaise/widget.hpp"
#include "mayonnaise/popup.hpp"

namespace mayo {

class TitleBarView;

/// Root of every window: background, content, and the client-side title bar with traffic lights
/// and optional unified toolbar.
class WindowRootView final : public Widget {
public:
    WindowRootView();

    void configure(bool dialog, float cornerRadius, float backgroundOpacity, bool compositorFrame = false);
    void setBackgroundOpacity(float opacity) { m_backgroundOpacity = opacity; }
    void setContent(std::shared_ptr<Widget> content);
    void setToolbar(std::shared_ptr<Widget> toolbar);
    void setTitle(const std::string& title);
    void setTitleVisible(bool visible);
    void setWindowState(bool maximized, bool key, bool transparent, bool fullscreen = false);
    void setChromeHidden(bool hidden);
    [[nodiscard]] float windowRadius() const;
    [[nodiscard]] float titleBarHeight() const;
    [[nodiscard]] bool isKey() const { return m_key; }

    Size measureContent(Size available) override;
    void arrangeContent(const Rect& content) override;
    void draw(Painter& painter) override;
    /// Hairline border and highlight, drawn after overlays so it sits on top of everything.
    void paintWindowFrame(Painter& painter);

    std::function<void()> onClose;
    std::function<void()> onMinimize;
    std::function<void()> onZoom;

private:
    std::shared_ptr<Widget> m_content;
    std::shared_ptr<TitleBarView> m_titleBar;
    bool m_dialog = false;
    bool m_compositorFrame = false;
    float m_radius = -1.0f;
    float m_backgroundOpacity = 1.0f;
    bool m_maximized = false;
    bool m_key = true;
    bool m_transparent = true;
    bool m_fullscreen = false;
    Animated<float> m_chrome{1.0f};
};

}  // namespace mayo
