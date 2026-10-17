#include "mayonnaise/ui_translation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../render/font.hpp"
#include "../render/rhi/rhi.hpp"
#include "../render/renderer2d.hpp"
#include "app_impl.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/painter.hpp"
#include "window_impl.hpp"
#include "window_root.hpp"

namespace mayo {

void drawRendererTestScene(render::Renderer2D& r, Size size, const Theme& theme);
void drawTextTestScene(Painter& p, Size size, const Theme& theme);

namespace {
constexpr double kTooltipDelay = 0.9;
constexpr float kResizeBorder = 6.0f;
// Wheel notches and touchpad units to points (tuned to feel like macOS on libinput devices).
constexpr float kWheelStep = 40.0f;
constexpr float kPreciseStep = 14.0f;
}  // namespace

// --- Window (public) ---------------------------------------------------------------------------

Window::Window(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) { m_impl->self = this; }
Window::~Window() = default;

void Window::setContent(const Element& content) {
    m_impl->root->setContent(content.widget());
    m_impl->requestLayout();
}

void Window::setToolbar(const Element& toolbar) {
    m_impl->root->setToolbar(toolbar.widget());
    m_impl->requestLayout();
}

float Window::Impl::titleBarHeight() const { return root->titleBarHeight(); }

void Window::onKey(std::function<bool(const KeyEvent&)> handler) {
    m_impl->dispatcher.onUnhandledKey = std::move(handler);
}

void Window::onFilesDropped(std::function<void(const std::vector<std::string>&)> handler) {
    m_impl->dropHandler = std::move(handler);
}

void Window::setTitle(const std::string& title) {
    m_impl->title = uiText(title);
    m_impl->platformWindow->setTitle(uiText(title));
    m_impl->root->setTitle(uiText(title));
}
const std::string& Window::title() const { return m_impl->title; }
Size Window::size() const { return m_impl->platformWindow->sizeInPoints(); }
void Window::close() { m_impl->closed = true; }
void Window::raise() { m_impl->platformWindow->raise(); }
void Window::onClosed(std::function<void()> handler) { m_impl->closedHandler = std::move(handler); }
void Window::onCloseRequested(std::function<bool()> handler) { m_impl->closeRequestHandler = std::move(handler); }

void Window::Impl::show() {
    platformWindow->show();
    if (restoreMaximized && !platformWindow->isMaximized()) platformWindow->toggleMaximize();
    restoreMaximized = false;
}

void Window::Impl::rememberFrame() {
    if (autosaveName.empty() || platformWindow->isFullscreen()) return;
    Settings& s = App::current().settings();
    const std::string key = "window." + autosaveName + ".";
    const bool maximized = platformWindow->isMaximized();
    s.set(key + "maximized", maximized);
    if (maximized) return;
    const Size size = platformWindow->sizeInPoints();
    s.set(key + "width", static_cast<int>(std::lround(size.width)));
    s.set(key + "height", static_cast<int>(std::lround(size.height)));
}
void Window::setSize(Size size) {
    if (m_impl->platformWindow->isMaximized() || m_impl->platformWindow->isFullscreen()) return;
    m_impl->platformWindow->setSize(static_cast<int>(std::lround(size.width)), static_cast<int>(std::lround(size.height)));
}

void Window::setMinimumSize(Size size) {
    m_impl->platformWindow->setMinimumSize(static_cast<int>(std::lround(size.width)), static_cast<int>(std::lround(size.height)));
}

void Window::setFullscreen(bool fullscreen) { m_impl->platformWindow->setFullscreen(fullscreen); }
bool Window::isFullscreen() const { return m_impl->platformWindow->isFullscreen(); }
void Window::setTitleVisible(bool visible) { m_impl->root->setTitleVisible(visible); }
void Window::setChromeHidden(bool hidden) { m_impl->root->setChromeHidden(hidden); }

// --- Impl ---------------------------------------------------------------------------------------

Window::Impl::Impl(const platform::WindowConfig& config)
    : platformWindow(std::make_unique<platform::PlatformWindow>(config)),
      root(std::make_shared<WindowRootView>()),
      title(config.title) {
    root->setHost(this);
    root->setTitle(title);
    root->onClose = [this] { requestClose(); };
    root->onMinimize = [this] { platformWindow->minimize(); };
    root->onZoom = [this] { platformWindow->toggleMaximize(); };
    dispatcher.setRoot(root);
    dispatcher.onOverlayDismissed = [](Widget* overlay) {
        if (auto* popup = dynamic_cast<PopupHost*>(overlay)) popup->popupDismissed();
    };
    platformWindow->setHitTest([this](Point p) { return hitRegion(p); });
}

Window::Impl::~Impl() {
    if (rhi::Device* device = rhi::device()) device->releaseWindow(*platformWindow);
    platformWindow->setHitTest(nullptr);
    root->setHost(nullptr);
}

void Window::Impl::themeChanged() {
    root->notifyThemeChanged();
    for (auto& o : dispatcher.overlays()) o->notifyThemeChanged();
    requestLayout();
}

double Window::Impl::nextDeadline() const {
    if (needsFrame) return 0.0;
    return wakeAt;
}

void Window::Impl::scheduleFrame(double t) {
    if (wakeAt < 0 || t < wakeAt) wakeAt = t;
}

void Window::Impl::setCursor(CursorShape shape) { platform::Platform::setCursor(shape); }

void Window::Impl::showOverlay(std::shared_ptr<Widget> overlay) {
    overlay->setHost(this);
    dispatcher.pushOverlay(std::move(overlay));
    requestLayout();
}

void Window::Impl::dismissOverlay(Widget* overlay) {
    dispatcher.removeOverlay(overlay);
    requestRedraw();
}

std::string Window::Impl::clipboardText() const { return platform::Platform::clipboardText(); }
void Window::Impl::setClipboardText(const std::string& text) { platform::Platform::setClipboardText(text); }
void Window::Impl::setTextInput(bool active, Rect caret) { platformWindow->setTextInputActive(active, caret); }

void Window::Impl::updateCursor() { platform::Platform::setCursor(dispatcher.cursor()); }

platform::HitRegion Window::Impl::hitRegion(Point p) {
    using platform::HitRegion;
    Size s = platformWindow->sizeInPoints();
    if (!platformWindow->isMaximized() && !platformWindow->isFullscreen()) {
        const float b = kResizeBorder;
        bool left = p.x < b;
        bool right = p.x >= s.width - b;
        bool top = p.y < b;
        bool bottom = p.y >= s.height - b;
        if (top && left) return HitRegion::ResizeTopLeft;
        if (top && right) return HitRegion::ResizeTopRight;
        if (bottom && left) return HitRegion::ResizeBottomLeft;
        if (bottom && right) return HitRegion::ResizeBottomRight;
        if (left) return HitRegion::ResizeLeft;
        if (right) return HitRegion::ResizeRight;
        if (top) return HitRegion::ResizeTop;
        if (bottom) return HitRegion::ResizeBottom;
    }
    if (!dispatcher.overlays().empty()) return HitRegion::Normal;
    for (Widget* w = dispatcher.hitTest(p); w; w = w->parent()) {
        if (w->isInteractive()) return HitRegion::Normal;
        if (w->isDragRegion()) return HitRegion::Draggable;
    }
    return HitRegion::Normal;
}

void Window::Impl::handleEvent(const platform::PlatformEvent& event) {
    using platform::EventType;
    auto hideTooltip = [&] {
        if (!tooltipWidget.expired()) requestRedraw();
        tooltipWidget.reset();
    };
    switch (event.type) {
        case EventType::WindowClose: requestClose(); break;
        case EventType::WindowResized:
        case EventType::WindowMaximized:
        case EventType::WindowRestored:
            rememberFrame();
            requestLayout();
            break;
        case EventType::WindowScaleChanged: requestLayout(); break;
        case EventType::WindowExposed: requestRedraw(); break;
        case EventType::WindowFocusGained:
            keyWindow = true;
            root->notifyThemeChanged();  // key-state colours (traffic lights, selection) change
            requestLayout();
            break;
        case EventType::WindowFocusLost:
            keyWindow = false;
            root->notifyThemeChanged();
            requestLayout();
            hideTooltip();
            break;
        case EventType::MouseMove: {
            pointer = event.position;
            PointerEvent e{event.position, MouseButton::None, 0, event.modifiers};
            dispatcher.pointerMove(e);
            Widget* h = dispatcher.hovered();
            // Find the nearest widget with a tooltip on the hover path.
            Widget* tip = nullptr;
            for (Widget* w = h; w; w = w->parent()) {
                if (!w->tooltip().empty()) {
                    tip = w;
                    break;
                }
            }
            if (tip != tooltipWidget.lock().get()) {
                hideTooltip();
                if (tip) {
                    tooltipWidget = tip->weak_from_this();
                    tooltipSince = platform::Platform::now();
                    scheduleFrame(tooltipSince + kTooltipDelay);
                }
            }
            updateCursor();
            break;
        }
        case EventType::MouseDown: {
            hideTooltip();
            tooltipWidget.reset();
            PointerEvent e{event.position, event.button, event.clicks, event.modifiers};
            dispatcher.pointerDown(e);
            updateCursor();
            break;
        }
        case EventType::MouseUp: {
            PointerEvent e{event.position, event.button, event.clicks, event.modifiers};
            dispatcher.pointerUp(e);
            updateCursor();
            break;
        }
        case EventType::DragEnded:
            // The compositor kept the button release; finish the press that started the drag ourselves.
            dispatcher.cancelPointer();
            needsFrame = true;
            break;
        case EventType::MouseLeave:
            dispatcher.pointerLeave();
            hideTooltip();
            break;
        case EventType::Scroll: {
            hideTooltip();
            ScrollEvent e;
            e.position = event.position;
            e.precise = event.precise;
            e.reportsEnd = event.scrollEnds;
            e.delta = event.delta * (event.precise ? kPreciseStep : kWheelStep);
            e.modifiers = event.modifiers;
            // Shift+wheel scrolls horizontally, as on macOS.
            if (!event.precise && hasModifier(event.modifiers, Modifiers::Shift) && e.delta.x == 0) {
                e.delta = {e.delta.y, 0};
            }
            dispatcher.scroll(e);
            updateCursor();
            break;
        }
        case EventType::ScrollEnd: {
            ScrollEvent e;
            e.position = pointer;
            e.precise = true;
            e.phase = ScrollPhase::Ended;
            e.reportsEnd = true;
            dispatcher.scroll(e);
            break;
        }
        case EventType::KeyDown: {
            hideTooltip();
            KeyEvent e{event.key, event.modifiers, event.repeat};
            // Ctrl+W closes the window only if nobody wanted the key (it deletes a word in a terminal).
            if (!dispatcher.keyDown(e) && e.primary() && !e.shift() && e.key == Key::W) requestClose();
            break;
        }
        case EventType::TextInput: dispatcher.textInput(TextInputEvent{event.text}); break;
        case EventType::Pinch:
            hideTooltip();
            dispatcher.magnify(MagnifyEvent{pointer, event.scale});
            break;
        case EventType::DropPosition:
            if (dropHandler) {
                // There's no "drag left" event, so the highlight times out unless refreshed.
                dropHoverUntil = platform::Platform::now() + 0.3;
                scheduleFrame(dropHoverUntil + 0.01);
                requestRedraw();
            }
            break;
        case EventType::DropFile:
            if (!event.text.empty()) pendingDrops.push_back(event.text);
            break;
        case EventType::DropComplete: {
            dropHoverUntil = -1;
            requestRedraw();
            auto files = std::move(pendingDrops);
            pendingDrops.clear();
            if (dropHandler && !files.empty()) dropHandler(files);
            break;
        }
        default: break;
    }
}

void Window::Impl::layout() {
    needsLayout = false;
    Size size = platformWindow->sizeInPoints();
    root->setWindowState(platformWindow->isMaximized(), keyWindow, platformWindow->isTransparent(),
                         platformWindow->isFullscreen());
    root->measure(size);
    root->arrange({0, 0, size.width, size.height});
    for (auto& overlay : dispatcher.overlays()) {
        // Popups take their ideal size (expanding content must not stretch them to the window).
        Size m = overlay->measure({kInfinity, kInfinity});
        if (m.width > size.width - 16 || m.height > size.height - 16)
            m = overlay->measure({std::min(m.width, size.width - 16), std::min(m.height, size.height - 16)});
        Rect placed{(size.width - m.width) * 0.5f, (size.height - m.height) * 0.5f, m.width, m.height};
        if (auto* popup = dynamic_cast<PopupHost*>(overlay.get())) placed = popup->placePopup(size, m);
        overlay->arrange(placed);
    }
    dispatcher.refreshHover();
}

void Window::Impl::drawTooltip(Painter& p) {
    auto w = tooltipWidget.lock();
    if (!w || w->tooltip().empty() || !w->host()) return;
    double elapsed = frameTime - tooltipSince;
    if (elapsed < kTooltipDelay) return;
    const Theme& t = theme();
    Font font = t.font(TextStyle::Subheadline);
    float tw = p.measureText(w->tooltip(), font);
    Size win = platformWindow->sizeInPoints();
    Rect r{pointer.x, pointer.y + 22, tw + 16, font.lineHeight + 8};
    r.x = std::clamp(r.x, 4.0f, std::max(4.0f, win.width - r.width - 4));
    if (r.bottom() > win.height - 4) r.y = pointer.y - r.height - 6;
    float fade = std::clamp(static_cast<float>((elapsed - kTooltipDelay) / 0.12), 0.0f, 1.0f);
    if (fade < 1.0f) requestRedraw();
    p.pushOpacity(fade);
    p.drawShadow(r, 6, t.colors.shadow.multipliedAlpha(0.6f), 10, {0, 3});
    p.drawMaterial(r, 6, Material::Popover);
    p.drawText(w->tooltip(), r.inset(8, 0), font, t.colors.label);
    p.popOpacity();
}

void Window::Impl::drawDropHighlight(Painter& p) {
    if (dropHoverUntil < 0 || frameTime > dropHoverUntil) return;
    const Theme& t = theme();
    Size s = platformWindow->sizeInPoints();
    Rect r = Rect{0, 0, s.width, s.height}.inset(4);
    float radius = std::max(4.0f, root->windowRadius() - 4);
    p.fillRoundedRect(r, radius, t.colors.accent.withAlpha(0.08f));
    p.strokeRoundedRect(r, radius, t.colors.accent, 3);
}

void Window::Impl::render() {
    needsFrame = false;
    wakeAt = -1;
    frameTime = platform::Platform::now();
    inFrame = true;
    if (needsLayout) layout();

    int w = 0;
    int h = 0;
    platformWindow->pixelSize(w, h);
    auto& app = App::current();
    const Theme& theme = app.theme();
    auto& r = *app.impl().renderer;
    Size size = platformWindow->sizeInPoints();
    const float scale = static_cast<float>(w) / std::max(1.0f, size.width);
    r.beginFrame(*platformWindow, w, h, scale);
    Painter painter(r, *app.impl().text);

    static const std::string testScene = std::getenv("MAYO_TEST_SCENE") ? std::getenv("MAYO_TEST_SCENE") : "";
    if (!testScene.empty()) {
        painter.pushClip({0, 0, size.width, size.height}, root->windowRadius());
        if (testScene == "text")
            drawTextTestScene(painter, size, theme);
        else
            drawRendererTestScene(r, size, theme);
        painter.popClip();
    } else {
        root->paint(painter);
        for (auto& overlay : dispatcher.overlays()) overlay->paint(painter);
        drawTooltip(painter);
        drawDropHighlight(painter);
        root->paintWindowFrame(painter);
    }
    r.endFrame();
    inFrame = false;
    // MAYO_VERBOSE=2: frames per second, to spot widgets that keep requesting frames.
    static const bool countFrames = [] {
        const char* v = std::getenv("MAYO_VERBOSE");
        return v && std::string(v) == "2";
    }();
    if (countFrames) {
        static int frames = 0;
        static double since = frameTime;
        static double busy = 0;  // CPU time spent building frames (layout + draw, not the swap)
        ++frames;
        busy += platform::Platform::now() - frameTime;
        if (frameTime - since >= 1.0) {
            std::fprintf(stderr, "mayonnaise: %d fps, %d draw calls, %.2f ms/frame\n", frames, r.drawCallsLastFrame(),
                         busy * 1000.0 / frames);
            frames = 0;
            busy = 0;
            since = frameTime;
        }
    }
    r.present(*platformWindow);
}

}  // namespace mayo
