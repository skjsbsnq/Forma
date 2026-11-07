#include "kconfig.hpp"

#include <glib/gstdio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "ui.hpp"

namespace desk {

Desktop current() {
    static const Desktop d = [] {
        auto upper = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        };
        if (const char* force = std::getenv("SETTINGS_DESKTOP")) {
            const std::string f = upper(force);
            if (f == "KDE") return Desktop::Kde;
            if (f == "GNOME") return Desktop::Gnome;
            return Desktop::Other;
        }
        if (const char* xdg = std::getenv("XDG_CURRENT_DESKTOP")) {
            for (const auto& part : ui::split(upper(xdg), ':')) {
                if (part == "KDE") return Desktop::Kde;
                if (part.rfind("GNOME", 0) == 0) return Desktop::Gnome;
            }
        }
        if (std::getenv("KDE_FULL_SESSION")) return Desktop::Kde;
        return Desktop::Other;
    }();
    return d;
}

}  // namespace desk

namespace kcfg {

namespace {

using Values = std::map<std::pair<std::string, std::string>, std::string>;

struct File {
    std::string name;
    std::string path;
    std::vector<std::string> lines;
    Values values;
    std::vector<std::string> groupOrder;
    std::set<std::pair<std::string, std::string>> locks;
    Values defaults;  ///< system-wide files ($XDG_CONFIG_DIRS)
    bool loaded = false;
    GFileMonitor* monitor = nullptr;
};

std::string configHome() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return x;
    return std::string(g_get_home_dir()) + "/.config";
}

std::string trim(const std::string& s) { return ui::trim(s); }

/// Entry name without KConfig's option markers ("folders[$e]" -> "folders"); empty for localized entries.
std::string entryName(std::string key, bool* expand = nullptr) {
    const size_t br = key.find('[');
    if (br == std::string::npos) return key;
    const std::string opts = key.substr(br);
    if (opts.rfind("[$", 0) != 0) return {};  // "Name[de]" — a translation, not the entry
    if (expand) *expand = opts.find('e') != std::string::npos;
    return key.substr(0, br);
}

std::string expandHome(std::string v) {
    const std::string home = g_get_home_dir();
    for (size_t pos; (pos = v.find("$HOME")) != std::string::npos;) v.replace(pos, 5, home);
    return v;
}

/// "[A][B]" -> "A][B"; empty for lines that aren't group headers.
std::string headerName(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return {};
    const std::string h = t.substr(1, t.size() - 2);
    return h.ends_with("][$i") ? h.substr(0, h.size() - 4) : h;
}

void parse(const std::vector<std::string>& lines, Values& values, std::vector<std::string>* order) {
    std::string group = "<default>";
    for (const auto& raw : lines) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            group = headerName(line);
            if (order && !group.empty() && group[0] != '$') order->push_back(group);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        bool expand = false;
        const std::string key = entryName(trim(line.substr(0, eq)), &expand);
        if (key.empty()) continue;
        std::string value = unescape(trim(line.substr(eq + 1)));
        if (expand) value = expandHome(value);
        values[{group, key}] = value;
    }
}

void scanLocks(const std::vector<std::string>& lines, File& f) {
    std::string group = "<default>";
    for (const auto& raw : lines) {
        const std::string line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            group = headerName(line);
            if (line == "[$i]")
                f.locks.insert({"*", "*"});
            else if (line.ends_with("[$i]"))
                f.locks.insert({group, "*"});
        } else {
            const auto eq = line.find('=');
            const std::string lhs = trim(line.substr(0, eq));
            if (eq != std::string::npos && lhs.find("[$i]") != std::string::npos)
                f.locks.insert({group, entryName(lhs)});
        }
    }
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
}

std::map<std::string, std::vector<std::function<void()>>>& watchers() {
    static std::map<std::string, std::vector<std::function<void()>>> w;
    return w;
}

std::string watchId(const std::string& file, const std::string& group, const std::string& key) {
    return file + '\n' + group + '\n' + key;
}

void fire(const std::string& id) {
    auto it = watchers().find(id);
    if (it == watchers().end()) return;
    auto fns = it->second;  // a watcher may add watchers
    for (auto& fn : fns) fn();
}

std::map<std::string, std::unique_ptr<File>>& files() {
    static std::map<std::string, std::unique_ptr<File>> f;
    return f;
}

void load(File& f) {
    f.lines = readLines(f.path);
    f.values.clear();
    f.groupOrder.clear();
    parse(f.lines, f.values, &f.groupOrder);
    f.locks.clear();
    scanLocks(f.lines, f);
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    for (const auto& dir : ui::split(dirs && *dirs ? dirs : "/etc/xdg", ':'))
        scanLocks(readLines(dir + "/" + f.name), f);
}

void reloadFromDisk(const std::string& name) {
    auto it = files().find(name);
    if (it == files().end()) return;
    File& f = *it->second;
    const Values before = f.values;
    load(f);
    const std::string prefix = name + '\n';
    for (auto w = watchers().lower_bound(prefix); w != watchers().end() && w->first.rfind(prefix, 0) == 0; ++w) {
        const std::string rest = w->first.substr(prefix.size());
        const size_t nl = rest.find('\n');
        const std::pair<std::string, std::string> k{rest.substr(0, nl), rest.substr(nl + 1)};
        auto a = before.find(k);
        auto b = f.values.find(k);
        const bool same = (a == before.end()) == (b == f.values.end()) && (a == before.end() || a->second == b->second);
        if (!same) fire(w->first);
    }
}

void onFileChanged(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer data) {
    if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_DELETED && event != G_FILE_MONITOR_EVENT_MOVED_IN && event != G_FILE_MONITOR_EVENT_RENAMED)
        return;
    gx::toUi([name = *static_cast<std::string*>(data)] { reloadFromDisk(name); });
}

File& file(const std::string& name) {
    auto& slot = files()[name];
    if (!slot) slot = std::make_unique<File>();
    File& f = *slot;
    if (f.loaded) return f;
    f.loaded = true;
    f.name = name;
    f.path = configHome() + "/" + name;
    load(f);
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    auto list = ui::split(dirs && *dirs ? dirs : "/etc/xdg", ':');
    // Earlier directories win, so read them last.
    for (auto d = list.rbegin(); d != list.rend(); ++d) parse(readLines(*d + "/" + name), f.defaults, nullptr);
    GFile* gf = g_file_new_for_path(f.path.c_str());
    f.monitor = g_file_monitor_file(gf, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(gf);
    if (f.monitor) g_signal_connect(f.monitor, "changed", G_CALLBACK(onFileChanged), new std::string(name));
    return f;
}

bool isEntryLine(const std::string& raw, const std::string& key) {
    const std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == '[') return false;
    const size_t eq = line.find('=');
    return eq != std::string::npos && entryName(trim(line.substr(0, eq))) == key;
}

/// D-Bus object path KConfigWatcher listens on: "/" + file name, with characters paths can't hold replaced.
std::string notifyPath(const std::string& name) {
    std::string p = "/" + name;
    for (size_t i = 1; i < p.size(); ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (!std::isalnum(c) && c != '_' && c != '/') p[i] = '_';
    }
    return p;
}

void notifyChanged(const std::string& name, const std::string& group, const std::string& key) {
    GDBusConnection* bus = gx::sessionBus();
    const std::string path = notifyPath(name);
    if (!bus || !g_variant_is_object_path(path.c_str())) return;
    // KConfig separates nested group names with \x1d in the notification.
    std::string g = group;
    for (size_t pos; (pos = g.find("][")) != std::string::npos;) g.replace(pos, 2, "\x1d");
    GVariantBuilder keys;
    g_variant_builder_init(&keys, G_VARIANT_TYPE("aay"));
    g_variant_builder_add_value(&keys, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, key.data(), key.size(), 1));
    GVariantBuilder dict;
    g_variant_builder_init(&dict, G_VARIANT_TYPE("a{saay}"));
    g_variant_builder_add(&dict, "{s@aay}", g.c_str(), g_variant_builder_end(&keys));
    g_dbus_connection_emit_signal(bus, nullptr, path.c_str(), "org.kde.kconfig.notify", "ConfigChanged",
                                  g_variant_new("(@a{saay})", g_variant_builder_end(&dict)), nullptr);
}

void emitSignal(const char* path, const char* iface, const char* member, GVariant* params) {
    if (GDBusConnection* bus = gx::sessionBus()) g_dbus_connection_emit_signal(bus, nullptr, path, iface, member, params, nullptr);
    else if (params) g_variant_unref(g_variant_ref_sink(params));
}

void runReload(Reload r) {
    auto bus = gx::sessionBus();
    auto globalChange = [](int type) {
        emitSignal("/KGlobalSettings", "org.kde.KGlobalSettings", "notifyChange", g_variant_new("(ii)", type, 0));
    };
    switch (r) {
        case Reload::None: break;
        case Reload::KWin: gx::call(bus, "org.kde.KWin", "/KWin", "org.kde.KWin", "reconfigure", nullptr, {}); break;
        case Reload::Keyboard: emitSignal("/Layouts", "org.kde.keyboard", "reloadConfig", nullptr); break;
        case Reload::PowerDevil:
            gx::call(bus, "org.kde.Solid.PowerManagement", "/org/kde/Solid/PowerManagement", "org.kde.Solid.PowerManagement",
                     "refreshStatus", nullptr, {});
            break;
        case Reload::ScreenLocker:
            gx::call(bus, "org.freedesktop.ScreenSaver", "/ScreenSaver", "org.kde.screensaver", "configure", nullptr, {});
            break;
        case Reload::Fonts: globalChange(1); break;
        case Reload::Style: globalChange(2); break;
        case Reload::Icons: globalChange(4); break;
        case Reload::Cursor: globalChange(5); break;
        case Reload::Mouse: globalChange(3); break;
        case Reload::Kio:
            emitSignal("/KIO/Scheduler", "org.kde.KIO.Scheduler", "reparseSlaveConfiguration", g_variant_new("(s)", ""));
            break;
    }
}

}  // namespace

std::string unescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        const char c = raw[++i];
        switch (c) {
            case 's': out.push_back(' '); break;
            case 't': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case '\\': out.push_back('\\'); break;
            case 'x':
                if (i + 2 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
                    out.push_back(static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16)));
                    i += 2;
                } else {
                    out += "\\x";
                }
                break;
            default:
                // "\," and "\;" are list escapes; keep them for splitList().
                out.push_back('\\');
                out.push_back(c);
        }
    }
    return out;
}

std::string escape(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c == '\\' && !(i + 1 < value.size() && (value[i + 1] == ',' || value[i + 1] == ';'))) out += "\\\\";
        else if (c == ' ' && (i == 0 || i + 1 == value.size())) out += "\\s";
        else out.push_back(c);
    }
    return out;
}

std::optional<std::string> read(const std::string& name, const std::string& group, const std::string& key) {
    File& f = file(name);
    if (auto it = f.values.find({group, key}); it != f.values.end()) return it->second;
    if (auto it = f.defaults.find({group, key}); it != f.defaults.end()) return it->second;
    return std::nullopt;
}

bool writable(const std::string& name, const std::string& group, const std::string& key) {
    if (gx::readOnly()) return false;
    const File& f = file(name);
    if (f.locks.contains({"*", "*"}) || f.locks.contains({group, key})) return false;
    for (std::string ancestor = group;;) {
        if (f.locks.contains({ancestor, "*"})) return false;
        const auto split = ancestor.rfind("][");
        if (split == std::string::npos) break;
        ancestor.resize(split);
    }
    std::error_code ec;
    std::filesystem::path path = f.path;
    if (std::filesystem::exists(path, ec) &&
        (!std::filesystem::is_regular_file(path, ec) || g_access(path.c_str(), W_OK) != 0))
        return false;
    path = path.parent_path();
    while (!path.empty() && !std::filesystem::exists(path, ec)) path = path.parent_path();
    return !path.empty() && std::filesystem::is_directory(path, ec) && g_access(path.c_str(), W_OK | X_OK) == 0;
}

gx::WriteResult write(const std::string& name, const std::string& group, const std::string& key,
                      const std::optional<std::string>& value, Reload reload) {
    return writeEntries(name, {{group, key, value}}, reload);
}

gx::WriteResult writeEntries(const std::string& name, const std::vector<Change>& changes, Reload reload) {
    if (gx::readOnly()) return gx::WriteResult::readOnly();
    File& f = file(name);
    auto lines = f.lines;
    std::vector<Change> changed;
    for (const auto& change : changes) {
        const auto& [group, key, value] = change;
        if (!writable(name, group, key)) return gx::WriteResult::readOnly(f.path + ": entry is locked or not writable");
        auto current = f.values.find({group, key});
        if (value ? (current != f.values.end() && current->second == *value) : current == f.values.end()) continue;
        changed.push_back(change);
        // Edit a candidate copy so comments, ordering and entries we don't know survive.
        size_t header = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (headerName(lines[i]) == group) {
                header = i;
                break;
            }
        }
        const std::string entry = value ? key + "=" + escape(*value) : std::string();
        if (header == lines.size()) {
            if (value) {
                if (!lines.empty() && !trim(lines.back()).empty()) lines.emplace_back();
                lines.push_back("[" + group + "]");
                lines.push_back(entry);
            }
        } else {
            size_t end = header + 1;
            while (end < lines.size() && headerName(lines[end]).empty()) ++end;
            size_t found = end;
            for (size_t i = header + 1; i < end; ++i) {
                if (isEntryLine(lines[i], key)) found = i;
            }
            if (found < end) {
                if (value)
                    lines[found] = entry;
                else
                    lines.erase(lines.begin() + static_cast<long>(found));
            } else if (value) {
                size_t at = end;
                while (at > header + 1 && trim(lines[at - 1]).empty()) --at;
                lines.insert(lines.begin() + static_cast<long>(at), entry);
            }
        }
    }
    if (changed.empty()) return {};
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    char* dir = g_path_get_dirname(f.path.c_str());
    const int made = g_mkdir_with_parents(dir, 0700);
    const int savedErrno = errno;
    g_free(dir);
    if (made != 0) return gx::WriteResult::failure(f.path + ": " + std::strerror(savedErrno));
    GError* error = nullptr;
    const bool saved = g_file_set_contents(f.path.c_str(), text.c_str(), static_cast<gssize>(text.size()), &error);
    if (!saved) {
        const std::string reason = error ? error->message : "Cannot write configuration file";
        g_clear_error(&error);
        return gx::WriteResult::failure(f.path + ": " + reason);
    }
    f.lines = std::move(lines);
    f.values.clear();
    f.groupOrder.clear();
    parse(f.lines, f.values, &f.groupOrder);
    for (const auto& change : changed) {
        notifyChanged(name, change.group, change.key);
        fire(watchId(name, change.group, change.key));
    }
    kcfg::reload(reload);
    return {};
}

void watch(const std::string& name, const std::string& group, const std::string& key, std::function<void()> fn) {
    (void)file(name);  // starts monitoring
    watchers()[watchId(name, group, key)].push_back(std::move(fn));
}

std::vector<std::string> groups(const std::string& name) { return file(name).groupOrder; }

std::vector<std::string> keys(const std::string& name, const std::string& group) {
    std::vector<std::string> out;
    for (const auto& [k, v] : file(name).values) {
        if (k.first == group) out.push_back(k.second);
    }
    return out;
}

void reload(Reload r) {
    if (r == Reload::None || gx::readOnly()) return;
    static std::map<Reload, std::unique_ptr<gx::Debounce>> pending;
    auto& d = pending[r];
    if (!d) d = std::make_unique<gx::Debounce>(0.3, [r] { runReload(r); });
    (*d)();
}

gx::Key key(const std::string& name, const std::string& group, const std::string& k, const std::string& type,
            const std::string& fallback, Reload r, bool emptyRemoves) {
    auto b = std::make_shared<gx::KeyBackend>();
    b->type = type;
    b->fallback = fallback;
    b->read = [name, group, k] { return read(name, group, k); };
    b->write = [name, group, k, r, emptyRemoves](const std::optional<std::string>& v, gx::WriteDone) {
        return write(name, group, k, emptyRemoves && v && v->empty() ? std::nullopt : v, r);
    };
    b->writable = [name, group, k] { return writable(name, group, k); };
    b->watch = [name, group, k](std::function<void()> fn) { watch(name, group, k, std::move(fn)); };
    return gx::Key(b, k);
}

gx::Key multiKey(const std::string& name, const std::vector<std::string>& groups, const std::string& k, const std::string& type,
                 const std::string& fallback, Reload r) {
    if (groups.empty()) return {};
    auto b = std::make_shared<gx::KeyBackend>();
    b->type = type;
    b->fallback = fallback;
    b->read = [name, g = groups.front(), k] { return read(name, g, k); };
    b->write = [name, groups, k, r](const std::optional<std::string>& v, gx::WriteDone) {
        std::vector<Change> changes;
        for (const auto& g : groups) changes.push_back({g, k, v});
        return writeEntries(name, changes, r);
    };
    b->writable = [name, groups, k] {
        return std::all_of(groups.begin(), groups.end(), [&](const auto& g) { return writable(name, g, k); });
    };
    b->watch = [name, g = groups.front(), k](std::function<void()> fn) { watch(name, g, k, std::move(fn)); };
    return gx::Key(b, k);
}

gx::Key flagKey(const std::string& name, const std::string& group, const std::string& k, const std::string& on,
                const std::optional<std::string>& off, bool fallback, Reload r) {
    auto b = std::make_shared<gx::KeyBackend>();
    b->type = "b";
    b->fallback = fallback ? "true" : "false";
    b->read = [name, group, k, on]() -> std::optional<std::string> {
        auto v = read(name, group, k);
        if (!v) return std::nullopt;
        return *v == on ? "true" : "false";
    };
    b->write = [name, group, k, on, off, r](const std::optional<std::string>& v, gx::WriteDone) {
        if (!v) return write(name, group, k, std::nullopt, r);
        return write(name, group, k, *v == "true" ? std::optional<std::string>(on) : off, r);
    };
    b->writable = [name, group, k] { return writable(name, group, k); };
    b->watch = [name, group, k](std::function<void()> fn) { watch(name, group, k, std::move(fn)); };
    return gx::Key(b, k);
}

gx::Key appliedKey(const std::string& name, const std::string& group, const std::string& k, const std::string& type,
                   const std::string& fallback,
                   std::function<gx::WriteResult(const std::string&, gx::WriteDone)> apply) {
    auto b = std::make_shared<gx::KeyBackend>();
    b->type = type;
    b->fallback = fallback;
    b->read = [name, group, k] { return read(name, group, k); };
    b->write = [apply = std::move(apply), fallback, name](const std::optional<std::string>& v, gx::WriteDone done) {
        if (gx::readOnly()) return gx::WriteResult::readOnly();
        auto complete = [name, done = std::move(done)](gx::WriteResult result) {
            // A tool may have changed part of the file even when it failed. Always re-read the truth.
            reloadFromDisk(name);
            if (done) done(result);
        };
        auto result = apply(v.value_or(fallback), complete);
        if (!result.pending()) reloadFromDisk(name);
        return result;
    };
    b->writable = [name, group, k] { return writable(name, group, k); };
    b->watch = [name, group, k](std::function<void()> fn) { watch(name, group, k, std::move(fn)); };
    return gx::Key(b, k);
}

std::string findTool(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (gx::haveProgram(n)) return n;
        for (const char* dir : {"/usr/lib/libexec", "/usr/libexec", "/usr/lib/x86_64-linux-gnu/libexec", "/usr/lib64/libexec",
                                "/usr/lib/aarch64-linux-gnu/libexec", "/usr/lib"}) {
            const std::string p = std::string(dir) + "/" + n;
            if (::access(p.c_str(), X_OK) == 0) return p;
        }
    }
    return {};
}

}  // namespace kcfg
