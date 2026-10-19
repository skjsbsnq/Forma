#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "../render/image_io.hpp"
#include "app_impl.hpp"
#include "window_impl.hpp"

namespace mayo {

namespace {
App* g_app = nullptr;
/// Idle time after which freed heap memory is handed back to the OS.
constexpr double kTrimDelay = 1.0;
/// Settings are written this long after the last change (a slider drag is a single write).
constexpr double kSettingsSaveDelay = 0.5;

std::string defaultAppName() {
#ifdef __GLIBC__
    if (program_invocation_short_name && *program_invocation_short_name) return program_invocation_short_name;
#endif
    return "mayonnaise";
}

const char* modeName(AppearanceMode m) {
    switch (m) {
        case AppearanceMode::Light: return "light";
        case AppearanceMode::Dark: return "dark";
        case AppearanceMode::System: break;
    }
    return "system";
}

std::string hexColor(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(c.toRGBA8() >> 8));
    return buf;
}
}  // namespace

App::Impl::Impl() {
    if (const char* a = std::getenv("MAYO_APPEARANCE")) {
        envAppearance = std::string(a) == "dark" ? Appearance::Dark : Appearance::Light;
        appearance = *envAppearance;
    }
    if (const char* s = std::getenv("MAYO_SCREENSHOT")) screenshotPath = s;
    if (const char* f = std::getenv("MAYO_SCREENSHOT_FRAMES")) screenshotFrames = std::atoi(f);
    if (!screenshotPath.empty() && screenshotFrames <= 0) screenshotFrames = 5;
    if (const char* d = std::getenv("MAYO_SCREENSHOT_DELAY")) screenshotDelay = std::atof(d);
    rebuildTheme();
}

App::Impl::~Impl() = default;

void App::Impl::rebuildTheme() {
    theme = Theme::make(appearance, accent);
    for (auto& w : windows) w->impl().themeChanged();
}

AppearanceMode App::Impl::mode() const {
    const std::string s = settings->getString("appearance");
    if (s == "light") return AppearanceMode::Light;
    if (s == "dark") return AppearanceMode::Dark;
    if (s == "system") return AppearanceMode::System;
    return defaultMode;
}

bool App::Impl::followsSystemAccent() const {
    const std::string s = settings->getString("accent", "system");
    return s == "system" || !Color::parse(s);
}

void App::Impl::refreshAppearance(bool rereadAccent) {
    Appearance a = Appearance::Light;
    if (envAppearance) {
        a = *envAppearance;
    } else if (const AppearanceMode m = mode(); m == AppearanceMode::System) {
        a = platform::Platform::systemAppearance().value_or(Appearance::Light);
    } else {
        a = m == AppearanceMode::Dark ? Appearance::Dark : Appearance::Light;
    }
    Color c = Theme::defaultAccent();
    if (followsSystemAccent()) {
        if (rereadAccent) systemAccent = platform::Platform::systemAccentColor();
        if (systemAccent) c = nearestAccent(*systemAccent).color;
    } else {
        c = Color::parse(settings->getString("accent")).value_or(Theme::defaultAccent());
    }
    if (a == appearance && c == accent) return;
    appearance = a;
    accent = c;
    rebuildTheme();
}

void App::Impl::dispatch(const platform::PlatformEvent& event) {
    using platform::EventType;
    if (event.type == EventType::Quit) {
        quitRequested = true;
        return;
    }
    if (event.type == EventType::SystemThemeChanged) {
        refreshAppearance(true);
        return;
    }
    // The portal can't be asked to notify us cheaply about accent changes; coming back to the app after
    // changing it in the desktop settings is the moment it matters.
    if (event.type == EventType::WindowFocusGained && followsSystemAccent()) refreshAppearance(true);
    for (auto& w : windows) {
        if (w->impl().platformWindow->id() == event.window) {
            w->impl().handleEvent(event);
            return;
        }
    }
}

void App::Impl::runDueTimers() {
    double now = platform::Platform::now();
    std::vector<std::function<void()>> due;
    {
        std::lock_guard lock(timersMutex);
        std::erase_if(timers, [&](Timer& t) {
            if (t.due > now) return false;
            due.push_back(std::move(t.fn));
            return true;
        });
    }
    for (auto& fn : due) fn();
    if (!due.empty()) trimPending = true;
}

double App::Impl::nextDeadline() const {
    double best = -1;
    {
        std::lock_guard lock(const_cast<std::mutex&>(timersMutex));
        for (const auto& t : timers) {
            if (best < 0 || t.due < best) best = t.due;
        }
    }
    for (const auto& w : windows) {
        double d = w->impl().nextDeadline();
        if (d >= 0 && (best < 0 || d < best)) best = d;
    }
    return best;
}

void App::Impl::renderDueWindows() {
    double now = platform::Platform::now();
    for (auto& w : windows) {
        double d = w->impl().nextDeadline();
        if (d >= 0 && d <= now + 0.001) w->impl().render();
    }
    if (!screenshotPath.empty() && !windows.empty()) {
        if (++framesRendered >= screenshotFrames && platform::Platform::now() - startTime >= screenshotDelay) {
            // All windows share one scene buffer: render the one to capture last.
            const char* which = std::getenv("MAYO_SCREENSHOT_WINDOW");
            Window& target = which && std::string(which) == "last" ? *windows.back() : *windows.front();
            target.impl().render();
            int w = 0;
            int h = 0;
            auto pixels = renderer->readPixels(w, h);
            if (!render::writePng(screenshotPath, w, h, std::move(pixels)))
                std::fprintf(stderr, "mayonnaise: failed to write %s\n", screenshotPath.c_str());
            screenshotPath.clear();
            quitRequested = true;
        } else {
            windows.front()->impl().needsFrame = true;  // keep frames coming until the capture
        }
    }
}

void App::Impl::reapClosedWindows() {
    for (auto& w : windows) {
        if (!w->impl().closed || !w->impl().closedHandler) continue;
        auto handler = std::move(w->impl().closedHandler);
        w->impl().closedHandler = nullptr;
        handler();
    }
    std::erase_if(windows, [](const std::unique_ptr<Window>& w) { return w->impl().closed; });
}

App::App() : App(defaultAppName()) {}

App::App(std::string name, std::string desktopId) : m_impl(std::make_unique<Impl>()) {
    if (g_app) {
        std::fprintf(stderr, "mayonnaise: only one mayo::App may exist at a time\n");
        std::abort();
    }
    g_app = this;
    Impl* impl = m_impl.get();
    impl->name = std::move(name);
    impl->settings = std::make_unique<Settings>(impl->name);
    impl->settings->setReadOnly(!impl->screenshotPath.empty());
    impl->settings->setOnChange([this, impl] {
        if (impl->settingsSavePending) return;
        impl->settingsSavePending = true;
        after(kSettingsSaveDelay, [impl] {
            impl->settingsSavePending = false;
            impl->settings->save();
        });
    });
    if (!platform::Platform::init(desktopId)) std::abort();
    impl->refreshAppearance(true);
}

App::~App() {
    m_impl->settings->save();
    // Shared GPU objects go while a window still has the context current: once the last window is
    // destroyed, glDelete* can jump into an unloaded driver (crashed with Arch's -O3 package flags).
    // Vulkan's device also owns the window surfaces, which must go before their windows.
    m_impl->text.reset();
    m_impl->renderer.reset();
    m_impl->device.reset();
    rhi::setDevice(nullptr);
    m_impl->windows.clear();

    platform::Platform::shutdown();
    g_app = nullptr;
}

App& App::current() { return *g_app; }
const Theme& theme() {
    // Without an App (headless tests, measuring before startup) fall back to the default theme.
    if (!g_app) {
        static const Theme fallback = Theme::make(Appearance::Light);
        return fallback;
    }
    return g_app->theme();
}

Window& App::createWindow(const WindowOptions& options) {
    platform::WindowConfig config;
    config.title = options.title;
    config.width = options.width;
    config.height = options.height;
    config.minWidth = options.minWidth;
    config.minHeight = options.minHeight;
    config.resizable = options.resizable;
    bool maximized = false;
    if (!options.autosaveName.empty()) {
        const Settings& s = *m_impl->settings;
        const std::string key = "window." + options.autosaveName + ".";
        config.width = std::clamp(s.getInt(key + "width", options.width), options.minWidth, 16384);
        config.height = std::clamp(s.getInt(key + "height", options.height), options.minHeight, 16384);
        maximized = s.getBool(key + "maximized");
    }
    // Windows are created for one graphics API, so the renderer backend is chosen before the first one:
    // Vulkan when it suits, else OpenGL (whose device needs the first window's context).
    if (!m_impl->device) {
        m_impl->device = rhi::createVulkanDevice();
        platform::Platform::setGraphicsApi(m_impl->device ? platform::GraphicsApi::Vulkan
                                                          : platform::GraphicsApi::OpenGL);
    }
    auto impl = std::make_unique<Window::Impl>(config);
    impl->showOnRun = !options.initiallyHidden;
    impl->autosaveName = options.autosaveName;
    impl->restoreMaximized = maximized;
    if (!m_impl->device) {
        m_impl->device = rhi::createGLDevice();
        if (!m_impl->device) {
            std::fprintf(stderr, "mayonnaise: failed to load OpenGL 3.3 functions\n");
            std::abort();
        }
    }
    if (rhi::device() != m_impl->device.get()) {
        rhi::setDevice(m_impl->device.get());
        if (const char* v = std::getenv("MAYO_VERBOSE"); v && *v && *v != '0')
            std::fprintf(stderr, "mayonnaise: rendering with %s\n", m_impl->device->description().c_str());
    }
    if (!m_impl->renderer) {
        m_impl->renderer = std::make_unique<render::Renderer2D>();
        m_impl->text = std::make_unique<render::TextSystem>();
    }
    auto window = std::unique_ptr<Window>(new Window(std::move(impl)));
    Window& ref = *window;
    m_impl->windows.push_back(std::move(window));
    if (m_impl->running && ref.impl().showOnRun) ref.impl().show();
    return ref;
}

int App::run() {
    using platform::Platform;
    m_impl->running = true;
    m_impl->startTime = Platform::now();
    // Render once before showing so the first visible frame isn't garbage.
    for (auto& w : m_impl->windows) {
        if (w->impl().showOnRun) w->impl().show();
    }
    while (!m_impl->quitRequested && (m_impl->keepAlive || !m_impl->windows.empty())) {
        double deadline = m_impl->nextDeadline();
        double timeout = deadline < 0 ? -1.0 : std::max(0.0, deadline - Platform::now());
        // glibc keeps freed memory (and worker threads' arenas) mapped; after a burst of work such as image
        // decoding that can be tens of MB. Once the app has been idle for a moment, give it back.
        const bool trimWhenIdle = deadline < 0 && m_impl->trimPending;
        if (trimWhenIdle) timeout = kTrimDelay;
        platform::PlatformEvent event;
        if (Platform::waitEvent(event, timeout)) {
            m_impl->dispatch(event);
            while (Platform::pollEvent(event)) m_impl->dispatch(event);
            m_impl->trimPending = true;
        } else if (trimWhenIdle) {
#ifdef __GLIBC__
            malloc_trim(0);
#endif
            m_impl->trimPending = false;
        }
        m_impl->runDueTimers();
        m_impl->renderDueWindows();
        m_impl->reapClosedWindows();
    }
    m_impl->running = false;
    return m_impl->exitCode;
}

void App::quit(int exitCode) {
    m_impl->exitCode = exitCode;
    m_impl->quitRequested = true;
    platform::Platform::wake();
}
void App::setActivationToken(const std::string& token) { platform::Platform::setActivationToken(token); }

void App::setKeepAlive(bool keepAlive) {
    m_impl->keepAlive = keepAlive;
    platform::Platform::wake();
}

void App::post(std::function<void()> fn) { after(0, std::move(fn)); }

void App::after(double seconds, std::function<void()> fn) {
    {
        std::lock_guard lock(m_impl->timersMutex);
        m_impl->timers.push_back({platform::Platform::now() + seconds, std::move(fn)});
    }
    platform::Platform::wake();
}

namespace {
void showDialog(App& app, Window* parent, const std::vector<FileFilter>& filters, bool folders, bool multiple,
                std::function<void(std::vector<std::string>)> done) {
    std::vector<platform::DialogFilter> f;
    for (const auto& x : filters) f.push_back({x.name, x.pattern});
    platform::PlatformWindow* pw = parent ? parent->impl().platformWindow.get() : nullptr;
    // The platform may answer on a helper thread; hop back to the UI thread.
    platform::Platform::showOpenDialog(pw, f, folders, multiple,
                                       [&app, done = std::move(done)](std::vector<std::string> paths) mutable {
                                           app.post([done, paths = std::move(paths)] { done(paths); });
                                       });
}
}  // namespace

void App::openFiles(const std::vector<FileFilter>& filters, bool multiple,
                    std::function<void(std::vector<std::string>)> done, Window* parent) {
    if (!parent && !m_impl->windows.empty()) parent = m_impl->windows.front().get();
    showDialog(*this, parent, filters, false, multiple, std::move(done));
}

void App::openFolder(std::function<void(std::vector<std::string>)> done, Window* parent) {
    if (!parent && !m_impl->windows.empty()) parent = m_impl->windows.front().get();
    showDialog(*this, parent, {}, true, false, std::move(done));
}

const std::string& App::name() const { return m_impl->name; }
Settings& App::settings() { return *m_impl->settings; }

void App::setAppearanceMode(AppearanceMode mode) {
    m_impl->envAppearance.reset();  // an explicit choice beats MAYO_APPEARANCE
    m_impl->settings->set("appearance", modeName(mode));
    m_impl->refreshAppearance(false);
}
AppearanceMode App::appearanceMode() const { return m_impl->mode(); }
void App::setDefaultAppearanceMode(AppearanceMode mode) {
    m_impl->defaultMode = mode;
    m_impl->refreshAppearance(false);
}
void App::setAppearance(Appearance appearance) {
    setAppearanceMode(appearance == Appearance::Dark ? AppearanceMode::Dark : AppearanceMode::Light);
}
Appearance App::appearance() const { return m_impl->appearance; }

void App::setAccentColor(Color accent) {
    m_impl->settings->set("accent", hexColor(accent));
    m_impl->refreshAppearance(false);
}
void App::setAccentFollowsSystem(bool follow) {
    m_impl->settings->set("accent", follow ? std::string("system") : hexColor(m_impl->accent));
    m_impl->refreshAppearance(follow);
}
bool App::accentFollowsSystem() const { return m_impl->followsSystemAccent(); }
Color App::accentColor() const { return m_impl->accent; }
const Theme& App::theme() const { return m_impl->theme; }

}  // namespace mayo
