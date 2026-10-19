#include "wayland_material.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#include "platform.hpp"
#include "wayland_drag.hpp"

namespace mayo::platform {

namespace {

SDL_GLContext g_context = nullptr;
GraphicsApi g_api = GraphicsApi::OpenGL;
bool g_vulkanLoaded = false;
std::unordered_map<WindowId, PlatformWindow*> g_windows;
std::array<SDL_Cursor*, 10> g_cursors{};
CursorShape g_currentCursor = CursorShape::Arrow;
Uint32 g_wakeEvent = 0;
Uint32 g_dragEndEvent = 0;
Uint32 g_scrollEndEvent = 0;

/// Turns a finger lift seen on our Wayland queue into an SDL event, queued behind the wheel events SDL
/// already produced from the same read, so the widget sees the gesture's last movement first.
void forwardScrollStop() {
    if (!wayland::takeScrollStop()) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_scrollEndEvent;
    SDL_Window* focus = SDL_GetMouseFocus();
    e.user.windowID = focus ? SDL_GetWindowID(focus) : 0;
    SDL_PushEvent(&e);
}

void* waylandDisplay() {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::strcmp(driver, "wayland") != 0) return nullptr;
    return SDL_GetPointerProperty(SDL_GetGlobalProperties(), SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, nullptr);
}

#ifdef __linux__
/// Name of the kernel driver bound to a DRM card ("amdgpu", "i915", "nvidia").
std::string drmDriver(const std::filesystem::path& card) {
    std::error_code ec;
    auto target = std::filesystem::read_symlink(card / "device" / "driver", ec);
    return ec ? std::string() : target.filename().string();
}

/// True on hybrid laptops where an NVIDIA driver is installed but Mesa drives every connected screen. Loading
/// the NVIDIA stack there is pure overhead (~90 MB of mapped libraries, ~6 MB private, and it can wake the dGPU).
/// Only on Wayland, where the compositor (not an NVIDIA-primary X server) owns the display. PRIME offload
/// (`prime-run`), explicit loader variables and MAYO_EGL_ALL_VENDORS=1 opt out.
bool mesaDrivesDisplays(std::initializer_list<const char*> optOuts) {
    namespace fs = std::filesystem;
    for (const char* var : {"__NV_PRIME_RENDER_OFFLOAD", "MAYO_EGL_ALL_VENDORS", "DRI_PRIME"}) {
        if (std::getenv(var)) return false;
    }
    for (const char* var : optOuts) {
        if (std::getenv(var)) return false;
    }
    const char* driver = std::getenv("SDL_VIDEO_DRIVER");
    if (!std::getenv("WAYLAND_DISPLAY") || (driver && std::strcmp(driver, "wayland") != 0)) return false;

    // Connected connectors are named card<N>-<port>; the dGPU often has none (or is suspended and absent).
    std::error_code ec;
    bool otherConnected = false;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename().string();
        const auto dash = name.find('-');
        if (name.rfind("card", 0) != 0 || dash == std::string::npos) continue;
        std::ifstream statusFile(entry.path() / "status");
        std::string status;
        if (!(statusFile >> status) || status != "connected") continue;
        if (drmDriver(entry.path().parent_path() / name.substr(0, dash)) == "nvidia") return false;
        otherConnected = true;
    }
    return otherConnected;
}

/// Restricts glvnd's libEGL to Mesa for this process when Mesa drives the display (see mesaDrivesDisplays).
/// Returns true if applied.
bool preferMesaEgl() {
    namespace fs = std::filesystem;
    if (!mesaDrivesDisplays({"__EGL_VENDOR_LIBRARY_FILENAMES", "__EGL_VENDOR_LIBRARY_DIRS", "MAYO_GLX"})) return false;
    std::error_code ec;
    fs::path mesa;
    bool nvidiaVendor = false;
    for (const char* dir : {"/etc/glvnd/egl_vendor.d", "/usr/share/glvnd/egl_vendor.d"}) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.find("nvidia") != std::string::npos) nvidiaVendor = true;
            if (name.find("mesa") != std::string::npos && mesa.empty()) mesa = entry.path();
        }
    }
    if (!nvidiaVendor || mesa.empty()) return false;

    setenv("__EGL_VENDOR_LIBRARY_FILENAMES", mesa.c_str(), 1);
    if (const char* v = std::getenv("MAYO_VERBOSE"); v && *v && *v != '0')
        SDL_Log("mayonnaise: Mesa drives the display; EGL restricted to %s", mesa.c_str());
    return true;
}
bool g_restrictedEgl = false;
bool g_restrictedVulkan = false;
#endif

float envScaleOverride() {
    if (const char* s = std::getenv("MAYO_SCALE")) {
        float v = std::strtof(s, nullptr);
        if (v > 0.25f && v < 8.0f) return v;
    }
    return 0.0f;
}

Modifiers translateMods(SDL_Keymod m) {
    Modifiers out = Modifiers::None;
    if (m & SDL_KMOD_SHIFT) out = out | Modifiers::Shift;
    if (m & SDL_KMOD_CTRL) out = out | Modifiers::Ctrl;
    if (m & SDL_KMOD_ALT) out = out | Modifiers::Alt;
    if (m & SDL_KMOD_GUI) out = out | Modifiers::Super;
    return out;
}

Key translateKey(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9)
        return static_cast<Key>(static_cast<int>(Key::Num0) + static_cast<int>(k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(k - SDLK_F1));
    switch (k) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return Key::Enter;
        case SDLK_ESCAPE: return Key::Escape;
        case SDLK_BACKSPACE: return Key::Backspace;
        case SDLK_TAB: return Key::Tab;
        case SDLK_SPACE: return Key::Space;
        case SDLK_DELETE: return Key::Delete;
        case SDLK_INSERT: return Key::Insert;
        case SDLK_LEFT: return Key::Left;
        case SDLK_RIGHT: return Key::Right;
        case SDLK_UP: return Key::Up;
        case SDLK_DOWN: return Key::Down;
        case SDLK_HOME: return Key::Home;
        case SDLK_END: return Key::End;
        case SDLK_PAGEUP: return Key::PageUp;
        case SDLK_PAGEDOWN: return Key::PageDown;
        case SDLK_COMMA: return Key::Comma;
        case SDLK_PERIOD: return Key::Period;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: return Key::Minus;
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: return Key::Equals;
        case SDLK_SLASH: return Key::Slash;
        default: return Key::Unknown;
    }
}

MouseButton translateButton(Uint8 b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return MouseButton::Left;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        case SDL_BUTTON_RIGHT: return MouseButton::Right;
        default: return MouseButton::None;
    }
}

SDL_HitTestResult toSdl(HitRegion r) {
    switch (r) {
        case HitRegion::Normal: return SDL_HITTEST_NORMAL;
        case HitRegion::Draggable: return SDL_HITTEST_DRAGGABLE;
        case HitRegion::ResizeTop: return SDL_HITTEST_RESIZE_TOP;
        case HitRegion::ResizeBottom: return SDL_HITTEST_RESIZE_BOTTOM;
        case HitRegion::ResizeLeft: return SDL_HITTEST_RESIZE_LEFT;
        case HitRegion::ResizeRight: return SDL_HITTEST_RESIZE_RIGHT;
        case HitRegion::ResizeTopLeft: return SDL_HITTEST_RESIZE_TOPLEFT;
        case HitRegion::ResizeTopRight: return SDL_HITTEST_RESIZE_TOPRIGHT;
        case HitRegion::ResizeBottomLeft: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        case HitRegion::ResizeBottomRight: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    }
    return SDL_HITTEST_NORMAL;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
struct PlatformWindow::Impl {
    SDL_Window* window = nullptr;
    std::unique_ptr<WaylandMaterial> material;
    float materialRadius = 0;
    std::function<HitRegion(Point)> hitTest;
    float scaleOverride = 0.0f;
    bool textInput = false;
    bool secureTextInput = false;
    bool startTextInput() {
        if (!secureTextInput) return SDL_StartTextInput(window);
        const SDL_PropertiesID props = SDL_CreateProperties();
        // If properties cannot be allocated, do not start a normal input session for a password.
        if (!props) return false;
        const bool configured =
            SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN) &&
            SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_NONE) &&
            SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
        const bool started = configured && SDL_StartTextInputWithProperties(window, props);
        SDL_DestroyProperties(props);
        return started;
    }

    static SDL_HitTestResult hitTestThunk(SDL_Window* /*win*/, const SDL_Point* area, void* data) {
        auto* self = static_cast<PlatformWindow*>(data);
        auto& impl = *self->m_impl;
        if (!impl.hitTest) return SDL_HITTEST_NORMAL;
        float k = self->pointsPerWindowUnit();
        return toSdl(impl.hitTest(Point{static_cast<float>(area->x) * k, static_cast<float>(area->y) * k}));
    }
};

PlatformWindow::PlatformWindow(const WindowConfig& config) : m_impl(std::make_unique<Impl>()) {
    m_impl->scaleOverride = envScaleOverride();
    const char* noTransparency = std::getenv("MAYO_NO_TRANSPARENCY");
    m_transparent = config.transparent && !(noTransparency && *noTransparency && *noTransparency != '0');

    const bool gl = g_api == GraphicsApi::OpenGL;
    if (gl) SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, m_transparent ? 8 : 0);
    SDL_WindowFlags flags = (gl ? SDL_WINDOW_OPENGL : SDL_WINDOW_VULKAN) | SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (config.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (m_transparent) flags |= SDL_WINDOW_TRANSPARENT;

    m_impl->window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!m_impl->window && m_transparent) {
        // Some X11 setups have no ARGB visual; fall back to an opaque window.
        m_transparent = false;
        if (gl) SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        flags &= ~SDL_WINDOW_TRANSPARENT;
        m_impl->window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    }
    if (!m_impl->window) {
        SDL_Log("mayonnaise: SDL_CreateWindow failed: %s", SDL_GetError());
        std::abort();
    }

    // Requested size is in points; SDL sizes are in window units, which differ on X11 with scaling.
    float k = pointsPerWindowUnit();
    if (std::abs(k - 1.0f) > 0.01f) {
        SDL_SetWindowSize(m_impl->window, static_cast<int>(std::lround(config.width / k)),
                          static_cast<int>(std::lround(config.height / k)));
    }
    setMinimumSize(config.minWidth, config.minHeight);
    g_windows[id()] = this;
    if (config.backgroundBlur && m_transparent) {
        auto props = SDL_GetWindowProperties(m_impl->window);
        auto* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        auto* surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        m_impl->materialRadius = config.cornerRadius >= 0 ? config.cornerRadius : 12.0f;
        m_impl->material = std::make_unique<WaylandMaterial>(display, surface,
            m_impl->materialRadius / pointsPerWindowUnit(), config.compositorFrame);
        int w = 0, h = 0;
        SDL_GetWindowSize(m_impl->window, &w, &h);
        m_impl->material->update(w, h, m_impl->materialRadius / pointsPerWindowUnit());
    }
    if (!gl) return;

    if (!g_context) {
        g_context = SDL_GL_CreateContext(m_impl->window);
        if (!g_context) {
            SDL_Log("mayonnaise: could not create an OpenGL 3.3 core context: %s", SDL_GetError());
            std::abort();
        }
        SDL_GL_MakeCurrent(m_impl->window, g_context);
#ifdef __linux__
        // glvnd has picked its vendors; don't pass the restriction on to programs we launch.
        if (g_restrictedEgl) {
            unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
            using GetString = const unsigned char* (*)(unsigned);
            auto getString = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"));
            const auto* renderer = getString ? reinterpret_cast<const char*>(getString(0x1F01 /* GL_RENDERER */)) : nullptr;
            if (renderer && std::strstr(renderer, "llvmpipe"))
                SDL_Log("mayonnaise: software rendering with Mesa-only EGL; set MAYO_EGL_ALL_VENDORS=1 to load all vendors");
        }
#endif
    } else {
        SDL_GL_MakeCurrent(m_impl->window, g_context);
    }
    SDL_GL_SetSwapInterval(1);
}

PlatformWindow::~PlatformWindow() {
    g_windows.erase(id());
    if (m_impl->textInput) SDL_StopTextInput(m_impl->window);
    m_impl->material.reset();
    SDL_DestroyWindow(m_impl->window);
}

WindowId PlatformWindow::id() const { return SDL_GetWindowID(m_impl->window); }

float PlatformWindow::scale() const {
    if (m_impl->scaleOverride > 0) return m_impl->scaleOverride;
    float s = SDL_GetWindowDisplayScale(m_impl->window);
    return s > 0 ? s : 1.0f;
}

float PlatformWindow::pointsPerWindowUnit() const {
    float density = SDL_GetWindowPixelDensity(m_impl->window);
    if (density <= 0) density = 1.0f;
    return density / scale();
}

Size PlatformWindow::sizeInPoints() const {
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(m_impl->window, &w, &h);
    float k = pointsPerWindowUnit();
    return {static_cast<float>(w) * k, static_cast<float>(h) * k};
}

bool PlatformWindow::hasBackgroundBlur() const {
    return m_impl->material && m_impl->material->available();
}
void PlatformWindow::pixelSize(int& w, int& h) const {
    if (m_impl->material) {
        int sw = 0, sh = 0;
        SDL_GetWindowSize(m_impl->window, &sw, &sh);
        const float radius = isMaximized() || isFullscreen() ? 0 : m_impl->materialRadius / pointsPerWindowUnit();
        m_impl->material->update(sw, sh, radius);
    }
    SDL_GetWindowSizeInPixels(m_impl->window, &w, &h);
}

bool PlatformWindow::isMaximized() const { return (SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_MAXIMIZED) != 0; }
bool PlatformWindow::isFullscreen() const {
    return (SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void PlatformWindow::setTitle(const std::string& title) { SDL_SetWindowTitle(m_impl->window, title.c_str()); }
void PlatformWindow::show() { SDL_ShowWindow(m_impl->window); }
void PlatformWindow::hide() { SDL_HideWindow(m_impl->window); }
void PlatformWindow::minimize() { SDL_MinimizeWindow(m_impl->window); }
void PlatformWindow::raise() { SDL_RaiseWindow(m_impl->window); }
void PlatformWindow::toggleMaximize() {
    if (isMaximized())
        SDL_RestoreWindow(m_impl->window);
    else
        SDL_MaximizeWindow(m_impl->window);
}
void PlatformWindow::setFullscreen(bool fullscreen) { SDL_SetWindowFullscreen(m_impl->window, fullscreen); }

void PlatformWindow::setSize(int w, int h) {
    const float k = pointsPerWindowUnit();
    SDL_SetWindowSize(m_impl->window, static_cast<int>(std::lround(static_cast<float>(w) / k)),
                      static_cast<int>(std::lround(static_cast<float>(h) / k)));
}

void PlatformWindow::setMinimumSize(int w, int h) {
    float k = pointsPerWindowUnit();
    SDL_SetWindowMinimumSize(m_impl->window, static_cast<int>(static_cast<float>(w) / k),
                             static_cast<int>(static_cast<float>(h) / k));
}

bool PlatformWindow::isVisible() const {
    return (SDL_GetWindowFlags(m_impl->window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}

std::uint64_t PlatformWindow::createVulkanSurface(void* instance) {
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(m_impl->window, static_cast<VkInstance>(instance), nullptr, &surface)) {
        SDL_Log("mayonnaise: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(surface);
}

void PlatformWindow::makeCurrent() { SDL_GL_MakeCurrent(m_impl->window, g_context); }
void PlatformWindow::swapBuffers() { SDL_GL_SwapWindow(m_impl->window); }

void PlatformWindow::setHitTest(std::function<HitRegion(Point)> callback) {
    m_impl->hitTest = std::move(callback);
    SDL_SetWindowHitTest(m_impl->window, m_impl->hitTest ? &Impl::hitTestThunk : nullptr, this);
}

void PlatformWindow::setSecureTextInput(bool secure) {
    if (m_impl->secureTextInput == secure) return;
    m_impl->secureTextInput = secure;
    if (m_impl->textInput) {
        SDL_StopTextInput(m_impl->window);
        m_impl->textInput = m_impl->startTextInput();
    }
}

void PlatformWindow::setTextInputActive(bool active, Rect caretRect) {
    if (active) {
        float k = pointsPerWindowUnit();
        SDL_Rect r{static_cast<int>(caretRect.x / k), static_cast<int>(caretRect.y / k),
                   static_cast<int>(caretRect.width / k), static_cast<int>(caretRect.height / k)};
        SDL_SetTextInputArea(m_impl->window, &r, 0);
        if (!m_impl->textInput) m_impl->textInput = m_impl->startTextInput();
    } else if (m_impl->textInput) {
        SDL_StopTextInput(m_impl->window);
        m_impl->textInput = false;
    }
}

bool PlatformWindow::startFileDrag(const std::vector<std::string>& paths) {
    void* display = waylandDisplay();
    void* surface = SDL_GetPointerProperty(SDL_GetWindowProperties(m_impl->window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    const SDL_WindowID id = SDL_GetWindowID(m_impl->window);
    return wayland::startFileDrag(display, surface, paths, [id] {
        SDL_Event e;
        SDL_zero(e);
        e.type = g_dragEndEvent;
        e.user.windowID = id;
        SDL_PushEvent(&e);
    });
}

// ---------------------------------------------------------------------------------------------
bool Platform::init(const std::string& appId) {
    if (!appId.empty()) SDL_SetHint(SDL_HINT_APP_ID, appId.c_str());
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    // EGL on X11 too (Wayland always uses it): zero-copy video needs EGL dma-buf import.
    if (!std::getenv("MAYO_GLX")) SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
#ifdef __linux__
    g_restrictedEgl = preferMesaEgl();
#endif
    // Without this, the first click on an unfocused window is swallowed — macOS delivers it.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("mayonnaise: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    g_wakeEvent = SDL_RegisterEvents(1);
    g_dragEndEvent = SDL_RegisterEvents(1);
    g_scrollEndEvent = SDL_RegisterEvents(1);
    if (void* display = waylandDisplay()) wayland::prepare(display);
    return true;
}

void Platform::shutdown() {
    for (auto*& c : g_cursors) {
        if (c) SDL_DestroyCursor(c);
        c = nullptr;
    }
    if (g_context) {
        SDL_GL_DestroyContext(g_context);
        g_context = nullptr;
    }
    if (g_vulkanLoaded) {
        SDL_Vulkan_UnloadLibrary();
        g_vulkanLoaded = false;
    }
    SDL_Quit();
}

namespace {

bool translate(const SDL_Event& e, PlatformEvent& out) {
    out = PlatformEvent{};
    auto toPoints = [](SDL_WindowID wid, float x, float y) {
        auto it = g_windows.find(wid);
        float k = it != g_windows.end() ? it->second->pointsPerWindowUnit() : 1.0f;
        return Point{x * k, y * k};
    };
    switch (e.type) {
        case SDL_EVENT_QUIT: out.type = EventType::Quit; return true;
        case SDL_EVENT_SYSTEM_THEME_CHANGED: out.type = EventType::SystemThemeChanged; return true;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: out.type = EventType::WindowClose; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: out.type = EventType::WindowResized; break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: out.type = EventType::WindowScaleChanged; break;
        case SDL_EVENT_WINDOW_EXPOSED: out.type = EventType::WindowExposed; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: out.type = EventType::WindowFocusGained; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: out.type = EventType::WindowFocusLost; break;
        case SDL_EVENT_WINDOW_MAXIMIZED: out.type = EventType::WindowMaximized; break;
        case SDL_EVENT_WINDOW_RESTORED: out.type = EventType::WindowRestored; break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: out.type = EventType::WindowResized; break;
        case SDL_EVENT_WINDOW_MINIMIZED: out.type = EventType::WindowMinimized; break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE: out.type = EventType::MouseLeave; break;
        case SDL_EVENT_MOUSE_MOTION:
            out.type = EventType::MouseMove;
            out.window = e.motion.windowID;
            out.position = toPoints(e.motion.windowID, e.motion.x, e.motion.y);
            out.modifiers = translateMods(SDL_GetModState());
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            out.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? EventType::MouseDown : EventType::MouseUp;
            out.window = e.button.windowID;
            out.position = toPoints(e.button.windowID, e.button.x, e.button.y);
            out.button = translateButton(e.button.button);
            out.clicks = e.button.clicks;
            out.modifiers = translateMods(SDL_GetModState());
            return true;
        case SDL_EVENT_MOUSE_WHEEL: {
            out.type = EventType::Scroll;
            out.window = e.wheel.windowID;
            out.position = toPoints(e.wheel.windowID, e.wheel.mouse_x, e.wheel.mouse_y);
            float dx = e.wheel.x;
            float dy = e.wheel.y;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                dx = -dx;
                dy = -dy;
            }
            // Touchpads report arbitrary fractional deltas; wheels report notches, and high-resolution wheels
            // fractions of a notch in 1/120 steps. A single touchpad event can still land on a whole value
            // (1.0, or 0 on the idle axis), so once a gesture looks like a touchpad, events that follow it
            // closely stay "precise" — otherwise one event would turn into a 40 pt wheel glide mid-gesture.
            // On Wayland the compositor says which device it was (and when the fingers lift), which beats
            // guessing: a slow touchpad swipe produces whole values often enough to be taken for a wheel.
            if (const int source = wayland::scrollSource(); source >= 0) {
                out.precise = source == 1 || source == 2;  // finger, continuous
                out.scrollEnds = out.precise;
            } else {
                static Uint64 lastPrecise = 0;
                auto notchLike = [](float v) {
                    const float scaled = v * 120.0f;
                    return std::abs(scaled - std::round(scaled)) < 1e-3f;
                };
                const bool fractional = !notchLike(dx) || !notchLike(dy);
                if (fractional) lastPrecise = e.wheel.timestamp;
                constexpr Uint64 kStickyNs = 250'000'000;
                out.precise = fractional || (lastPrecise != 0 && e.wheel.timestamp - lastPrecise < kStickyNs);
            }
            out.delta = Vec2{dx, -dy};
            out.modifiers = translateMods(SDL_GetModState());
            return true;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            out.type = e.type == SDL_EVENT_KEY_DOWN ? EventType::KeyDown : EventType::KeyUp;
            out.window = e.key.windowID;
            out.key = translateKey(e.key.key);
            out.modifiers = translateMods(e.key.mod);
            out.repeat = e.key.repeat;
            return true;
        case SDL_EVENT_DROP_FILE:
        case SDL_EVENT_DROP_POSITION:
        case SDL_EVENT_DROP_COMPLETE:
            out.type = e.type == SDL_EVENT_DROP_FILE       ? EventType::DropFile
                       : e.type == SDL_EVENT_DROP_POSITION ? EventType::DropPosition
                                                           : EventType::DropComplete;
            out.window = e.drop.windowID;
            out.position = toPoints(e.drop.windowID, e.drop.x, e.drop.y);
            out.text = e.drop.data ? e.drop.data : "";
            return true;
        case SDL_EVENT_PINCH_UPDATE:
            out.type = EventType::Pinch;
            out.window = e.pinch.windowID;
            out.scale = e.pinch.scale;
            return true;
        case SDL_EVENT_TEXT_INPUT:
            out.type = EventType::TextInput;
            out.window = e.text.windowID;
            out.text = e.text.text ? e.text.text : "";
            return true;
        default:
            if (e.type == g_wakeEvent) {
                out.type = EventType::Wake;
                return true;
            }
            if (e.type == g_scrollEndEvent) {
                out.type = EventType::ScrollEnd;
                out.window = e.user.windowID;
                return true;
            }
            if (e.type == g_dragEndEvent) {
                out.type = EventType::DragEnded;
                out.window = e.user.windowID;
                return true;
            }
            return false;
    }
    out.window = e.window.windowID;
    return true;
}

}  // namespace

bool Platform::waitEvent(PlatformEvent& out, double timeoutSeconds) {
    SDL_Event e;
    Sint32 ms = timeoutSeconds < 0 ? -1 : static_cast<Sint32>(std::ceil(timeoutSeconds * 1000.0));
    // During an outgoing drag the drop target asks for the data over our own Wayland queue, which SDL's wait
    // doesn't report: wake up often enough to answer.
    if (wayland::dragActive() && (ms < 0 || ms > 15)) ms = 15;
    if (wayland::scrollGestureActive() && (ms < 0 || ms > 8)) ms = 8;  // notice the fingers lifting
    while (SDL_WaitEventTimeout(&e, ms)) {
        wayland::dispatch();
        forwardScrollStop();
        if (translate(e, out)) return true;
        ms = 0;  // swallowed an untranslated event; don't block again past the deadline
    }
    wayland::dispatch();
    forwardScrollStop();
    return false;
}

bool Platform::pollEvent(PlatformEvent& out) {
    wayland::dispatch();
    forwardScrollStop();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (translate(e, out)) return true;
    }
    return false;
}

void Platform::wake() {
    SDL_Event e;
    SDL_zero(e);
    e.type = g_wakeEvent;
    SDL_PushEvent(&e);
}

double Platform::now() { return static_cast<double>(SDL_GetTicksNS()) / 1e9; }

void Platform::setCursor(CursorShape shape) {
    if (shape == g_currentCursor && g_cursors[static_cast<size_t>(shape)]) return;
    g_currentCursor = shape;
    auto& c = g_cursors[static_cast<size_t>(shape)];
    if (!c) {
        SDL_SystemCursor sys = SDL_SYSTEM_CURSOR_DEFAULT;
        switch (shape) {
            case CursorShape::Arrow: sys = SDL_SYSTEM_CURSOR_DEFAULT; break;
            case CursorShape::IBeam: sys = SDL_SYSTEM_CURSOR_TEXT; break;
            case CursorShape::PointingHand: sys = SDL_SYSTEM_CURSOR_POINTER; break;
            case CursorShape::ResizeHorizontal: sys = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
            case CursorShape::ResizeVertical: sys = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
            case CursorShape::NotAllowed: sys = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
            case CursorShape::Crosshair: sys = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
            case CursorShape::Move: sys = SDL_SYSTEM_CURSOR_MOVE; break;
            case CursorShape::ResizeNWSE: sys = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
            case CursorShape::ResizeNESW: sys = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
        }
        c = SDL_CreateSystemCursor(sys);
    }
    if (c) SDL_SetCursor(c);
}

std::string Platform::clipboardText() {
    char* text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
}

void Platform::setClipboardText(const std::string& text) { SDL_SetClipboardText(text.c_str()); }
void Platform::setActivationToken(const std::string& token) {
    if (!token.empty()) SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "XDG_ACTIVATION_TOKEN", token.c_str(), true);
}

float Platform::doubleClickTime() { return 0.5f; }

namespace {
struct DialogRequest {
    std::vector<std::string> names;
    std::vector<std::string> patterns;
    std::vector<SDL_DialogFileFilter> filters;  // points into names/patterns; must outlive the dialog
    std::function<void(std::vector<std::string>)> done;
};

void SDLCALL dialogCallback(void* userdata, const char* const* files, int /*filter*/) {
    std::unique_ptr<DialogRequest> req(static_cast<DialogRequest*>(userdata));
    std::vector<std::string> out;
    if (!files) SDL_Log("mayonnaise: file dialog failed: %s", SDL_GetError());
    for (const char* const* f = files; f && *f; ++f) out.emplace_back(*f);
    if (req->done) req->done(std::move(out));
}
}  // namespace

void Platform::showOpenDialog(PlatformWindow* parent, const std::vector<DialogFilter>& filters, bool folders,
                              bool multiple, std::function<void(std::vector<std::string>)> done) {
    auto req = std::make_unique<DialogRequest>();
    req->done = std::move(done);
    for (const auto& f : filters) {
        req->names.push_back(f.name);
        req->patterns.push_back(f.pattern);
    }
    for (size_t i = 0; i < filters.size(); ++i) req->filters.push_back({req->names[i].c_str(), req->patterns[i].c_str()});
    SDL_Window* win = parent ? parent->impl().window : nullptr;
    DialogRequest* raw = req.release();  // owned by the callback from here on
    if (folders)
        SDL_ShowOpenFolderDialog(dialogCallback, raw, win, nullptr, multiple);
    else
        SDL_ShowOpenFileDialog(dialogCallback, raw, win, raw->filters.data(), static_cast<int>(raw->filters.size()),
                               nullptr, multiple);
}

PlatformWindow* Platform::windowFromId(WindowId id) {
    auto it = g_windows.find(id);
    return it != g_windows.end() ? it->second : nullptr;
}

std::optional<Appearance> Platform::systemAppearance() {
    switch (SDL_GetSystemTheme()) {
        case SDL_SYSTEM_THEME_DARK: return Appearance::Dark;
        case SDL_SYSTEM_THEME_LIGHT: return Appearance::Light;
        default: return std::nullopt;
    }
}

void Platform::setGraphicsApi(GraphicsApi api) {
    g_api = api;
#ifdef __linux__
    // No GL context will be created to consume the EGL restriction; don't pass it on to launched programs.
    if (api == GraphicsApi::Vulkan && g_restrictedEgl) {
        unsetenv("__EGL_VENDOR_LIBRARY_FILENAMES");
        g_restrictedEgl = false;
    }
#endif
}

GraphicsApi Platform::graphicsApi() { return g_api; }

bool Platform::loadVulkan() {
    if (!g_vulkanLoaded) g_vulkanLoaded = SDL_Vulkan_LoadLibrary(nullptr);
    return g_vulkanLoaded;
}

void* Platform::vulkanGetInstanceProcAddr() { return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr()); }

std::vector<const char*> Platform::vulkanInstanceExtensions() {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) return {};
    return {names, names + count};
}

bool Platform::vulkanPresentationSupport(void* instance, void* physicalDevice, std::uint32_t queueFamily) {
    return SDL_Vulkan_GetPresentationSupport(static_cast<VkInstance>(instance),
                                             static_cast<VkPhysicalDevice>(physicalDevice), queueFamily);
}

void Platform::restrictVulkanDrivers() {
#ifdef __linux__
    namespace fs = std::filesystem;
    if (g_restrictedVulkan ||
        !mesaDrivesDisplays({"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_DRIVERS_SELECT",
                             "VK_LOADER_DRIVERS_DISABLE", "MESA_VK_DEVICE_SELECT"}))
        return;
    std::error_code ec;
    bool nvidia = false;
    for (const char* dir : {"/etc/vulkan/icd.d", "/usr/share/vulkan/icd.d"}) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().filename().string().find("nvidia") != std::string::npos) nvidia = true;
        }
    }
    if (!nvidia) return;
    setenv("VK_LOADER_DRIVERS_DISABLE", "*nvidia*", 1);
    g_restrictedVulkan = true;
    if (const char* v = std::getenv("MAYO_VERBOSE"); v && *v && *v != '0')
        SDL_Log("mayonnaise: Mesa drives the display; NVIDIA Vulkan driver hidden");
#endif
}

void Platform::endVulkanDriverRestriction() {
#ifdef __linux__
    if (!g_restrictedVulkan) return;
    unsetenv("VK_LOADER_DRIVERS_DISABLE");
    g_restrictedVulkan = false;
#endif
}

void* Platform::eglDisplay() { return SDL_EGL_GetCurrentDisplay(); }

void* Platform::eglProcAddress(const char* name) { return reinterpret_cast<void*>(SDL_EGL_GetProcAddress(name)); }

void* Platform::glProcAddress(const char* name) {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

}  // namespace mayo::platform
