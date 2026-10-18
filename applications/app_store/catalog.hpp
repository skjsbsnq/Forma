// Flatpak catalogue for Forma App Store: AppStream metadata, installed refs and category curation.
//
// Everything here is UI-free and safe to run on a worker thread; only the icon cache (ui.cpp) touches
// the renderer. Commands are executed with fork/execvp (never a shell), so ids from AppStream are first
// validated against the characters a Flatpak reference may contain.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forma::store {

struct Screenshot {
    std::string caption;
    std::string source;     ///< full-size image URL
    std::string thumbnail;  ///< small image URL (falls back to source)
    int width = 0;
    int height = 0;
};

struct AppEntry {
    std::string id;
    std::string name;
    std::string summary;
    std::string developer;
    std::string description;  ///< plain text, paragraphs separated by blank lines
    std::string homepage;
    std::string license;
    std::string runtime;
    std::string iconPath;  ///< local PNG, when AppStream shipped a cached copy
    std::string iconUrl;   ///< remote icon, used when iconPath is empty
    std::string accent;    ///< "#RRGGBB" brand colour from AppStream
    std::vector<std::string> categories;
    std::vector<std::string> keywords;
    std::vector<Screenshot> screenshots;
    std::string version;        ///< latest version known to AppStream
    std::string installedVersion;
    std::string branch = "stable";
    std::string arch = "x86_64";
    std::string origin;          ///< remote the app comes from ("flathub")
    std::string installation;    ///< "system" / "user" when installed
    std::string releaseNotes;
    std::int64_t releaseTimestamp = 0;
    std::uint64_t downloadSize = 0;
    std::uint64_t installedSize = 0;
    int ageRating = 0;  ///< 0 = not rated, else 4 / 9 / 12 / 17
    bool installed = false;
    bool verified = false;
    bool updateAvailable = false;
    bool eol = false;

    /// The primary AppStream category, e.g. "AudioVideo".
    [[nodiscard]] std::string primaryCategory() const;
};

/// A catalogue row: every desktop application known to AppStream plus installed apps that AppStream
/// does not describe (locally built bundles, side-loaded refs).
struct Catalog {
    std::vector<AppEntry> apps;
    std::map<std::string, std::size_t> byId;
    std::vector<std::string> remotes;
    std::string appStreamPath;
    std::string error;  ///< set when Flatpak could not be queried
    bool flatpakAvailable = false;

    [[nodiscard]] const AppEntry* find(std::string_view id) const;
    [[nodiscard]] std::vector<const AppEntry*> search(std::string_view query, std::size_t limit = 120) const;
    [[nodiscard]] std::vector<const AppEntry*> inCategory(std::string_view category, std::size_t limit = 120) const;
    [[nodiscard]] std::vector<const AppEntry*> installedApps() const;
    [[nodiscard]] std::vector<const AppEntry*> updates() const;
};

// --- command execution ---------------------------------------------------------------------------

struct CommandResult {
    int exitCode = -1;
    std::string output;  ///< combined stdout + stderr
    bool timedOut = false;
};

/// Runs a program directly (no shell) with a cleaned environment (LC_ALL=C, so columns and numbers are
/// stable). `onOutput` receives decoded chunks as they arrive; the whole output is returned as well.
/// Kills the process (group) after `timeoutSeconds` when > 0.
CommandResult run(const std::vector<std::string>& argv, int timeoutSeconds = 0,
                  const std::function<void(std::string_view)>& onOutput = {});

/// Starts a program detached from the store (the app keeps running after the store quits).
bool launchDetached(const std::vector<std::string>& argv);

/// True when `id` looks like a Flatpak app id ("org.example.App").
bool validAppId(std::string_view id);

// --- catalogue loading ---------------------------------------------------------------------------

/// The newest cached AppStream file for a Flatpak installation, or an empty string.
std::string findAppStreamFile(const std::vector<std::string>& extraRoots = {});

/// The icon cache folder that belongs to an AppStream file (`<dir>/icons`, or `<dir>` when the icons sit
/// directly next to the XML). Empty when the file path is unusable.
std::string iconRootFor(const std::string& appStreamPath);

/// Parses an AppStream document. `iconRoot` is the directory that contains `128x128/<id>.png`
/// (the `<appstream>/<arch>/active` folder) and may be empty.
std::vector<AppEntry> parseAppStream(std::string_view xml, const std::string& iconRoot = {});

/// Reads installed applications (`flatpak list --app`). Empty when Flatpak is missing.
std::vector<AppEntry> listInstalled();
/// Remote names from `flatpak remotes`.
std::vector<std::string> listRemotes();
/// Download/installed sizes for every available app, keyed by id. Empty on failure (sizes stay 0).
std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> remoteSizes();

/// Full load: AppStream + installed apps merged, sizes filled in. Blocking; call from a worker thread.
Catalog loadCatalog(std::string appStreamPath = {});

/// Authoritative update check (needs the network). Blocking; empty when the query fails.
std::vector<std::string> queryUpdates();

// --- formatting and localisation -----------------------------------------------------------------

std::string formatBytes(std::uint64_t bytes);
std::string formatDate(std::int64_t unixSeconds);
/// Chinese display name for an AppStream category ("AudioVideo" -> "音频与视频").
std::string categoryName(std::string_view category);
/// Chinese name for the curated store sections.
std::string sectionName(std::string_view section);
/// The curated sections, in sidebar order (ids are used by the UI and by `inCategory`).
struct Section {
    std::string id;
    std::string title;
    std::string subtitle;
    std::vector<std::string> categories;  ///< AppStream categories that belong here
    std::vector<std::string> featured;    ///< hand-picked apps shown first
};
const std::vector<Section>& sections();
const Section* section(std::string_view id);

/// The apps shown on the Discover page, in a stable editorial order.
std::vector<std::string> discoverOrder();
/// "今日应用" hero rotation.
std::vector<std::string> heroOrder();

/// Highlight matching subsequence positions of `query` in `text` (for bold search results).
std::vector<std::size_t> matchPositions(std::string_view text, std::string_view query);

}  // namespace forma::store
