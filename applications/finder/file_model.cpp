#include "file_model.hpp"

#include "applications.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "applications.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <string_view>

namespace fs = std::filesystem;

namespace finder {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct KindInfo {
    FileKind kind;
    const char* description;
};

const std::map<std::string, KindInfo>& kindTable() {
    static const std::map<std::string, KindInfo> table = {
        {"png", {FileKind::Image, "PNG 图像"}}, {"jpg", {FileKind::Image, "JPEG 图像"}},
        {"jpeg", {FileKind::Image, "JPEG 图像"}}, {"gif", {FileKind::Image, "GIF 图像"}},
        {"webp", {FileKind::Image, "WebP 图像"}}, {"bmp", {FileKind::Image, "BMP 图像"}},
        {"tga", {FileKind::Image, "TGA 图像"}}, {"tif", {FileKind::Image, "TIFF 图像"}},
        {"tiff", {FileKind::Image, "TIFF 图像"}}, {"heic", {FileKind::Image, "HEIC 图像"}},
        {"avif", {FileKind::Image, "AVIF 图像"}}, {"ico", {FileKind::Image, "Windows 图标"}},
        {"svg", {FileKind::Image, "SVG 文档"}}, {"psd", {FileKind::Image, "Photoshop 文档"}},
        {"xcf", {FileKind::Image, "GIMP 图像"}}, {"kra", {FileKind::Image, "Krita 文档"}},
        {"mp4", {FileKind::Video, "MPEG-4 视频"}}, {"m4v", {FileKind::Video, "MPEG-4 视频"}},
        {"mkv", {FileKind::Video, "Matroska 视频"}}, {"webm", {FileKind::Video, "WebM 视频"}},
        {"mov", {FileKind::Video, "QuickTime 视频"}}, {"avi", {FileKind::Video, "AVI 视频"}},
        {"wmv", {FileKind::Video, "Windows Media 视频"}}, {"flv", {FileKind::Video, "Flash 视频"}},
        {"mp3", {FileKind::Audio, "MP3 音频"}}, {"flac", {FileKind::Audio, "FLAC 音频"}},
        {"wav", {FileKind::Audio, "Waveform 音频"}}, {"ogg", {FileKind::Audio, "Ogg Vorbis 音频"}},
        {"opus", {FileKind::Audio, "Opus 音频"}}, {"m4a", {FileKind::Audio, "MPEG-4 音频"}},
        {"aac", {FileKind::Audio, "AAC 音频"}}, {"aiff", {FileKind::Audio, "AIFF 音频"}},
        {"pdf", {FileKind::Pdf, "PDF 文档"}},
        {"txt", {FileKind::Text, "纯文本"}}, {"md", {FileKind::Text, "Markdown 文档"}},
        {"rst", {FileKind::Text, "reStructuredText"}}, {"log", {FileKind::Text, "日志文件"}},
        {"json", {FileKind::Text, "JSON 文档"}}, {"xml", {FileKind::Text, "XML 文档"}},
        {"yaml", {FileKind::Text, "YAML 文档"}}, {"yml", {FileKind::Text, "YAML 文档"}},
        {"toml", {FileKind::Text, "TOML 文档"}}, {"ini", {FileKind::Text, "配置文件"}},
        {"conf", {FileKind::Text, "配置文件"}}, {"cfg", {FileKind::Text, "配置文件"}},
        {"desktop", {FileKind::Text, "桌面启动项"}}, {"srt", {FileKind::Text, "字幕"}},
        {"c", {FileKind::Code, "C 源代码"}}, {"h", {FileKind::Code, "C 头文件"}},
        {"cpp", {FileKind::Code, "C++ 源代码"}}, {"cc", {FileKind::Code, "C++ 源代码"}},
        {"cxx", {FileKind::Code, "C++ 源代码"}}, {"hpp", {FileKind::Code, "C++ 头文件"}},
        {"py", {FileKind::Code, "Python 脚本"}}, {"js", {FileKind::Code, "JavaScript"}},
        {"mjs", {FileKind::Code, "JavaScript"}}, {"ts", {FileKind::Code, "TypeScript"}},
        {"tsx", {FileKind::Code, "TypeScript JSX"}}, {"jsx", {FileKind::Code, "JavaScript JSX"}},
        {"rs", {FileKind::Code, "Rust 源代码"}}, {"go", {FileKind::Code, "Go 源代码"}},
        {"java", {FileKind::Code, "Java 源代码"}}, {"kt", {FileKind::Code, "Kotlin 源代码"}},
        {"swift", {FileKind::Code, "Swift 源代码"}}, {"cs", {FileKind::Code, "C# 源代码"}},
        {"rb", {FileKind::Code, "Ruby 脚本"}}, {"lua", {FileKind::Code, "Lua 脚本"}},
        {"sh", {FileKind::Code, "Shell 脚本"}}, {"bash", {FileKind::Code, "Shell 脚本"}},
        {"zsh", {FileKind::Code, "Shell 脚本"}}, {"fish", {FileKind::Code, "Shell 脚本"}},
        {"cmake", {FileKind::Code, "CMake 脚本"}}, {"glsl", {FileKind::Code, "GLSL 着色器"}},
        {"vert", {FileKind::Code, "GLSL 着色器"}}, {"frag", {FileKind::Code, "GLSL 着色器"}},
        {"css", {FileKind::Code, "CSS 样式表"}}, {"sql", {FileKind::Code, "SQL 脚本"}},
        {"html", {FileKind::Web, "HTML 文档"}}, {"htm", {FileKind::Web, "HTML 文档"}},
        {"zip", {FileKind::Archive, "ZIP 压缩包"}}, {"tar", {FileKind::Archive, "tar 压缩包"}},
        {"gz", {FileKind::Archive, "gzip 压缩包"}}, {"tgz", {FileKind::Archive, "gzip tar 压缩包"}},
        {"xz", {FileKind::Archive, "XZ 压缩包"}}, {"bz2", {FileKind::Archive, "bzip2 压缩包"}},
        {"zst", {FileKind::Archive, "Zstandard 压缩包"}}, {"7z", {FileKind::Archive, "7-Zip 压缩包"}},
        {"rar", {FileKind::Archive, "RAR 压缩包"}}, {"deb", {FileKind::Archive, "Debian 软件包"}},
        {"rpm", {FileKind::Archive, "RPM 软件包"}}, {"jar", {FileKind::Archive, "Java 压缩包"}},
        {"iso", {FileKind::DiskImage, "磁盘映像"}}, {"img", {FileKind::DiskImage, "磁盘映像"}},
        {"dmg", {FileKind::DiskImage, "Apple 磁盘映像"}}, {"qcow2", {FileKind::DiskImage, "QEMU 磁盘映像"}},
        {"ttf", {FileKind::Font, "TrueType 字体"}}, {"otf", {FileKind::Font, "OpenType 字体"}},
        {"woff", {FileKind::Font, "Web 字体"}}, {"woff2", {FileKind::Font, "Web 字体"}},
        {"doc", {FileKind::Document, "Word 文档"}}, {"docx", {FileKind::Document, "Word 文档"}},
        {"odt", {FileKind::Document, "OpenDocument 文本"}}, {"rtf", {FileKind::Document, "Rich Text 文档"}},
        {"epub", {FileKind::Document, "EPUB 电子书"}},
        {"xls", {FileKind::Spreadsheet, "Excel 电子表格"}}, {"xlsx", {FileKind::Spreadsheet, "Excel 电子表格"}},
        {"ods", {FileKind::Spreadsheet, "OpenDocument 电子表格"}}, {"csv", {FileKind::Spreadsheet, "CSV 文档"}},
        {"ppt", {FileKind::Presentation, "PowerPoint 演示文稿"}},
        {"pptx", {FileKind::Presentation, "PowerPoint 演示文稿"}},
        {"odp", {FileKind::Presentation, "OpenDocument 演示文稿"}},
        {"so", {FileKind::Executable, "共享库"}}, {"appimage", {FileKind::Executable, "AppImage"}},
        {"exe", {FileKind::Executable, "Windows 可执行文件"}}, {"bin", {FileKind::Executable, "二进制文件"}},
    };
    return table;
}

std::optional<FileEntry> makeEntry(const std::string& path, std::string name) {
    struct statx sx {};
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &sx) != 0) return std::nullopt;
    FileEntry e;
    e.path = path;
    e.name = std::move(name);
    e.isHidden = !e.name.empty() && (e.name[0] == '.' || e.name.back() == '~');
    e.isLink = S_ISLNK(sx.stx_mode);
    unsigned mode = sx.stx_mode;
    std::uintmax_t size = sx.stx_size;
    std::time_t mtime = sx.stx_mtime.tv_sec;
    if (e.isLink) {
        struct stat target {};
        if (::stat(path.c_str(), &target) == 0) {  // broken links keep the link's own attributes
            mode = target.st_mode;
            size = static_cast<std::uintmax_t>(target.st_size);
            mtime = target.st_mtime;
        }
    }
    e.isDir = S_ISDIR(mode);
    e.isExecutable = !e.isDir && (mode & 0111) != 0;
    e.special = mode & (S_ISUID | S_ISGID | S_ISVTX);
    e.mode = mode & 07777;
    e.size = e.isDir ? 0 : size;
    e.modified = mtime;
    e.created = (sx.stx_mask & STATX_BTIME) ? static_cast<std::time_t>(sx.stx_btime.tv_sec) : 0;
    if (e.isDir) {
        e.kind = FileKind::Folder;
    } else if (e.isExecutable) {
        // What the Finder colours as a program: the x bit, whatever the name says. A dot keeps "run.sh" a
        // script (its kind and extension), a bare name is a program.
        e.kind = FileKind::Executable;
        if (std::string_view(e.name).find('.') != std::string_view::npos) {
            e.ext = lower(fs::path(e.name).extension().string());
            if (!e.ext.empty()) e.ext.erase(0, 1);
        }
    } else {
        // std::filesystem treats ".bashrc" as a stem without extension, which is what we want.
        e.ext = lower(fs::path(e.name).extension().string());
        if (!e.ext.empty()) e.ext.erase(0, 1);
        e.kind = kindForExtension(e.ext);
        if (e.kind == FileKind::Unknown && (e.name == "Makefile" || e.name == "Dockerfile" || e.name == "PKGBUILD"))
            e.kind = FileKind::Code;
    }
    return e;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string percentEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::vector<FileEntry> listRecents(bool showHidden) {
    std::ifstream in(homeDirectory() + "/.local/share/recently-used.xbel");
    std::string xml((std::istreambuf_iterator<char>(in)), {});
    // <bookmark href="file:///..." ... modified="2026-10-01T10:00:00Z" ...>
    std::vector<std::pair<std::string, std::string>> found;  // modified, path
    size_t pos = 0;
    while ((pos = xml.find("<bookmark ", pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        std::string tag = xml.substr(pos, end - pos);
        pos = end;
        auto attr = [&](const char* name) {
            std::string key = std::string(name) + "=\"";
            size_t a = tag.find(key);
            if (a == std::string::npos) return std::string();
            a += key.size();
            return tag.substr(a, tag.find('"', a) - a);
        };
        std::string href = attr("href");
        if (href.rfind("file://", 0) != 0) continue;
        std::string when = attr("modified");
        if (when.empty()) when = attr("visited");
        found.emplace_back(when, percentDecode(href.substr(7)));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<FileEntry> out;
    std::set<std::string> seen;
    for (const auto& [when, path] : found) {
        if (out.size() >= 200 || !seen.insert(path).second) continue;
        auto e = makeEntry(path, fs::path(path).filename().string());
        if (!e || e->isDir || (e->isHidden && !showHidden)) continue;
        out.push_back(std::move(*e));
    }
    return out;
}

/// The directory holding `path`'s mount ("topdir" in the Trash spec).
std::string mountTop(const std::string& path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return "/";
    fs::path cur = fs::path(path).parent_path();
    fs::path top = cur;
    while (!cur.empty()) {
        struct stat up {};
        if (::stat(cur.c_str(), &up) != 0 || up.st_dev != st.st_dev) break;
        top = cur;
        if (cur == cur.root_path()) break;
        cur = cur.parent_path();
    }
    return top.string();
}

std::string homeTrash() {
    const char* data = std::getenv("XDG_DATA_HOME");
    return (data && *data ? std::string(data) : homeDirectory() + "/.local/share") + "/Trash";
}

std::string localTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm {};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

bool onPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty() && ::access((dir + "/" + program).c_str(), X_OK) == 0) return true;
    }
    return false;
}

}  // namespace

std::vector<FileEntry> listDirectory(const std::string& path, bool showHidden, std::string* error) {
    if (path == kRecents) return listRecents(showHidden);
    std::vector<FileEntry> out;
    std::error_code ec;
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error) *error = ec.message();
        return out;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (!showHidden && (name[0] == '.' || name.back() == '~')) continue;
        if (auto e = makeEntry(it->path().string(), std::move(name))) out.push_back(std::move(*e));
    }
    return out;
}

std::optional<FileEntry> statEntry(const std::string& path) {
    std::string name = fs::path(path).filename().string();
    if (name.empty()) name = path;
    return makeEntry(path, std::move(name));
}

int compareNames(const std::string& a, const std::string& b) {
    size_t i = 0;
    size_t j = 0;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            size_t i0 = i;
            size_t j0 = j;
            while (i0 < a.size() && a[i0] == '0') ++i0;
            while (j0 < b.size() && b[j0] == '0') ++j0;
            size_t i1 = i0;
            size_t j1 = j0;
            while (i1 < a.size() && digit(a[i1])) ++i1;
            while (j1 < b.size() && digit(b[j1])) ++j1;
            if (i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0 ? -1 : 1;
            int c = a.compare(i0, i1 - i0, b, j0, j1 - j0);
            if (c != 0) return c < 0 ? -1 : 1;
            i = i1;
            j = j1;
            continue;
        }
        int ca = std::tolower(static_cast<unsigned char>(a[i]));
        int cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return a < b ? -1 : (a > b ? 1 : 0);
}

bool startsWithNoCase(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

bool containsNoCase(const std::string& text, const std::string& needle) {
    return lower(text).find(lower(needle)) != std::string::npos;
}

void sortEntries(std::vector<FileEntry>& entries, const SortOrder& order) {
    std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry& a, const FileEntry& b) {
        if (order.foldersFirst && a.isDir != b.isDir) return a.isDir;
        int c = 0;
        switch (order.key) {
            case SortKey::Name: break;
            case SortKey::Modified: c = a.modified < b.modified ? -1 : (a.modified > b.modified ? 1 : 0); break;
            case SortKey::Size: c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
            case SortKey::Kind: c = compareNames(kindDescription(a), kindDescription(b)); break;
        }
        if (c == 0) c = compareNames(a.name, b.name);
        return order.ascending ? c < 0 : c > 0;
    });
}

FileKind kindForExtension(const std::string& ext) {
    auto it = kindTable().find(ext);
    return it == kindTable().end() ? FileKind::Unknown : it->second.kind;
}

std::string kindLabel(FileKind kind, bool link) {
    if (link) return "符号链接";
    switch (kind) {
        case FileKind::Folder: return "文件夹";
        case FileKind::Image: return "图像";
        case FileKind::Video: return "视频";
        case FileKind::Audio: return "音频";
        case FileKind::Pdf: return "PDF 文档";
        case FileKind::Text: return "文本文稿";
        case FileKind::Code: return "源代码";
        case FileKind::Archive: return "压缩包";
        case FileKind::Executable: return "Unix 可执行文件";
        case FileKind::Document: return "文稿";
        case FileKind::Spreadsheet: return "电子表格";
        case FileKind::Presentation: return "演示文稿";
        case FileKind::DiskImage: return "磁盘映像";
        case FileKind::Font: return "字体";
        case FileKind::Web: return "网页";
        case FileKind::Unknown: return "文档";
    }
    return "文档";
}

std::string kindDescription(const FileEntry& e) {
    if (e.isLink) return "符号链接";
    if (e.isDir) return "文件夹";
    auto it = kindTable().find(e.ext);
    if (it != kindTable().end()) return it->second.description;
    if (e.kind == FileKind::Executable) return "Unix 可执行文件";
    if (e.kind == FileKind::Code) return "构建脚本";
    if (!e.ext.empty()) {
        std::string up = e.ext;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return up + " 文件";
    }
    return "文档";
}

bool isImageExtension(const std::string& ext) {
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp" || ext == "tga";
}

bool isTextLike(const FileEntry& e) {
    return e.kind == FileKind::Text || e.kind == FileKind::Code || e.kind == FileKind::Web || e.ext == "csv" ||
           e.ext == "svg" || (e.kind == FileKind::Unknown && e.size < 512 * 1024);
}

std::string formatSize(std::uintmax_t b) {
    char buf[32];
    if (b == 0) return "0 字节";
    if (b < 1000) std::snprintf(buf, sizeof buf, "%ju 字节", b);
    else if (b < 1000 * 1000) std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(b) / 1e3);
    else if (b < 1000ull * 1000 * 1000) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / 1e6);
    else if (b < 1000ull * 1000 * 1000 * 1000) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / 1e9);
    else std::snprintf(buf, sizeof buf, "%.2f TB", static_cast<double>(b) / 1e12);
    return buf;
}

std::string formatDate(std::time_t t) {
    if (t <= 0) return "--";
    std::tm when {};
    std::tm today {};
    std::time_t now = std::time(nullptr);
    localtime_r(&t, &when);
    localtime_r(&now, &today);
    char time[16];
    std::strftime(time, sizeof time, "%H:%M", &when);
    std::tm yesterday = today;
    yesterday.tm_mday -= 1;
    std::mktime(&yesterday);
    auto sameDay = [](const std::tm& a, const std::tm& b) { return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday; };
    if (sameDay(when, today)) return std::string("今天 ") + time;
    if (sameDay(when, yesterday)) return std::string("昨天 ") + time;
    char date[32];
    std::strftime(date, sizeof date, "%Y年%m月%d日", &when);
    std::string d = date;
    if (!d.empty() && d[0] == ' ') d.erase(0, 1);
    return d + " " + time;
}

std::string formatPermissions(unsigned mode, bool dir) {
    std::string s = dir ? "d" : "-";
    const char* rwx = "rwx";
    for (int i = 8; i >= 0; --i) s.push_back((mode & (1u << i)) ? rwx[(8 - i) % 3] : '-');
    return s;
}

std::string itemCount(size_t n) { return std::to_string(n) + " 个项目"; }

std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return home && *home ? home : "/";
}

std::string userDirectory(const char* key, const char* fallback) {
    const std::string home = homeDirectory();
    std::ifstream in(home + "/.config/user-dirs.dirs");
    std::string line;
    const std::string prefix = std::string("XDG_") + key + "_DIR=";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0) continue;
        std::string v = line.substr(prefix.size());
        if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
        if (v == home || v == home + "/") return {};  // unset directories point at $HOME
        return v;
    }
    return home + "/" + fallback;
}

std::string trashFilesDirectory() { return homeTrash() + "/files"; }

std::optional<std::uintmax_t> availableSpace(const std::string& path) {
    struct statvfs st {};
    if (path == kRecents || ::statvfs(path.c_str(), &st) != 0) return std::nullopt;
    return static_cast<std::uintmax_t>(st.f_bavail) * st.f_frsize;
}

std::string displayName(const std::string& path) {
    if (path == kRecents) return "最近使用";
    if (path == "/") return "计算机";
    if (path == homeDirectory()) return "主目录";
    if (path == trashFilesDirectory()) return "回收站";
    std::string name = fs::path(path).filename().string();
    return name.empty() ? path : name;
}

std::string parentPath(const std::string& path) {
    if (path == "/" || path == kRecents) return path;
    std::string p = fs::path(path).parent_path().string();
    return p.empty() ? "/" : p;
}

bool isInside(const std::string& path, const std::string& folder) {
    if (folder == "/") return !path.empty() && path[0] == '/';
    return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
                              path[folder.size()] == '/');
}

std::vector<Volume> mountedVolumes() {
    std::vector<Volume> out;
    std::ifstream in("/proc/self/mounts");
    std::string device, mount, type, rest;
    while (in >> device >> mount >> type && std::getline(in, rest)) {
        // Mount points escape spaces as \040.
        std::string decoded;
        for (size_t i = 0; i < mount.size(); ++i) {
            if (mount[i] == '\\' && i + 3 < mount.size()) {
                decoded.push_back(static_cast<char>(std::stoi(mount.substr(i + 1, 3), nullptr, 8)));
                i += 3;
            } else {
                decoded.push_back(mount[i]);
            }
        }
        const bool removable = decoded.rfind("/run/media/", 0) == 0 || decoded.rfind("/media/", 0) == 0;
        if (!removable && decoded.rfind("/mnt/", 0) != 0) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Volume& v) { return v.mountPoint == decoded; })) continue;
        out.push_back({fs::path(decoded).filename().string(), decoded, removable});
    }
    return out;
}

std::string uniqueName(const std::string& folder, const std::string& name) {
    std::error_code ec;
    if (!fs::exists(fs::path(folder) / name, ec)) return name;
    const fs::path p(name);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    if (stem.empty()) {  // ".hidden"
        stem = name;
        ext.clear();
    }
    for (int i = 2;; ++i) {
        std::string candidate = stem + " " + std::to_string(i) + ext;
        if (!fs::exists(fs::path(folder) / candidate, ec)) return candidate;
    }
}

std::string copyName(const std::string& folder, const std::string& name) {
    const fs::path p(name);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    if (stem.empty()) {
        stem = name;
        ext.clear();
    }
    std::error_code ec;
    std::string candidate = stem + " 副本" + ext;
    for (int i = 2; fs::exists(fs::path(folder) / candidate, ec); ++i) candidate = stem + " 副本 " + std::to_string(i) + ext;
    return candidate;
}

// --- Trash -------------------------------------------------------------------------------------------

std::optional<TrashedItem> moveToTrash(const std::string& path, std::string* error) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        if (error) *error = std::strerror(errno);
        return std::nullopt;
    }
    std::string trash = homeTrash();
    std::error_code ec;
    fs::create_directories(trash + "/files", ec);
    fs::create_directories(trash + "/info", ec);
    struct stat trashStat {};
    bool relative = false;
    std::string top;
    if (::stat(trash.c_str(), &trashStat) != 0 || trashStat.st_dev != st.st_dev) {
        // Another file system (USB stick): use its own $topdir/.Trash-$uid so nothing is copied.
        top = mountTop(path);
        trash = top + "/.Trash-" + std::to_string(::getuid());
        fs::create_directories(trash + "/files", ec);
        fs::create_directories(trash + "/info", ec);
        fs::permissions(trash, fs::perms::owner_all, ec);
        relative = true;
    }
    const std::string base = fs::path(path).filename().string();
    std::string name = base;
    std::string infoPath;
    for (int i = 2;; ++i) {
        infoPath = trash + "/info/" + name + ".trashinfo";
        // O_EXCL claims the name atomically, as the spec asks.
        int fd = ::open(infoPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 && !fs::exists(trash + "/files/" + name, ec)) {
            std::string original = relative ? path.substr(top.size() + (top == "/" ? 0 : 1)) : path;
            std::string info = "[Trash Info]\nPath=" + percentEncode(original) + "\nDeletionDate=" + localTimestamp() + "\n";
            bool ok = ::write(fd, info.data(), info.size()) == static_cast<ssize_t>(info.size());
            ::close(fd);
            if (!ok) {
                ::unlink(infoPath.c_str());
                if (error) *error = "无法写入回收站。";
                return std::nullopt;
            }
            break;
        }
        if (fd >= 0) {
            ::close(fd);
            ::unlink(infoPath.c_str());
        } else if (errno != EEXIST) {
            if (error) *error = std::strerror(errno);
            return std::nullopt;
        }
        const fs::path p(base);
        name = p.stem().string() + "." + std::to_string(i) + p.extension().string();
    }
    const std::string target = trash + "/files/" + name;
    if (::rename(path.c_str(), target.c_str()) != 0) {
        if (error) *error = std::strerror(errno);
        ::unlink(infoPath.c_str());
        return std::nullopt;
    }
    return TrashedItem{path, target, infoPath};
}

std::optional<std::string> trashOriginalPath(const std::string& trashedPath) {
    const fs::path p(trashedPath);
    const fs::path trash = p.parent_path().parent_path();
    std::ifstream in((trash / "info" / (p.filename().string() + ".trashinfo")).string());
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Path=", 0) != 0) continue;
        std::string original = percentDecode(line.substr(5));
        if (!original.empty() && original[0] != '/') original = (trash.parent_path() / original).string();
        return original;
    }
    return std::nullopt;
}

bool isInTrash(const std::string& path) { return isInside(path, trashFilesDirectory()) && path != trashFilesDirectory(); }

void emptyTrash() {
    std::error_code ec;
    for (const char* sub : {"/files", "/info"}) {
        for (const auto& entry : fs::directory_iterator(homeTrash() + sub, ec)) fs::remove_all(entry.path(), ec);
    }
}

OpenResult launchWithDefaultApp(const std::string& path) {
    OpenResult result;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        result.error = "找不到该项目，它可能已被移动或删除。";
        return result;
    }
    const std::string type = mimeTypeFor(path);
    // Only a program needs an execute bit, and when it lacks one it can't be started by anybody, so say why
    // instead of handing it to a launcher that would fail with an error nobody explains. Everything else — a
    // picture, a text, a video — is opened by its default application, whatever its permissions are.
    const bool isProgram = (st.st_mode & 0111) != 0;
    if (S_ISREG(st.st_mode) && !isProgram && (isExecutableType(type) || lower(fs::path(path).extension().string()) == ".appimage")) {
        result.needsExecutableBit = true;
        return result;
    }
    const std::string id = defaultApplicationId(path);
    if (!id.empty()) {
        const Application* app = findApplication(id);
        result.application = app ? app->name : id;
        if (launchApplication(id, path, &result.error)) {
            result.launched = true;
            return result;
        }
        if (!result.error.empty()) return result;  // say what went wrong, don't guess another application
    }
    // No default application: a program is run directly — running it *is* opening it — and anything else is
    // offered the "打开方式" chooser.
    if (S_ISREG(st.st_mode) && isProgram) {
        if (spawnDetachedProcess({path}, &result.error)) {
            result.launched = true;
            result.application = displayName(path);
            return result;
        }
        if (result.error.empty()) result.error = "无法运行“" + displayName(path) + "”。";
        return result;
    }
    result.noDefaultApp = true;
    result.type = type;
    return result;
}

bool isExecutableType(const std::string& mimeType) {
    static const char* const types[] = {"application/x-executable", "application/x-sharedlib",
                                        "application/x-pie-executable", "application/x-msdos-program",
                                        "application/vnd.microsoft.portable-executable", "application/x-msi",
                                        "application/x-shared-library-launcher", "application/x-iso9660-appimage",
                                        "application/vnd.appimage", "application/x-appimage"};
    for (const char* t : types) {
        if (mimeType == t) return true;
    }
    return false;
}

std::string setExecutable(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return std::string("找不到该项目：") + std::strerror(errno) + "。";
    const mode_t mode = (st.st_mode & 07777) | 0111;
    if (::chmod(path.c_str(), mode) != 0) {
        if (errno == EPERM) return "只能为自己拥有的文件添加执行权限。";
        return std::string("无法更改权限：") + std::strerror(errno) + "。";
    }
    return {};
}

void openTerminal(const std::string& folder) {
    // Our own Terminal first, like Finder opens Terminal.app. It ships next to us in /usr/bin; when running from
    // a build tree, look for the sibling build directory.
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    for (const fs::path& own : {self.parent_path() / "terminal", self.parent_path().parent_path() / "terminal" / "terminal"}) {
        if (!ec && fs::exists(own, ec) && own != self) spawnDetachedIn({own.string(), folder}, folder);
    }
    if (onPath("terminal")) spawnDetachedIn({"terminal", folder}, folder);
    if (const char* t = std::getenv("TERMINAL"); t && *t && onPath(t)) spawnDetachedIn({t}, folder);
    // Most terminals run as a single process per user, so a new window starts in *that* process's directory
    // unless it's passed explicitly; the working directory of the child alone isn't enough.
    const std::vector<std::vector<std::string>> known = {
        {"xdg-terminal-exec", "--dir=" + folder},
        {"ptyxis", "--new-window", "--working-directory=" + folder},
        {"kgx", "--working-directory=" + folder},
        {"gnome-terminal", "--working-directory=" + folder},
        {"konsole", "--workdir", folder},
        {"kitty", "--directory", folder},
        {"alacritty", "--working-directory", folder},
        {"foot", "--working-directory=" + folder},
        {"wezterm", "start", "--cwd", folder},
        {"ghostty", "--working-directory=" + folder},
        {"xfce4-terminal", "--working-directory=" + folder},
        {"tilix", "--working-directory=" + folder},
        {"xterm"},
    };
    for (const auto& cmd : known) {
        if (onPath(cmd[0])) spawnDetachedIn(cmd, folder);
    }
}

}  // namespace finder
