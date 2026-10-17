#pragma once

// Freedesktop.org applications: the installed Desktop Entries, the user's default application per MIME type
// (mimeapps.list) and the command line that starts an application with a file. Parsing and launching only — the
// "Open With" and "Properties" panels live in file_panels.cpp.

#include <string>
#include <vector>

namespace finder {

/// An installed .desktop entry.
struct Application {
    std::string id;    ///< file name including ".desktop"
    std::string name;
    std::string icon;  ///< themed icon name
    std::string path;  ///< the .desktop file itself
};

/// Every usable entry of $XDG_DATA_HOME/applications and $XDG_DATA_DIRS/applications, sorted by name (a file in
/// an earlier directory hides same-named ones further down). Read once per process.
const std::vector<Application>& installedApplications();
/// "" when no such entry is installed.
const Application* findApplication(const std::string& id);
std::string applicationDisplayName(const std::string& desktopId);
std::string applicationIcon(const std::string& desktopId);

/// Type of `path` from its content and name (shared-mime-info's guesses); "inode/directory" for folders.
std::string mimeTypeFor(const std::string& path);
/// The text shown for a type ("JPEG 图像" for image/jpeg), or "" when unknown.
std::string mimeTypeLabel(const std::string& type);
/// Keep MIME detection inside this process (no xdg-mime helper): used by demo runs and tests.
void classifyInProcessOnly(bool on);

/// Desktop id the user opens items like `path` with (mimeapps.list, then the desktop's defaults), or "".
std::string defaultApplicationId(const std::string& path);
/// Applications worth offering for `path`: the default first, then everything else that opens this kind of
/// file. Folder are passed right through to their default only.
std::vector<std::string> applicationsFor(const std::string& path);
/// Why `appId` can't be made the default for `path` (not installed, opens neither this type nor this kind of
/// file), or "".
std::string defaultApplicationProblem(const std::string& path, const std::string& appId);
/// Stores the user's choice for the item's MIME type in $XDG_CONFIG_HOME/mimeapps.list, where every application
/// reads it ("" forgets it again). False when the file can't be written.
bool setDefaultApplication(const std::string& path, const std::string& appId);
/// Whether the user's pick for `path` actually ends up as the default (another file may override it).
bool pickBecomesDefault(const std::string& path, const std::string& appId);

/// The command line that runs `appId` on `path` (%f/%F/%u/%U expanded and quoted). Returns the program, which is
/// "" with `error` set when the entry doesn't declare how to open this item. Terminal applications are returned
/// unwrapped; launchApplication wraps them.
std::string launchCommand(const std::string& appId, const std::string& path, std::vector<std::string>* argv,
                          std::string* error);
/// Starts `appId` on `path` detached. On failure (false) `error` says why, e.g. the program isn't installed.
bool launchApplication(const std::string& appId, const std::string& path, std::string* error);
/// Runs argv[0] (searched in PATH when it has no "/") detached with its stdio on /dev/null.
bool spawnDetachedProcess(const std::vector<std::string>& argv, std::string* error);
/// Same, started in `cwd`.
bool spawnDetachedIn(const std::vector<std::string>& argv, const std::string& cwd, std::string* error = nullptr);

}  // namespace finder
