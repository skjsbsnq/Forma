#pragma once

// File system model for the Finder example: directory listings, kinds, sorting, formatting, places and the
// freedesktop.org Trash. Pure std::filesystem + POSIX; nothing here touches the UI.

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace finder {

enum class FileKind : std::uint8_t {
    Folder, Image, Video, Audio, Pdf, Text, Code, Archive, Executable, Document, Spreadsheet, Presentation,
    DiskImage, Font, Web, Unknown,
};

struct FileEntry {
    std::string name;
    std::string path;            ///< absolute
    std::string ext;             ///< lower case, without the dot
    FileKind kind = FileKind::Unknown;
    bool isDir = false;          ///< follows symlinks (a link to a folder opens like a folder)
    bool isLink = false;
    bool isHidden = false;
    bool isExecutable = false;   ///< the file itself has an execute bit (link targets decide isDir, not this)
    unsigned special = 0;        ///< S_ISUID | S_ISGID | S_ISVTX
    std::uintmax_t size = 0;     ///< files only
    std::time_t modified = 0;
    std::time_t created = 0;     ///< birth time where the file system records it, else 0
    unsigned mode = 0;           ///< st_mode permission bits
};

/// Virtual location listing ~/.local/share/recently-used.xbel.
inline constexpr const char* kRecents = "recents:";

enum class SortKey : std::uint8_t { Name, Modified, Size, Kind };

struct SortOrder {
    SortKey key = SortKey::Name;
    bool ascending = true;
    bool foldersFirst = true;
};

/// Lists `path` (or kRecents). Never throws; `error` receives a message when the folder can't be read.
std::vector<FileEntry> listDirectory(const std::string& path, bool showHidden, std::string* error = nullptr);
std::optional<FileEntry> statEntry(const std::string& path);
void sortEntries(std::vector<FileEntry>& entries, const SortOrder& order);

/// Natural, case-insensitive comparison ("file 2" < "file 10"), like the Finder.
int compareNames(const std::string& a, const std::string& b);
bool startsWithNoCase(const std::string& text, const std::string& prefix);
bool containsNoCase(const std::string& text, const std::string& needle);

FileKind kindForExtension(const std::string& ext);
std::string kindDescription(const FileEntry& e);                    ///< the format ("PNG 图像", "符号链接")
std::string kindLabel(FileKind kind, bool link = false);           ///< the class of item ("图像", "文件夹")
bool isImageExtension(const std::string& ext);   ///< decodable by mayo::ImageData
bool isTextLike(const FileEntry& e);

std::string formatSize(std::uintmax_t bytes);          ///< "12 KB", "1.4 MB" (SI units, like macOS)
std::string formatDate(std::time_t t);                 ///< "Today at 14:03", "3 Oct 2026 at 09:12"
std::string formatPermissions(unsigned mode, bool dir);  ///< "rwxr-xr-x"
std::string itemCount(size_t n);                       ///< "1 item", "12 items"

std::string homeDirectory();
/// XDG user directory ("DESKTOP", "DOCUMENTS", ...) from ~/.config/user-dirs.dirs, falling back to ~/<fallback>.
std::string userDirectory(const char* key, const char* fallback);
std::string trashFilesDirectory();
/// Free bytes on the file system containing `path`.
std::optional<std::uintmax_t> availableSpace(const std::string& path);
/// "Home" for $HOME, "Trash", "Recents", "/" for the root, else the last component.
std::string displayName(const std::string& path);
std::string parentPath(const std::string& path);
bool isInside(const std::string& path, const std::string& folder);  ///< folder itself counts

struct Volume {
    std::string name;
    std::string mountPoint;
    bool removable = false;
};
/// Mounted user volumes (USB sticks, other disks) from /proc/self/mounts.
std::vector<Volume> mountedVolumes();

/// A name for a new item in `folder` that doesn't exist yet: "untitled folder", "untitled folder 2", ...
std::string uniqueName(const std::string& folder, const std::string& name);
/// "photo copy.jpg", "photo copy 2.jpg" (Duplicate).
std::string copyName(const std::string& folder, const std::string& name);

// --- Trash (freedesktop.org Trash specification) -------------------------------------------------------

struct TrashedItem {
    std::string original;   ///< where it was
    std::string trashed;    ///< where it is now (…/Trash/files/<name>)
    std::string infoFile;   ///< …/Trash/info/<name>.trashinfo
};
/// Moves `path` to the Trash of its file system. Returns the record needed to put it back.
std::optional<TrashedItem> moveToTrash(const std::string& path, std::string* error);
/// Original location of an item in the home Trash, from its .trashinfo.
std::optional<std::string> trashOriginalPath(const std::string& trashedPath);
bool isInTrash(const std::string& path);
/// Permanently deletes everything in the home Trash.
void emptyTrash();

/// What happened when opening an item with its default application.
struct OpenResult {
    bool launched = false;
    bool needsExecutableBit = false;  ///< it is a program nobody marked executable
    bool noDefaultApp = false;        ///< nothing is set up to open this type
    std::string type;                 ///< MIME type, for noDefaultApp
    std::string application;          ///< display name of the application that was started (or failed)
    std::string error;                ///< why it failed
};
/// Opens `path` with the default application from mimeapps.list (the freedesktop way), detached. Programs are
/// run directly; the execute-bit check only ever applies to them.
OpenResult launchWithDefaultApp(const std::string& path);
/// Whether a MIME type is a program (ELF executable or shared object, Windows binaries).
bool isExecutableType(const std::string& mimeType);
/// chmod +x (read and execute for owner, group and others — what a "trusted" program gets). Returns "" or the
/// reason it failed.
std::string setExecutable(const std::string& path);
/// Opens a terminal in `folder` ($TERMINAL, or a few common ones).
void openTerminal(const std::string& folder);

}  // namespace finder
