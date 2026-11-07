// KDE Plasma support: which desktop is running, and KConfig files (~/.config/kwinrc, kdeglobals, ...) read
// and written the way KConfig does, so Plasma picks the changes up live.
//
// Writes go to the user's file (system defaults in $XDG_CONFIG_DIRS are read, never written), then the
// org.kde.kconfig.notify ConfigChanged signal tells KConfigWatcher users (KWin, plasmashell, apps) what
// changed. Services that don't watch their files get an explicit nudge (Reload). Files are monitored, so
// changes made by KDE's own System Settings show up here too. UI thread only.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "glib_support.hpp"

namespace desk {

enum class Desktop { Gnome, Kde, Other };

/// From XDG_CURRENT_DESKTOP; SETTINGS_DESKTOP=gnome|kde overrides it (testing).
[[nodiscard]] Desktop current();
[[nodiscard]] inline bool kde() { return current() == Desktop::Kde; }

}  // namespace desk

namespace kcfg {

/// What to poke after writing, for services that don't watch their configuration.
enum class Reload {
    None,
    KWin,          ///< org.kde.KWin.reconfigure
    Keyboard,      ///< org.kde.keyboard reloadConfig (layouts)
    PowerDevil,    ///< org.kde.Solid.PowerManagement.refreshStatus
    ScreenLocker,  ///< org.kde.screensaver.configure
    Fonts,         ///< KGlobalSettings notifyChange(FontChanged)
    Style,         ///< KGlobalSettings notifyChange(StyleChanged)
    Icons,         ///< KGlobalSettings notifyChange(IconChanged)
    Cursor,        ///< KGlobalSettings notifyChange(CursorChanged)
    Mouse,         ///< KGlobalSettings notifyChange(SettingsChanged, mouse): double-click, single click
    Kio,           ///< KIO reparseSlaveConfiguration (proxy)
};

/// Nested groups are written like the file does: "Containments][1][Wallpaper".
[[nodiscard]] std::optional<std::string> read(const std::string& file, const std::string& group, const std::string& key);
/// nullopt removes the entry (back to the default). Skipped in read-only mode.
gx::WriteResult write(const std::string& file, const std::string& group, const std::string& key,
                      const std::optional<std::string>& value, Reload reload = Reload::None);
struct Change {
    std::string group;
    std::string key;
    std::optional<std::string> value;
};
/// Commit related entries atomically; failure leaves every cached value and line unchanged.
gx::WriteResult writeEntries(const std::string& file, const std::vector<Change>& changes, Reload reload = Reload::None);
[[nodiscard]] bool writable(const std::string& file, const std::string& group, const std::string& key);
/// `fn` runs when the entry changes, from this app or any other.
void watch(const std::string& file, const std::string& group, const std::string& key, std::function<void()> fn);
/// Group names in the user's file, in file order.
[[nodiscard]] std::vector<std::string> groups(const std::string& file);
/// Keys of one group in the user's file.
[[nodiscard]] std::vector<std::string> keys(const std::string& file, const std::string& group);
/// Runs the reload action now (debounced).
void reload(Reload r);

/// A key bound to one KConfig entry, for the ui:: rows. With `emptyRemoves`, writing "" deletes the entry.
[[nodiscard]] gx::Key key(const std::string& file, const std::string& group, const std::string& key, const std::string& type,
                          const std::string& fallback, Reload reload = Reload::None, bool emptyRemoves = false);
/// One value kept in several groups (PowerDevil's AC / Battery / LowBattery profiles): read from the first.
[[nodiscard]] gx::Key multiKey(const std::string& file, const std::vector<std::string>& groups, const std::string& key,
                               const std::string& type, const std::string& fallback, Reload reload = Reload::None);
/// A boolean shown for an entry that holds other values (`on` when true, `off` when false; nullopt removes it).
[[nodiscard]] gx::Key flagKey(const std::string& file, const std::string& group, const std::string& key,
                              const std::string& on, const std::optional<std::string>& off, bool fallback,
                              Reload reload = Reload::None);
/// A key whose writes are done by `apply` (usually a Plasma command line tool that also updates the file),
/// read from the file.
[[nodiscard]] gx::Key appliedKey(const std::string& file, const std::string& group, const std::string& key,
                                 const std::string& type, const std::string& fallback,
                                 std::function<gx::WriteResult(const std::string& value, gx::WriteDone)> apply);

/// KConfig value escaping (\s, \t, \n, \\, \xNN).
[[nodiscard]] std::string unescape(const std::string& raw);
[[nodiscard]] std::string escape(const std::string& value);

/// First of `names` found on PATH or in a libexec directory, empty if none (plasma-changeicons lives in libexec).
[[nodiscard]] std::string findTool(std::initializer_list<const char*> names);

}  // namespace kcfg
