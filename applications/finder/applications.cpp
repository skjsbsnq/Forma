#include "applications.hpp"

#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "file_model.hpp"

namespace fs = std::filesystem;

namespace finder {

namespace {

// --- Desktop Entries ---------------------------------------------------------------------------------------

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string unescapeString(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            out.push_back(c == 's' ? ' ' : c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string localeSuffix() {
    const char* lang = std::getenv("LC_ALL");
    if (!lang || !*lang) lang = std::getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = std::getenv("LANG");
    if (!lang || !*lang) return {};
    std::string value(lang);
    const auto dot = value.find('.');
    if (dot != std::string::npos) value.erase(dot);
    const auto at = value.find('@');
    if (at != std::string::npos) value.erase(at);
    for (auto& c : value) c = c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/// The keys of a Desktop Entry file's [Desktop Entry] group, as written ("Name[zh_CN]" included).
using Keys = std::map<std::string, std::string>;

Keys loadDesktopFile(const std::string& path) {
    Keys keys;
    std::ifstream in(path);
    if (!in) return keys;
    bool inMain = false;
    bool first = true;
    std::string line;
    while (std::getline(in, line)) {
        if (first) {  // UTF-8 byte order mark
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            first = false;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            inMain = t == "[Desktop Entry]";
            continue;
        }
        if (!inMain) continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        keys[trim(t.substr(0, eq))] = unescapeString(trim(t.substr(eq + 1)));
    }
    return keys;
}

std::string get(const Keys& keys, const std::string& key) {
    auto it = keys.find(key);
    return it == keys.end() ? std::string() : it->second;
}

/// Name, Icon, …: the key for the user's locale ("Name[zh_CN]") wins over the plain one. The fallback order is
/// the desktop specification's, least specific first so the most specific one ends up winning.
std::string localized(const Keys& keys, const std::string& key) {
    std::string out = get(keys, key);
    const std::string locale = localeSuffix();
    if (locale.empty()) return out;
    auto take = [&](const std::string& k) {
        const std::string v = get(keys, k);
        if (!v.empty()) out = v;
    };
    const auto dash = locale.find('-');
    const std::string language = dash == std::string::npos ? locale : locale.substr(0, dash);
    const std::string rest = dash == std::string::npos ? std::string() : locale.substr(dash + 1);
    const auto sheet = rest.find('@');
    const std::string region = sheet == std::string::npos ? rest : rest.substr(0, sheet);
    const std::string modifier = sheet == std::string::npos ? std::string() : rest.substr(sheet);
    if (language != locale) take(key + "[" + language + "]");
    if (!modifier.empty()) take(key + "[" + language + modifier + "]");
    if (!region.empty()) take(key + "[" + language + "_" + region + "]");
    if (!region.empty() && !modifier.empty()) take(key + "[" + language + "_" + region + modifier + "]");
    return out;
}

std::vector<std::string> list(const Keys& keys, const std::string& key) {
    std::vector<std::string> out;
    std::stringstream ss(get(keys, key));
    std::string item;
    while (std::getline(ss, item, ';')) {
        item = trim(item);
        if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
    return out;
}

std::vector<std::string> dataDirectories() {
    std::vector<std::string> dirs;
    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home) dirs.emplace_back(home);
    else dirs.push_back(homeDirectory() + "/.local/share");
    const char* system = std::getenv("XDG_DATA_DIRS");
    const std::string list = system && *system ? system : "/usr/local/share/:/usr/share/";
    std::stringstream ss(list);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty() || std::find(dirs.begin(), dirs.end(), dir) != dirs.end()) continue;
        dirs.push_back(dir);
    }
    return dirs;
}

std::string userApplicationsDirectory() {
    const char* data = std::getenv("XDG_DATA_HOME");
    return (data && *data ? std::string(data) : homeDirectory() + "/.local/share") + "/applications";
}

bool programOnPath(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::vector<Application> loadApplications() {
    std::vector<Application> apps;
    std::set<std::string> seen;  // ids already taken by a directory higher in the search path
    for (const auto& dir : dataDirectories()) {
        const fs::path base = fs::path(dir) / "applications";
        std::error_code ec;
        if (!fs::is_directory(base, ec)) continue;
        for (const auto& entry : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() != ".desktop") continue;
            if (seen.count(name)) continue;
            seen.insert(name);
            const Keys d = loadDesktopFile(entry.path().string());
            if (get(d, "Type") != "Application") continue;
            if (get(d, "Hidden") == "true" || get(d, "NoDisplay") == "true") continue;
            if (trim(get(d, "Exec")).empty()) continue;
            Application app;
            app.id = name;
            app.path = entry.path().string();
            app.name = localized(d, "Name");
            if (app.name.empty()) app.name = fs::path(name).stem().string();
            app.icon = localized(d, "Icon");
            apps.push_back(std::move(app));
        }
    }
    std::sort(apps.begin(), apps.end(), [](const Application& a, const Application& b) {
        return compareNames(a.name, b.name) < 0;
    });
    return apps;
}

/// Read once per process: a context menu asks for the list on every right click.
const std::vector<Application>& applications() {
    static const std::vector<Application> loaded = loadApplications();
    return loaded;
}

// --- mimeapps.list -----------------------------------------------------------------------------------------

using Associations = std::vector<std::pair<std::string, std::vector<std::string>>>;  // type -> ids

struct MimeAssociations {
    Associations defaults;
    Associations added;
};

MimeAssociations loadAssociations(const std::string& path) {
    MimeAssociations out;
    std::ifstream in(path);
    if (!in) return out;
    std::string section, line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            section = t;
            continue;
        }
        if (section != "[Default Applications]" && section != "[Added Associations]") continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::vector<std::string> ids;
        std::stringstream ss(t.substr(eq + 1));
        std::string id;
        while (std::getline(ss, id, ';')) {
            id = trim(id);
            if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        (section == "[Default Applications]" ? out.defaults : out.added).emplace_back(trim(t.substr(0, eq)), std::move(ids));
    }
    return out;
}

const std::vector<std::string>* lookup(const Associations& associations, const std::string& type) {
    for (const auto& [key, ids] : associations) {
        if (key == type) return &ids;
    }
    return nullptr;
}

/// The user's associations: ~/.config/mimeapps.list, then the $XDG_CONFIG_DIRS ones.
std::vector<MimeAssociations> userAssociations() {
    std::vector<std::string> files;
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        files.push_back(std::string(config) + "/mimeapps.list");
    else
        files.push_back(homeDirectory() + "/.config/mimeapps.list");
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    std::stringstream ss(dirs && *dirs ? dirs : "/etc/xdg");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty()) files.push_back(dir + "/mimeapps.list");
    }
    std::vector<MimeAssociations> out;
    for (const auto& f : files) out.push_back(loadAssociations(f));
    return out;
}

std::string userMimeAppsFile() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    return (config && *config ? std::string(config) : homeDirectory() + "/.config") + "/mimeapps.list";
}

/// The spellings distributors use for this desktop's mimeapps.list (KDE: kde, kde4; GNOME: GNOME, gnome).
std::vector<std::string> desktopFileNames() {
    const char* current = std::getenv("XDG_CURRENT_DESKTOP");
    std::string name = current ? current : "";
    if (!name.empty()) {
        std::stringstream ss(name);
        std::string first;
        std::getline(ss, first, ':');
        // "X-Cinnamon" is Cinnamon, "GNOME-Classic" is GNOME; distributors ship both cases.
        const auto dash = first.find('-');
        if (dash != std::string::npos) first = first.substr(0, dash);
        std::string lowerName = first;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<std::string> names{first};
        if (lowerName != first) names.push_back(lowerName);
        return names;
    }
    for (const char* marker : {"GNOME_DESKTOP_SESSION_ID", "KDE_FULL_SESSION", "PLASMA_SESSION", "XFCE4_SESSION"}) {
        if (const char* v = std::getenv(marker); v && *v) {
            const std::string m = marker;
            if (m.rfind("GNOME", 0) == 0) return {"GNOME", "gnome"};
            if (m.rfind("KDE", 0) == 0 || m.rfind("PLASMA", 0) == 0) return {"KDE", "kde", "plasma"};
            if (m.rfind("XFCE", 0) == 0) return {"X-XFCE", "xfce"};
        }
    }
    return {};
}

/// The default the specification resolves: the user's files first, then the desktop's own defaults
/// ($desktop-mimeapps.list and applications/mimeapps.list, each section in order).
std::string resolveDefaultId(const std::string& type) {
    for (const auto& file : userAssociations()) {
        const auto* ids = lookup(file.defaults, type);
        if (ids && !ids->empty()) return ids->front();
    }
    // XDG_CONFIG_DIRS/mimeapps.list (e.g. /etc/xdg/mimeapps.list), before the per-application ones.
    const char* configDirs = std::getenv("XDG_CONFIG_DIRS");
    std::stringstream ss(configDirs && *configDirs ? configDirs : "/etc/xdg");
    std::string configDir;
    std::vector<std::string> paths{userApplicationsDirectory() + "/mimeapps.list"};
    while (std::getline(ss, configDir, ':')) {
        if (!configDir.empty()) paths.push_back(configDir + "/mimeapps.list");
    }
    // The desktop's own file comes first in its directory, then the distributor's generic one. Distributor
    // files use every spelling of the desktop's name: KDE ships kde-mimeapps.list and kde4-mimeapps.list,
    // GNOME GNOME-mimeapps.list and gnome-mimeapps.list.
    // The current desktop's own file first, then the other spellings distributors use, then the generic one.
    std::vector<std::string> desktops = desktopFileNames();
    for (const char* other : {"GNOME", "GNOME-GNOMELIKE", "gnome", "KDE", "kde", "kde4", "plasma", "UNITY", "Unity",
                              "xfce", "X-XFCE"}) {
        if (std::find(desktops.begin(), desktops.end(), other) == desktops.end()) desktops.emplace_back(other);
    }
    for (const auto& dir : dataDirectories()) {
        for (const auto& desktop : desktops) paths.push_back(dir + "/applications/" + desktop + "-mimeapps.list");
        paths.push_back(dir + "/applications/mimeapps.list");
    }
    for (const auto& path : paths) {
        const MimeAssociations file = loadAssociations(path);
        if (const auto* ids = lookup(file.defaults, type); ids && !ids->empty()) return ids->front();
    }
    return {};
}

/// Rewrites the [Default Applications] entry of `type` in mimeapps.list's text, keeping comments, the other
/// sections and keys. An empty `id` removes the entry.
std::string rewriteMimeApps(const std::string& text, const std::string& type, const std::string& id) {
    std::vector<std::string> lines;
    {
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }
    const std::string section = "[Default Applications]";
    const std::string entry = type + "=" + id + ";";
    int sectionAt = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == section) {
            sectionAt = static_cast<int>(i);
            break;
        }
    }
    bool written = false;
    if (sectionAt >= 0) {
        size_t i = static_cast<size_t>(sectionAt) + 1;
        for (; i < lines.size() && !(!lines[i].empty() && lines[i][0] == '['); ++i) {
            const auto eq = lines[i].find('=');
            if (eq == std::string::npos || trim(lines[i].substr(0, eq)) != type) continue;
            if (id.empty()) lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            else lines[i] = entry;
            written = true;
            break;
        }
        if (!written && !id.empty()) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i), entry);
    } else if (!id.empty()) {
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.push_back(section);
        lines.push_back(entry);
    }
    std::string out;
    for (const auto& line : lines) out += line + "\n";
    return out;
}

/// Single-quoted for the shell (a path may contain anything).
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    return out + "'";
}

// --- MIME types --------------------------------------------------------------------------------------------

constexpr size_t kMimeHead = 8192;  // bytes of content the rules below look at

std::string lowerExtension(const fs::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

const std::map<std::string, std::string>& mimeNameTable() {
    static const std::map<std::string, std::string> table = {
        {"png", "image/png"},        {"jpg", "image/jpeg"},       {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},        {"webp", "image/webp"},      {"bmp", "image/bmp"},
        {"tga", "image/x-tga"},      {"tif", "image/tiff"},       {"tiff", "image/tiff"},
        {"heic", "image/heic"},      {"avif", "image/avif"},      {"ico", "image/vnd.microsoft.icon"},
        {"svg", "image/svg+xml"},    {"psd", "image/vnd.adobe.photoshop"}, {"xcf", "image/x-xcf"},
        {"mp4", "video/mp4"},        {"m4v", "video/mp4"},        {"mkv", "video/matroska"},
        {"webm", "video/webm"},      {"mov", "video/quicktime"},  {"avi", "video/msvideo"},
        {"wmv", "video/x-ms-wmv"},   {"flv", "video/x-flv"},      {"mpeg", "video/mpeg"},
        {"mpg", "video/mpeg"},       {"ts", "video/mp2t"},        {"3gp", "video/3gpp"},
        {"mp3", "audio/mpeg"},       {"flac", "audio/flac"},      {"wav", "audio/x-wav"},
        {"ogg", "audio/ogg"},        {"opus", "audio/ogg"},       {"m4a", "audio/mp4"},
        {"aac", "audio/aac"},        {"aif", "audio/x-aiff"},     {"aiff", "audio/x-aiff"},
        {"mka", "audio/matroska"}, {"pdf", "application/pdf"},  {"txt", "text/plain"},
        {"md", "text/markdown"},     {"rst", "text/x-rst"},       {"log", "text/x-log"},
        {"json", "application/json"}, {"xml", "application/xml"}, {"yaml", "application/yaml"},
        {"yml", "application/yaml"}, {"toml", "application/toml"}, {"ini", "text/plain"},
        {"conf", "text/plain"},      {"cfg", "text/plain"},       {"desktop", "application/x-desktop"},
        {"srt", "application/x-subrip"}, {"vtt", "text/vtt"},     {"c", "text/x-csrc"},
        {"h", "text/x-chdr"},        {"cpp", "text/x-c++src"},    {"cc", "text/x-c++src"},
        {"cxx", "text/x-c++src"},    {"hpp", "text/x-c++hdr"},    {"py", "text/x-python"},
        {"js", "text/javascript"},   {"mjs", "text/javascript"},  {"ts", "application/typescript"},
        {"tsx", "application/typescript"}, {"jsx", "text/javascript"}, {"rs", "text/rust"},
        {"go", "text/x-go"},         {"java", "text/x-java"},     {"kt", "text/x-kotlin"},
        {"swift", "text/x-swift"},   {"cs", "text/x-csharp"},     {"rb", "application/x-ruby"},
        {"lua", "text/x-lua"},       {"sh", "text/x-shellscript"}, {"bash", "text/x-shellscript"},
        {"zsh", "text/x-shellscript"}, {"fish", "text/x-shellscript"},
        {"cmake", "text/x-cmake"},   {"glsl", "text/x-glsl"},     {"css", "text/css"},
        {"sql", "application/sql"},  {"csv", "text/csv"},         {"rtf", "application/rtf"},
        {"html", "text/html"},       {"htm", "text/html"},        {"zip", "application/zip"},
        {"tar", "application/x-tar"}, {"gz", "application/gzip"}, {"xz", "application/x-xz"},
        {"bz2", "application/x-bzip2"}, {"zst", "application/zstd"}, {"7z", "application/x-7z-compressed"},
        {"rar", "application/x-rar"}, {"deb", "application/vnd.debian.binary-package"},
        {"rpm", "application/x-rpm"}, {"jar", "application/java-archive"},
        {"iso", "application/x-iso9660-image"}, {"img", "application/x-raw-disk-image"},
        {"qcow2", "application/x-qemu-disk"}, {"ttf", "font/ttf"}, {"otf", "font/otf"},
        {"woff", "font/woff"},       {"woff2", "font/woff2"},     {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"odt", "application/vnd.oasis.opendocument.text"}, {"epub", "application/epub+zip"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"ods", "application/vnd.oasis.opendocument.spreadsheet"}, {"ppt", "application/vnd.ms-powerpoint"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odp", "application/vnd.oasis.opendocument.presentation"},
        {"appimage", "application/x-executable"}, {"exe", "application/vnd.microsoft.portable-executable"},
        {"torrent", "application/x-bittorrent"}, {"ics", "text/calendar"},
    };
    return table;
}

/// Text that is valid UTF-8 with no NUL bytes in its first kilobyte.
bool looksLikeText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string head(1024, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    if (head.find('\0') != std::string::npos) return false;
    int bad = 0;  // malformed UTF-8 sequences; a Latin-1 log is still text
    for (size_t i = 0; i < head.size();) {
        const auto c = static_cast<unsigned char>(head[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        bool ok = extra > 0 && i + static_cast<size_t>(extra) < head.size();
        for (int k = 1; ok && k <= extra; ++k)
            ok = (static_cast<unsigned char>(head[i + static_cast<size_t>(k)]) & 0xC0) == 0x80;
        if (!ok) {
            ++bad;
            ++i;
            continue;
        }
        i += static_cast<size_t>(extra) + 1;
    }
    return bad * 10 <= static_cast<int>(head.size());
}

/// Content rules for the types a file manager meets, in shared-mime-info's order (the specific beats the
/// generic: an OOXML package before a zip, an ELF shared object before an ELF program).
std::string mimeFromContent(const std::string& path, const std::string& ext) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string head(kMimeHead, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    auto at = [&](size_t offset) -> unsigned {
        return offset < head.size() ? static_cast<unsigned char>(head[offset]) : 0u;
    };
    auto has = [&](size_t offset, std::string_view bytes) {
        return offset + bytes.size() <= head.size() && std::string_view(head).substr(offset, bytes.size()) == bytes;
    };
    if (has(0, "\x7f" "ELF")) {
        // e_type at offset 16 (byte 5 is the endianness): 3 DYN is a shared object or a position-independent
        // program, 2 EXEC a classic program, 1 REL an object file.
        const bool little = at(5) == 1;
        const unsigned type = little ? at(16) | at(17) << 8 : at(16) << 8 | at(17);
        return type == 3 ? "application/x-sharedlib" : "application/x-executable";
    }
    if (has(0, "MZ")) return "application/vnd.microsoft.portable-executable";
    if (has(0, "%PDF-")) return "application/pdf";
    if (has(0, "\x89PNG\r\n\x1a\n")) return "image/png";
    if (has(0, "\xff\xd8\xff")) return "image/jpeg";
    if (has(0, "GIF87a") || has(0, "GIF89a")) return "image/gif";
    if (has(0, "RIFF") && has(8, "WEBP")) return "image/webp";
    if (has(0, "RIFF") && has(8, "WAVE")) return "audio/x-wav";
    if (has(0, "II*\0") || has(0, "MM\0*")) return "image/tiff";
    if (has(4, "ftypavif")) return "image/avif";
    if (has(4, "ftypheic") || has(4, "ftypheix") || has(4, "ftypmif1")) return "image/heic";
    if (has(4, "ftypqt")) return "video/quicktime";
    if (has(4, "ftyp")) return "video/mp4";
    if (has(0, "\x1a\x45\xdf\xa3")) return "video/matroska";
    if (has(0, "ID3") || (at(0) == 0xff && (at(1) & 0xE0) == 0xE0)) return "audio/mpeg";
    if (has(0, "fLaC")) return "audio/flac";
    if (has(0, "OggS")) return "audio/ogg";
    if (has(0, "PK\x03\x04") || has(0, "PK\x05\x06")) {
        // OOXML and OpenDocument are zip containers; their first entries name them.
        if (head.find("mimetypeapplication/vnd.oasis.opendocument.text") != std::string::npos)
            return "application/vnd.oasis.opendocument.text";
        if (head.find("mimetypeapplication/vnd.oasis.opendocument.spreadsheet") != std::string::npos)
            return "application/vnd.oasis.opendocument.spreadsheet";
        if (head.find("mimetypeapplication/vnd.oasis.opendocument.presentation") != std::string::npos)
            return "application/vnd.oasis.opendocument.presentation";
        if (head.find("[Content_Types].xml") != std::string::npos) {
            if (ext == "pptx") return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
            if (ext == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
            return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
        }
        return "application/zip";
    }
    if (has(0, "\x1f\x8b")) return "application/gzip";
    if (has(0, "\xfd" "7zXZ\0")) return "application/x-xz";
    if (has(0, "7z\xbc\xaf\x27\x1c")) return "application/x-7z-compressed";
    if (has(0, "Rar!\x1a\x07")) return "application/x-rar";
    if (has(0, "BZh")) return "application/x-bzip2";
    if (has(0, "\x28\xb5\x2f\xfd")) return "application/zstd";
    if (has(257, "ustar")) return "application/x-tar";
    if (has(0, "<svg") || (has(0, "<?xml") && head.find("<svg") != std::string::npos)) return "image/svg+xml";
    if (has(0, "{\\rtf")) return "application/rtf";
    if (has(0, "#!")) {
        const std::string line = head.substr(0, head.find('\n'));
        if (line.find("bin/sh") != std::string::npos || line.find("bash") != std::string::npos ||
            line.find("zsh") != std::string::npos || line.find("fish") != std::string::npos)
            return "text/x-shellscript";
        if (line.find("python") != std::string::npos) return "text/x-python";
        if (line.find("perl") != std::string::npos) return "text/x-perl";
        if (line.find("ruby") != std::string::npos) return "application/x-ruby";
        if (line.find("node") != std::string::npos) return "text/javascript";
        return "text/x-script";
    }
    return {};
}

/// Globs of the shared MIME database (its compiled index if present, else the XML fragments left by
/// update-mime-database), plus our own list so a missing database still types the common files.
const std::vector<std::pair<std::string, std::string>>& mimeGlobs() {
    static const std::vector<std::pair<std::string, std::string>> globs = [] {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
        for (const auto& dir : dataDirectories()) {
            const fs::path root = fs::path(dir) / "mime";
            std::ifstream index(root / "globs2");
            if (index) {
                std::string line;
                while (std::getline(index, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    const auto first = line.find(':');
                    const auto second = first == std::string::npos ? first : line.find(':', first + 1);
                    if (second == std::string::npos) continue;
                    std::string pattern = line.substr(second + 1);
                    std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    out.emplace_back(line.substr(first + 1, second - first - 1), std::move(pattern));
                }
                continue;
            }
            for (const auto& base : {root / "freedesktop.org", root / "packages"}) {
                if (!fs::is_directory(base, ec)) continue;
                for (const auto& entry : fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
                    if (entry.path().extension() != ".xml") continue;
                    std::ifstream in(entry.path());
                    const std::string xml{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                    size_t at = 0;
                    while ((at = xml.find("<mime-type type=\"", at)) != std::string::npos) {
                        at += 17;
                        const std::string type = xml.substr(at, xml.find('"', at) - at);
                        const size_t end = xml.find("</mime-type>", at);
                        if (end == std::string::npos) break;
                        size_t g = at;
                        while ((g = xml.find("<glob pattern=\"", g)) != std::string::npos && g < end) {
                            g += 15;
                            const size_t close = xml.find('"', g);
                            std::string pattern = xml.substr(g, close - g);
                            std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            out.emplace_back(type, std::move(pattern));
                            g = close;
                        }
                        at = end;
                    }
                }
            }
        }
        // Always there, database or not, and first: a stale database can't outvote the obvious names.
        static const char* const builtins[][2] = {
            {"image/png", "*.png"},   {"image/jpeg", "*.jpg"},  {"image/jpeg", "*.jpeg"}, {"image/gif", "*.gif"},
            {"image/webp", "*.webp"}, {"image/bmp", "*.bmp"},   {"image/svg+xml", "*.svg"}, {"image/tiff", "*.tiff"},
            {"image/avif", "*.avif"}, {"image/heic", "*.heic"}, {"video/mp4", "*.mp4"},   {"video/matroska", "*.mkv"},
            {"video/webm", "*.webm"}, {"video/quicktime", "*.mov"}, {"video/msvideo", "*.avi"},
            {"audio/mpeg", "*.mp3"},  {"audio/flac", "*.flac"},  {"audio/x-wav", "*.wav"}, {"audio/ogg", "*.ogg"},
            {"audio/ogg", "*.opus"},  {"audio/mp4", "*.m4a"},    {"application/pdf", "*.pdf"}, {"text/plain", "*.txt"},
            {"text/markdown", "*.md"}, {"application/json", "*.json"}, {"application/xml", "*.xml"},
            {"application/yaml", "*.yaml"}, {"application/yaml", "*.yml"}, {"application/toml", "*.toml"},
            {"application/x-desktop", "*.desktop"}, {"application/x-subrip", "*.srt"},
            {"text/x-csrc", "*.c"},   {"text/x-chdr", "*.h"},    {"text/x-c++src", "*.cpp"}, {"text/x-c++src", "*.cc"},
            {"text/x-c++hdr", "*.hpp"}, {"text/x-python", "*.py"}, {"text/javascript", "*.js"},
            {"application/typescript", "*.ts"}, {"application/typescript", "*.tsx"}, {"text/rust", "*.rs"},
            {"text/x-go", "*.go"},    {"text/x-java", "*.java"}, {"text/x-kotlin", "*.kt"},  {"text/x-swift", "*.swift"},
            {"text/x-csharp", "*.cs"}, {"application/x-ruby", "*.rb"}, {"text/x-lua", "*.lua"},
            {"text/x-shellscript", "*.sh"}, {"text/x-shellscript", "*.bash"},
            {"text/x-shellscript", "*.zsh"}, {"text/x-shellscript", "*.fish"},
            {"text/x-cmake", "*.cmake"}, {"text/css", "*.css"},  {"application/sql", "*.sql"},
            {"text/html", "*.html"},  {"text/html", "*.htm"},    {"application/zip", "*.zip"},
            {"application/x-tar", "*.tar"}, {"application/gzip", "*.gz"}, {"application/x-compressed-tar", "*.tar.gz"},
            {"application/x-xz", "*.xz"}, {"application/x-xz-compressed-tar", "*.tar.xz"},
            {"application/x-bzip2", "*.bz2"}, {"application/zstd", "*.zst"}, {"application/x-7z-compressed", "*.7z"},
            {"application/vnd.debian.binary-package", "*.deb"}, {"application/x-rpm", "*.rpm"},
            {"application/java-archive", "*.jar"}, {"application/x-iso9660-image", "*.iso"},
            {"font/ttf", "*.ttf"},    {"font/otf", "*.otf"},     {"font/woff", "*.woff"},  {"font/woff2", "*.woff2"},
            {"application/msword", "*.doc"},
            {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "*.docx"},
            {"application/vnd.oasis.opendocument.text", "*.odt"}, {"application/epub+zip", "*.epub"},
            {"application/vnd.ms-excel", "*.xls"},
            {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "*.xlsx"},
            {"application/vnd.oasis.opendocument.spreadsheet", "*.ods"}, {"text/csv", "*.csv"},
            {"application/vnd.ms-powerpoint", "*.ppt"},
            {"application/vnd.openxmlformats-officedocument.presentationml.presentation", "*.pptx"},
            {"application/vnd.oasis.opendocument.presentation", "*.odp"},
        };
        for (const auto& b : builtins) out.insert(out.begin(), {b[0], b[1]});
        return out;
    }();
    return globs;
}

/// The rules without an external tool. The content decides first (an ELF is a program whatever its name),
/// then the shared MIME database's globs learned from the files it already knows, then our own name table as a
/// last resort. A name from the table only claims a *text* type — everything else goes by content, so a ".txt"
/// holding a picture is still a picture.
std::string localMimeType(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return "application/octet-stream";
    if (S_ISDIR(st.st_mode)) return "inode/directory";
    if (!S_ISREG(st.st_mode)) return "inode/special-file";
    const fs::path p(path);
    const std::string name = p.filename().string();
    const std::string ext = lowerExtension(p);
    if (const std::string byContent = mimeFromContent(path, ext); !byContent.empty()) return byContent;
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& [type, pattern] : mimeGlobs()) {
        if (pattern == "*" || pattern.empty()) continue;
        if (::fnmatch(pattern.c_str(), lowered.c_str(), 0) == 0) {
            if (type.rfind("text/", 0) == 0) {
                // Some bases type every text file with a name they know; bytes that don't look like text win.
                return looksLikeText(path) ? type : "application/octet-stream";
            }
            return type;
        }
    }
    for (const char* special : {"Makefile", "Dockerfile", "PKGBUILD", "CMakeLists.txt", "LICENSE", "AUTHORS"}) {
        if (name == special) return "text/plain";
    }
    if (auto it = mimeNameTable().find(ext); it != mimeNameTable().end()) {
        if (it->second.rfind("text/", 0) != 0 || looksLikeText(path)) return it->second;
        return "application/octet-stream";
    }
    if (looksLikeText(path)) return "text/plain";
    return "application/octet-stream";
}

/// Type of `path` as the shared MIME database sees it. xdg-mime answers when it is installed (it starts a
/// process, so answers are cached — a file manager asks for a folder's worth of items at once). What it says
/// about an extension is remembered: demo runs and systems without it then classify the next file of that kind
/// the same way instead of guessing.
bool& inProcessOnlyFlag() {
    static bool on = std::getenv("MAYO_DEMO") != nullptr;
    return on;
}

void setInProcessOnly(bool on) { inProcessOnlyFlag() = on; }

std::string mimeTypeForCached(const std::string& path) {
    static std::map<std::string, std::string> cache;
    static std::map<std::string, std::string> learnedByExtension;
    const bool inProcessOnly = inProcessOnlyFlag();
    if (auto it = cache.find(path); it != cache.end()) return it->second;
    std::string type;
    if (!inProcessOnly && programOnPath("xdg-mime")) {
        if (FILE* pipe = ::popen(("xdg-mime query filetype " + shellQuote(path) + " 2>/dev/null").c_str(), "r")) {
            char buf[256] = {};
            if (std::fgets(buf, sizeof buf, pipe)) {
                type.assign(buf);
                while (!type.empty() && (type.back() == '\n' || type.back() == '\r')) type.pop_back();
            }
            ::pclose(pipe);
        }
    }
    const std::string ext = lowerExtension(path);
    if (type.empty() || type.find('/') == std::string::npos) {
        // xdg-mime also fails without a display. A remembered answer may be used only for a plain file with
        // that extension — an executable copied into "photo.jpg" is still a program.
        struct stat st {};
        const bool regular = ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) == 0;
        auto learned = learnedByExtension.find(ext);
        if (regular && !ext.empty() && learned != learnedByExtension.end() && learned->second != "application/octet-stream") {
            type = learned->second;
        } else {
            type = localMimeType(path);
        }
    } else if (!ext.empty()) {
        learnedByExtension.emplace(ext, type);
    }
    cache[path] = type;
    return type;
}

// --- Desktop defaults --------------------------------------------------------------------------------------

// --- What opens an item ------------------------------------------------------------------------------------

/// What an entry claims for the item: the type itself, something of the same kind, or nothing.
enum class Claim : std::uint8_t { None, Kind, Exact };

/// Names of MIME types that open "this kind of file" (a picture, a video, …) when nothing types it exactly.
std::vector<std::string> kindTypes(FileKind kind) {
    switch (kind) {
        case FileKind::Image: return {"image/png", "image/jpeg", "image/gif", "image/webp"};
        case FileKind::Video: return {"video/mp4", "video/matroska", "video/webm"};
        case FileKind::Audio: return {"audio/mpeg", "audio/flac", "audio/ogg", "audio/x-wav"};
        case FileKind::Pdf: return {"application/pdf"};
        case FileKind::Text:
        case FileKind::Code:
        case FileKind::Unknown: return {"text/plain"};
        case FileKind::Web: return {"text/html"};
        case FileKind::Archive: return {"application/zip", "application/x-tar"};
        case FileKind::Executable: return {"application/x-executable", "application/x-msdos-program"};
        case FileKind::Document: return {"application/msword", "application/vnd.oasis.opendocument.text", "text/plain"};
        case FileKind::Spreadsheet: return {"text/csv", "application/vnd.oasis.opendocument.spreadsheet"};
        case FileKind::Presentation: return {"application/vnd.oasis.opendocument.presentation"};
        case FileKind::DiskImage: return {"application/x-iso9660-image"};
        case FileKind::Font: return {"font/ttf"};
        case FileKind::Folder: return {};
    }
    return {};
}

Claim claimFor(const std::vector<std::string>& takes, const std::string& type, const std::string& ext,
               const std::vector<std::string>& kind) {
    auto contains = [](const std::vector<std::string>& v, const std::string& t) {
        return std::find(v.begin(), v.end(), t) != v.end();
    };
    if (contains(takes, type)) return Claim::Exact;
    // Entries sometimes type "image/jpg" or "text/x-txt" for the extension they know best.
    if (!ext.empty()) {
        for (const auto& t : takes) {
            if (t.size() > ext.size() + 1 && t.compare(t.size() - ext.size() - 1, ext.size() + 1, "/" + ext) == 0)
                return Claim::Exact;
        }
    }
    const auto slash = type.find('/');
    const std::string media = type.substr(0, slash);
    for (const auto& t : takes) {
        if (t == media + "/*" || contains(kind, t)) return Claim::Kind;
    }
    return Claim::None;
}

// --- Launching helpers -------------------------------------------------------------------------------------

/// Splits an Exec line into argv (Desktop Entry quoting) with %f/%u and %F/%U expanded for `args`. Also reports
/// whether the line takes a file at all — an entry without such a placeholder can't be handed one.
std::vector<std::string> splitExec(const std::string& line, const std::vector<std::string>& args, bool* tookFile) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    bool quoted = false;
    bool took = false;
    const bool needed = !args.empty();
    auto flush = [&] {
        if (have) out.push_back(cur);
        cur.clear();
        have = false;
    };
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '\\' && i + 1 < line.size()) {
                cur.push_back(line[++i]);
                have = true;
            } else if (c == '"') {
                quoted = false;
            } else if (c == '%' && i + 1 < line.size()) {
                i++;  // %f inside a quoted argument
                have = true;
            } else {
                cur.push_back(c);
                have = true;
            }
            continue;
        }
        if (c == ' ' || c == '\t') {
            flush();
            continue;
        }
        if (c == '"') {
            quoted = true;
            have = true;
            continue;
        }
        if (c == '\\' && i + 1 < line.size()) {
            cur.push_back(line[++i]);
            have = true;
            continue;
        }
        if (c == '%' && i + 1 < line.size()) {
            const char code = line[++i];
            switch (code) {
                case 'f':
                case 'u':
                    took = true;
                    if (needed) {
                        cur += args.front();
                        have = true;
                    }
                    break;
                case 'F':
                case 'U':
                    took = true;
                    if (needed) {
                        for (size_t a = 0; a < args.size(); ++a) {
                            if (a) cur.push_back(' ');
                            cur += args[a];
                        }
                        have = true;
                    }
                    break;
                case '%': cur.push_back('%'); have = true; break;
                default: break;  // %i, %c, %k and the deprecated ones
            }
            continue;
        }
        cur.push_back(c);
        have = true;
    }
    flush();
    if (tookFile) *tookFile = took;
    return out;
}

std::string fileUri(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-_.~", static_cast<char>(c))) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

/// Distraction-free stdio for launched programs: they shouldn't scribble into ours.
void redirectToDevNull() {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull < 0) return;
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDOUT_FILENO);
    ::dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) ::close(devnull);
}

/// Terminal applications (Terminal=true) get a terminal around them. The flag differs between emulators: a
/// bare "-e" (or GNOME's old "-x", or a bare "--") introduces the program.
bool wrapInTerminal(const std::vector<std::string>& command, std::string* error) {
    if (command.empty()) return false;
    struct Syntax {
        const char* program;
        std::vector<const char*> prefix;
    };
    const Syntax syntaxes[]{
        {"x-terminal-emulator", {"-e"}}, {"ptyxis", {"-x"}},     {"kgx", {"-e"}},        {"gnome-terminal", {"--"}},
        {"konsole", {"-e"}},             {"kitty", {"-e"}},      {"alacritty", {"-e"}},  {"foot", {"-e"}},
        {"footclient", {"-e"}},          {"wezterm", {"start", "--"}}, {"xfce4-terminal", {"-x"}},
        {"mate-terminal", {"-x"}},       {"tilix", {"-e"}},      {"xterm", {"-e"}},
    };
    for (const auto& syntax : syntaxes) {
        if (!programOnPath(syntax.program)) continue;
        std::vector<std::string> wrapped{syntax.program};
        for (const char* p : syntax.prefix) wrapped.emplace_back(p);
        wrapped.insert(wrapped.end(), command.begin(), command.end());
        return spawnDetachedProcess(wrapped, error);
    }
    if (error) *error = "没有找到可用的终端模拟器，无法运行需要终端的程序。";
    return false;
}

/// Desktop entries whose program isn't installed can't be started: check the program before forking.
std::string missingProgram(const std::string& program) {
    if (program.empty()) return "（空）";
    if (program.find('/') != std::string::npos) return ::access(program.c_str(), X_OK) == 0 ? std::string() : program;
    return programOnPath(program) ? std::string() : program;
}

}  // namespace

// --- Applications ------------------------------------------------------------------------------------------

const std::vector<Application>& installedApplications() { return applications(); }

const Application* findApplication(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& app : applications()) {
        if (app.id == id) return &app;
    }
    return nullptr;
}

std::string applicationDisplayName(const std::string& desktopId) {
    if (const Application* app = findApplication(desktopId)) return app->name;
    return desktopId.empty() ? std::string() : fs::path(desktopId).stem().string();
}

std::string applicationIcon(const std::string& desktopId) {
    const Application* app = findApplication(desktopId);
    return app ? app->icon : std::string();
}

// --- MIME ---------------------------------------------------------------------------------------------------

std::string mimeTypeFor(const std::string& path) { return mimeTypeForCached(path); }

void classifyInProcessOnly(bool on) { setInProcessOnly(on); }

std::string mimeTypeLabel(const std::string& type) {
    static const std::map<std::string, std::string> labels = {
        {"image/png", "PNG 图像"}, {"image/jpeg", "JPEG 图像"}, {"image/gif", "GIF 图像"},
        {"image/webp", "WebP 图像"}, {"image/bmp", "BMP 图像"}, {"image/tiff", "TIFF 图像"},
        {"image/svg+xml", "SVG 图像"}, {"image/heic", "HEIC 图像"}, {"image/avif", "AVIF 图像"},
        {"video/mp4", "MPEG-4 视频"}, {"video/matroska", "Matroska 视频"}, {"video/webm", "WebM 视频"},
        {"video/quicktime", "QuickTime 视频"}, {"video/msvideo", "AVI 视频"}, {"video/mpeg", "MPEG 视频"},
        {"audio/mpeg", "MP3 音频"}, {"audio/flac", "FLAC 音频"}, {"audio/x-wav", "WAV 音频"},
        {"audio/ogg", "Ogg 音频"}, {"audio/mp4", "MPEG-4 音频"}, {"application/pdf", "PDF 文档"},
        {"text/plain", "纯文本"}, {"text/markdown", "Markdown 文档"}, {"text/html", "HTML 文档"},
        {"application/json", "JSON 文档"}, {"application/xml", "XML 文档"}, {"application/zip", "ZIP 压缩包"},
        {"application/x-tar", "tar 压缩包"}, {"application/gzip", "gzip 压缩包"}, {"application/x-txz", "XZ 压缩包"},
        {"application/x-xz", "XZ 压缩包"}, {"application/x-7z-compressed", "7-Zip 压缩包"},
        {"inode/directory", "文件夹"},
    };
    if (auto it = labels.find(type); it != labels.end()) return it->second;
    return {};
}

// --- Defaults -----------------------------------------------------------------------------------------------

std::string defaultApplicationId(const std::string& path) {
    const std::string id = resolveDefaultId(mimeTypeFor(path));
    return !id.empty() && findApplication(id) ? id : std::string();
}

std::string defaultApplicationProblem(const std::string& path, const std::string& appId) {
    if (appId.empty()) return {};
    const Application* app = findApplication(appId);
    if (!app) return "“" + applicationDisplayName(appId) + "”没有安装。";
    const std::string type = mimeTypeFor(path);
    if (type == "inode/directory") return "不能把应用程序设为文件夹的默认打开方式。";
    const auto choices = applicationsFor(path);
    if (std::find(choices.begin(), choices.end(), appId) == choices.end())
        return "“" + app->name + "”不在“打开方式”中处理这类文件，设为默认后它无法打开它们。";
    if (defaultApplicationId(path) == appId) return {};  // already the default
    // A distributor can pin the type in one of its own files; then mimeapps.list is shadowed for everyone,
    // whatever we write.
    for (const auto& desktopFile : desktopFileNames()) {
        for (const auto& dir : dataDirectories()) {
            const std::string file = dir + "/applications/" + desktopFile + "-mimeapps.list";
            if (::access(file.c_str(), R_OK) != 0) continue;
            const MimeAssociations associations = loadAssociations(file);
            const auto* ids = lookup(associations.defaults, type);
            if (ids && !ids->empty() && ids->front() != appId)
                return "系统的“" + desktopFile + "-mimeapps.list”把这类文件固定交给了“" + applicationDisplayName(ids->front()) +
                       "”，写入的选择不会生效。";
        }
    }
    return {};
}

bool pickBecomesDefault(const std::string& path, const std::string& appId) { return defaultApplicationId(path) == appId; }

bool setDefaultApplication(const std::string& path, const std::string& appId) {
    const std::string type = mimeTypeFor(path);
    if (type == "inode/directory") return false;
    if (!appId.empty()) {
        if (!findApplication(appId)) return false;
        if (!defaultApplicationProblem(path, appId).empty()) return false;
    }
    std::error_code ec;
    const std::string file = userMimeAppsFile();
    fs::create_directories(fs::path(file).parent_path(), ec);
    std::string text;
    {
        std::ifstream in(file);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string updated = rewriteMimeApps(text, type, appId);
    if (updated == text) return true;
    const std::string temp = file + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return false;
        out << updated;
        if (!out) return false;
    }
    if (std::rename(temp.c_str(), file.c_str()) != 0) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
    }
    return true;
}

// --- Applications that open an item -------------------------------------------------------------------------

std::vector<std::string> applicationsFor(const std::string& path) {
    // The folder is opened by whatever handles folders; "Open in <editor>" would only fail or open the wrong
    // thing, so folders have no "Open With" list.
    const std::string type = mimeTypeFor(path);
    if (type == "inode/directory") return {};
    const std::string ext = lowerExtension(fs::path(path));
    const std::string defaultId = defaultApplicationId(path);
    const std::vector<std::string> kind = kindTypes(kindForExtension(ext));
    std::vector<std::pair<std::string, std::string>> exact, similar;
    for (const auto& app : applications()) {
        const Keys d = loadDesktopFile(app.path);
        bool takesFile = false;
        (void)splitExec(get(d, "Exec"), {"x"}, &takesFile);
        if (!takesFile) continue;
        const std::vector<std::string> takes = list(d, "MimeType");
        switch (claimFor(takes, type, ext, kind)) {
            case Claim::Exact: exact.emplace_back(app.id, app.name); break;
            case Claim::Kind: similar.emplace_back(app.id, app.name); break;
            case Claim::None: break;
        }
    }
    auto byName = [](const auto& a, const auto& b) { return compareNames(a.second, b.second) < 0; };
    std::sort(exact.begin(), exact.end(), byName);
    std::sort(similar.begin(), similar.end(), byName);
    std::vector<std::string> out;
    auto push = [&](const std::string& id) {
        if (!id.empty() && findApplication(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    push(defaultId);  // whatever opens these items now comes first
    for (const auto& [id, name] : exact) {
        if (id != defaultId) push(id);
    }
    // Even an unknown type has somebody to open it with (as plain text, or as a picture); three at most, so the
    // list stays a choice rather than a catalogue.
    size_t added = 0;
    for (const auto& [id, name] : similar) {
        if (out.size() >= 8 || added >= 3) break;
        if (std::find(out.begin(), out.end(), id) != out.end()) continue;
        push(id);
        ++added;
    }
    return out;
}

// --- Launching ----------------------------------------------------------------------------------------------

std::string launchCommand(const std::string& appId, const std::string& path, std::vector<std::string>* out,
                          std::string* error) {
    if (out) out->clear();
    const Application* app = findApplication(appId);
    if (!app) {
        if (error) *error = "找不到“" + applicationDisplayName(appId) + "”的桌面文件。";
        return {};
    }
    const Keys d = loadDesktopFile(app->path);
    const std::string exec = get(d, "Exec");
    if (trim(exec).empty()) {
        if (error) *error = "“" + app->name + "”的桌面文件没有 Exec 行。";
        return {};
    }
    // The program is handed the path for %f/%F entries and a file:// URI for %u/%U, as the entry asks.
    const bool uri = exec.find("%U") != std::string::npos || exec.find("%u") != std::string::npos;
    std::vector<std::string> command = splitExec(exec, {uri ? fileUri(path) : path}, nullptr);
    if (command.empty()) {
        if (error) *error = "无法解析“" + app->name + "”的 Exec 行。";
        return {};
    }
    if (out) *out = command;
    return command.front();
}

bool spawnDetachedProcess(const std::vector<std::string>& argv, std::string* error) { return spawnDetachedIn(argv, {}, error); }

bool spawnDetachedIn(const std::vector<std::string>& argv, const std::string& cwd, std::string* error) {
    if (argv.empty()) return false;
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    // Dual fork: the middle process exits at once and is reaped here, the grandchild runs the program (never a
    // zombie of ours). A close-on-exec pipe carries its errno back if exec fails.
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) fds[0] = fds[1] = -1;
    const pid_t child = ::fork();
    if (child == 0) {
        ::setsid();
        const pid_t grandchild = ::fork();
        if (grandchild != 0) ::_exit(grandchild < 0 ? 126 : 0);
        if (fds[0] >= 0) ::close(fds[0]);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            const int e = errno;
            if (fds[1] >= 0) (void)!::write(fds[1], &e, sizeof e);
            ::_exit(127);
        }
        redirectToDevNull();
        ::execvp(args[0], args.data());
        const int e = errno;
        if (fds[1] >= 0) {
            (void)!::write(fds[1], &e, sizeof e);
            ::_exit(127);
        }
        ::_exit(127);
    }
    if (fds[1] >= 0) ::close(fds[1]);
    int execErrno = 0;
    if (fds[0] >= 0) {
        ssize_t n;
        do {
            n = ::read(fds[0], &execErrno, sizeof execErrno);
        } while (n < 0 && errno == EINTR);
        ::close(fds[0]);
        if (n != static_cast<ssize_t>(sizeof execErrno)) execErrno = 0;
    }
    if (child > 0) {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) == 126))
            execErrno = execErrno ? execErrno : ENOENT;
    } else if (child < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    if (execErrno != 0) {
        if (error) *error = std::string("找不到程序“") + argv.front() + "”（" + std::strerror(execErrno) + "）";
        return false;
    }
    return true;
}

bool launchApplication(const std::string& appId, const std::string& path, std::string* error) {
    const Application* app = findApplication(appId);
    std::vector<std::string> command;
    std::string why;
    if (!launchCommand(appId, path, &command, &why).empty()) {
        const Keys d = loadDesktopFile(app->path);
        if (get(d, "Terminal") == "true") {
            if (wrapInTerminal(command, &why)) return true;
        } else if (const std::string missing = missingProgram(command.front()); missing.empty()) {
            if (spawnDetachedProcess(command, &why)) return true;
        } else {
            why = "找不到程序“" + missing + "”，这可能是因为“" + app->name + "”没有安装完整。";
        }
    }
    if (error) *error = why;
    return false;
}

}  // namespace finder
