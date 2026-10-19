#pragma once

// Internal platform abstraction. This is the only layer allowed to include SDL; everything above
// it talks in PlatformEvent / PlatformWindow so another backend can be dropped in later.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mayonnaise/events.hpp"
#include "mayonnaise/color.hpp"
#include "mayonnaise/geometry.hpp"
#include "mayonnaise/theme.hpp"

namespace mayo::platform {

using WindowId = std::uint32_t;

enum class GraphicsApi : std::uint8_t { OpenGL, Vulkan };

enum class HitRegion : std::uint8_t {
    Normal, Draggable,
    ResizeTop, ResizeBottom, ResizeLeft, ResizeRight,
    ResizeTopLeft, ResizeTopRight, ResizeBottomLeft, ResizeBottomRight,
};

struct WindowConfig {
    std::string title;
    int width = 800;   // points
    int height = 600;  // points
    int minWidth = 200;
    int minHeight = 120;
    bool resizable = true;
    bool transparent = true;
    bool dialog = false;
    float cornerRadius = -1.0f;
    bool backgroundBlur = false;
    float backgroundOpacity = 1.0f;
    bool compositorFrame = false;
};

enum class EventType : std::uint8_t {
    None, Quit,
    WindowClose, WindowResized, WindowScaleChanged, WindowExposed,
    WindowFocusGained, WindowFocusLost, WindowMaximized, WindowRestored, WindowMinimized,
    MouseMove, MouseDown, MouseUp, MouseLeave, Scroll,
    ScrollEnd,           // the fingers left the touchpad (only where the platform reports it)
    KeyDown, KeyUp, TextInput,
    DropFile, DropPosition, DropComplete,
    Pinch,
    Wake,
    SystemThemeChanged,  // desktop switched between light and dark
    DragEnded,           // a drag started with startFileDrag() was dropped or cancelled
};

struct PlatformEvent {
    EventType type = EventType::None;
    WindowId window = 0;
    Point position;            // window coordinates (points)
    Vec2 delta;                // scroll delta (points)
    bool precise = false;
    bool scrollEnds = false;   // a ScrollEnd event will follow this touchpad gesture
    MouseButton button = MouseButton::None;
    int clicks = 0;
    Key key = Key::Unknown;
    Modifiers modifiers = Modifiers::None;
    bool repeat = false;
    std::string text;      // text input, dropped file path
    float scale = 1;       // pinch: scale change since the previous update
};

struct DialogFilter {
    std::string name;
    std::string pattern;  // "png;jpg;jpeg"
};

class PlatformWindow {
public:
    explicit PlatformWindow(const WindowConfig& config);
    ~PlatformWindow();
    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;

    [[nodiscard]] WindowId id() const;
    [[nodiscard]] Size sizeInPoints() const;
    /// Framebuffer size in physical pixels.
    void pixelSize(int& w, int& h) const;
    /// Physical pixels per point.
    [[nodiscard]] float scale() const;
    [[nodiscard]] bool isMaximized() const;
    [[nodiscard]] bool isFullscreen() const;
    [[nodiscard]] bool isTransparent() const { return m_transparent; }

    [[nodiscard]] bool hasBackgroundBlur() const;
    void setTitle(const std::string& title);
    void show();
    void hide();
    void minimize();
    void raise();
    void toggleMaximize();
    void setFullscreen(bool fullscreen);
    void setMinimumSize(int w, int h);
    void setSize(int w, int h);  // points

    /// Visible on screen (not hidden, not minimized): presenting to a hidden Wayland surface is an error.
    [[nodiscard]] bool isVisible() const;

    // OpenGL windows.
    void makeCurrent();
    void swapBuffers();
    // Vulkan windows: a VkSurfaceKHR for `instance` (VkInstance), or 0 on failure. The caller destroys it.
    [[nodiscard]] std::uint64_t createVulkanSurface(void* instance);

    void setHitTest(std::function<HitRegion(Point)> callback);
    void setTextInputActive(bool active, Rect caretRect = {});
    void setSecureTextInput(bool secure);
    /// Drags files out to other apps (Wayland). Must be called while the pointer button that started the drag
    /// is still down. A DragEnded event follows when it's over. False when the platform can't.
    bool startFileDrag(const std::vector<std::string>& paths);

    /// Converts window coordinates reported by SDL into points.
    [[nodiscard]] float pointsPerWindowUnit() const;

    struct Impl;
    [[nodiscard]] Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
    bool m_transparent = false;
};

/// Process-wide platform services. Must be initialized before creating windows.
class Platform {
public:
    /// `appId` names the .desktop file (Wayland app_id / X11 WM_CLASS) so docks show the right icon.
    static bool init(const std::string& appId = {});
    static void shutdown();

    /// Blocks up to `timeoutSeconds` (negative = forever) for the next event.
    static bool waitEvent(PlatformEvent& out, double timeoutSeconds);
    static bool pollEvent(PlatformEvent& out);
    /// Wakes up a blocking waitEvent from any thread.
    static void wake();

    static double now();  // seconds, monotonic

    static void setCursor(CursorShape shape);
    static std::string clipboardText();
    static void setClipboardText(const std::string& text);
    static void setActivationToken(const std::string& token);
    static float doubleClickTime();  // seconds
    /// The desktop's light/dark preference, if it has one (xdg-desktop-portal on Linux, via SDL).
    static std::optional<Appearance> systemAppearance();
    /// The desktop's accent colour, if it publishes one (portal `org.freedesktop.appearance accent-color`).
    static std::optional<Color> systemAccentColor();

    static PlatformWindow* windowFromId(WindowId id);
    /// Native open dialog (xdg portal / zenity). `done` may run on another thread; an empty list means
    /// the user cancelled.
    static void showOpenDialog(PlatformWindow* parent, const std::vector<DialogFilter>& filters, bool folders,
                               bool multiple, std::function<void(std::vector<std::string>)> done);
    /// Which API windows are created for; set before the first window. OpenGL by default.
    static void setGraphicsApi(GraphicsApi api);
    [[nodiscard]] static GraphicsApi graphicsApi();
    /// Loads libvulkan through SDL. False when there is none.
    static bool loadVulkan();
    /// PFN_vkGetInstanceProcAddr, once loadVulkan() succeeded.
    static void* vulkanGetInstanceProcAddr();
    /// Instance extensions windows need for surfaces.
    static std::vector<const char*> vulkanInstanceExtensions();
    static bool vulkanPresentationSupport(void* instance, void* physicalDevice, std::uint32_t queueFamily);
    /// While Mesa drives every screen, hides the NVIDIA Vulkan driver from the loader for this process (the
    /// same reasoning as the EGL vendor restriction). Undo it once the physical devices are enumerated, so
    /// programs we launch don't inherit it.
    static void restrictVulkanDrivers();
    static void endVulkanDriverRestriction();

    /// EGL display of the shared GL context, or nullptr when the context isn't EGL (GLX).
    static void* eglDisplay();
    static void* eglProcAddress(const char* name);
    /// Valid once the first window (and with it the shared GL context) exists.
    static void* glProcAddress(const char* name);
};

}  // namespace mayo::platform
