#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace forma::text {

// A baseline is tied to both the requested path (including symlink resolution) and
// the inode/version that was read. It is not just a modification timestamp.
struct FileVersion {
    std::filesystem::path target;
    bool exists = false;
    uint64_t device = 0, inode = 0, size = 0, links = 0;
    int64_t modifiedSeconds = 0, modifiedNanos = 0, changedSeconds = 0, changedNanos = 0;
    uint32_t mode = 0;
    uint64_t parentDevice = 0, parentInode = 0;
    uint32_t owner = 0, group = 0;
    bool operator==(const FileVersion&) const = default;
};
enum class FileStatus { Success, Conflict, HardLink, Error };
struct FileResult {
    FileStatus status = FileStatus::Error;
    FileVersion version;
    std::string error;
    explicit operator bool() const { return status == FileStatus::Success; }
};
// Test-only fault seam. Returning true fails that operation before committing;
// the real descriptor is still closed and the exclusive temporary is removed.
enum class SaveStage { Write, Flush, Sync, Close, Rename };
using SaveFault = std::function<bool(SaveStage)>;

FileResult inspectFile(const std::filesystem::path& path);
FileResult readFile(const std::filesystem::path& path, std::string& value);
// No baseline means "create only", not permission to clobber an existing file.
// An explicitly confirmed overwrite supplies a freshly inspected baseline.
// Multiply-linked files are always refused: Save As is the safe policy, since
// atomic replacement would silently split the links and in-place writes aren't atomic.
FileResult saveFile(const std::filesystem::path& path, std::string_view value,
                    const std::optional<FileVersion>& baseline = {}, const SaveFault& fault = {});

}  // namespace forma::text
