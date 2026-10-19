#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../platform/platform.hpp"
#include "../render/renderer2d.hpp"
#include "../render/rhi/rhi.hpp"
#include "../render/text_system.hpp"
#include "mayonnaise/app.hpp"
#include "mayonnaise/window.hpp"

namespace mayo {

struct App::Impl {
    std::vector<std::unique_ptr<Window>> windows;
    Theme theme;
    std::string name;
    std::unique_ptr<Settings> settings;
    bool settingsSavePending = false;
    Appearance appearance = Appearance::Light;  // effective
    AppearanceMode defaultMode = AppearanceMode::System;
    std::optional<Appearance> envAppearance;     // MAYO_APPEARANCE: wins, never saved
    Color accent = Theme::defaultAccent();       // effective
    std::optional<Color> systemAccent;           // last value read from the desktop
    bool running = false;
    bool keepAlive = false;
    bool trimPending = false;  // work happened since the heap was last returned to the OS
    struct Timer {
        double due;
        std::function<void()> fn;
    };
    std::vector<Timer> timers;
    std::mutex timersMutex;  // post() may be called from worker threads
    void runDueTimers();
    std::unique_ptr<rhi::Device> device;           // Vulkan or OpenGL, shared by all windows
    std::unique_ptr<render::Renderer2D> renderer;  // shared by all windows
    std::unique_ptr<render::TextSystem> text;
    bool quitRequested = false;
    int exitCode = 0;



    // MAYO_SCREENSHOT support: dump the first window after N frames and quit.
    std::string screenshotPath;
    int screenshotFrames = 0;
    int framesRendered = 0;
    double screenshotDelay = 0.4;  // seconds; frames alone may finish before timers fire
    double startTime = 0;

    Impl();
    ~Impl();
    void rebuildTheme();
    /// Re-reads the desktop preferences and the saved choices, and rebuilds the theme if anything changed.
    void refreshAppearance(bool rereadAccent);
    [[nodiscard]] AppearanceMode mode() const;
    [[nodiscard]] bool followsSystemAccent() const;
    void dispatch(const platform::PlatformEvent& event);
    [[nodiscard]] double nextDeadline() const;  // absolute time; <0 = none
    void renderDueWindows();
    void reapClosedWindows();
};

}  // namespace mayo
