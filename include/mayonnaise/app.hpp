#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "color.hpp"
#include "settings.hpp"
#include "theme.hpp"

namespace mayo {

class Window;

struct WindowOptions {
    std::string title = "Mayonnaise";
    int width = 800;    ///< points
    int height = 600;   ///< points
    int minWidth = 320;
    int minHeight = 200;
    bool resizable = true;
    /// When set, the window's size and maximized state are remembered in the app's settings under this name
    /// and restored next time ("main" for the main window).
    std::string autosaveName{};
    /// Create and initialize the window without mapping it; call Window::show() when ready.
    bool initiallyHidden = false;
};

/// How the app picks light or dark.
enum class AppearanceMode : std::uint8_t {
    System,  ///< follow the desktop (and switch live when it changes)
    Light,
    Dark,
};

struct FileFilter {
    std::string name;     ///< "Images"
    std::string pattern;  ///< "png;jpg;jpeg" (semicolon separated extensions, "*" for all)
};

/// Owns the event loop, the windows, the global appearance and the app's settings. Create exactly one.
///
/// The name picks the settings directory (`~/.config/mayonnaise/<name>/`); the appearance mode and accent
/// colour are stored there and restored at startup. By default the app follows the desktop's light/dark
/// preference and accent colour. MAYO_APPEARANCE=dark|light overrides the appearance without saving it.
class App {
public:
    /// Uses the executable's name as the app name.
    App();
    /// `desktopId` is the app's .desktop file name without the extension ("org.example.Notes"); it becomes the
    /// Wayland app_id / X11 class, which is how docks and task switchers find the icon. Defaults to the
    /// executable's name.
    explicit App(std::string name, std::string desktopId = {});
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// The returned reference stays valid until the window is closed.
    Window& createWindow(const WindowOptions& options = {});

    /// Runs until the last window closes or quit() is called. Returns the process exit code.
    int run();
    void quit(int exitCode = 0);
    /// Keep the event loop alive after the last window closes (for background applications).
    /// Hidden windows also remain dormant until shown. Defaults to false.
    void setKeepAlive(bool keepAlive);
    /// Forward an activation token from a launcher to a resident app before showing its window.
    void setActivationToken(const std::string& token);

    /// Runs `fn` on the UI thread at the next loop iteration. Safe to call from any thread.
    void post(std::function<void()> fn);
    /// Runs `fn` on the UI thread after `seconds`.
    void after(double seconds, std::function<void()> fn);

    /// Native "Open" dialog. `done` runs on the UI thread with the chosen paths (empty if cancelled).
    void openFiles(const std::vector<FileFilter>& filters, bool multiple,
                   std::function<void(std::vector<std::string>)> done, Window* parent = nullptr);
    void openFolder(std::function<void(std::vector<std::string>)> done, Window* parent = nullptr);

    [[nodiscard]] const std::string& name() const;
    /// Persistent preferences for this app (see Settings).
    [[nodiscard]] Settings& settings();

    /// Chooses light, dark or the desktop's preference; saved in the settings.
    void setAppearanceMode(AppearanceMode mode);
    [[nodiscard]] AppearanceMode appearanceMode() const;
    /// Mode used until the user picks one (e.g. dark for a media player). Not saved.
    void setDefaultAppearanceMode(AppearanceMode mode);
    /// Shorthand for setAppearanceMode(Light / Dark).
    void setAppearance(Appearance appearance);
    /// The effective appearance right now.
    [[nodiscard]] Appearance appearance() const;

    /// A fixed accent colour (saved); see accentChoices() for the macOS palette.
    void setAccentColor(Color accent);
    /// Follow the desktop's accent colour (mapped onto the macOS palette); the default. Saved.
    void setAccentFollowsSystem(bool follow);
    [[nodiscard]] bool accentFollowsSystem() const;
    [[nodiscard]] Color accentColor() const;

    [[nodiscard]] const Theme& theme() const;

    /// The live App; only valid between construction and destruction.
    static App& current();

    struct Impl;
    [[nodiscard]] Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

/// Shorthand for `App::current().theme()`.
const Theme& theme();

}  // namespace mayo
