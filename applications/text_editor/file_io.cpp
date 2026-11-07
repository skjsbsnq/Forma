#include "file_io.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace forma::text {
namespace fs = std::filesystem;
namespace {
FileResult failure(std::string_view operation) {
    return {FileStatus::Error, {}, std::string(operation) + ": " + std::strerror(errno)};
}
FileVersion version(const fs::path& target, const struct stat& st) {
    struct stat parent{};
    ::stat(target.parent_path().c_str(), &parent);
    return {target,
            true,
            static_cast<uint64_t>(st.st_dev),
            static_cast<uint64_t>(st.st_ino),
            static_cast<uint64_t>(st.st_size),
            static_cast<uint64_t>(st.st_nlink),
            st.st_mtim.tv_sec,
            st.st_mtim.tv_nsec,
            st.st_ctim.tv_sec,
            st.st_ctim.tv_nsec,
            static_cast<uint32_t>(st.st_mode & 07777),
            static_cast<uint64_t>(parent.st_dev),
            static_cast<uint64_t>(parent.st_ino),
            static_cast<uint32_t>(st.st_uid),
            static_cast<uint32_t>(st.st_gid)};
}
struct Descriptor {
    int fd = -1;
    ~Descriptor() {
        if (fd >= 0) ::close(fd);
    }
};
struct Temporary {
    fs::path path;
    FILE* file = nullptr;
    ~Temporary() {
        if (file) std::fclose(file);
        if (!path.empty()) ::unlink(path.c_str());
    }
};
FileResult conflict(FileVersion current) { return {FileStatus::Conflict, std::move(current), {}}; }
}  // namespace

FileResult inspectFile(const fs::path& path) {
    if (path.empty()) {
        errno = EINVAL;
        return failure("Path");
    }
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec) return {FileStatus::Error, {}, ec.message()};
    struct stat entry{};
    const bool exists = ::lstat(absolute.c_str(), &entry) == 0;
    if (!exists && errno != ENOENT) return failure("Inspect");
    // canonical() follows every symlink; a dangling link is an error, never
    // replaced by a regular file. For a new file only its parent must exist.
    auto target =
        exists ? fs::canonical(absolute, ec) : fs::canonical(absolute.parent_path(), ec) / absolute.filename();
    if (ec) return {FileStatus::Error, {}, ec.message()};
    if (!exists) {
        struct stat parent{};
        if (::stat(target.parent_path().c_str(), &parent) != 0) return failure("Inspect directory");
        return {FileStatus::Success,
                FileVersion{.target = target,
                            .parentDevice = static_cast<uint64_t>(parent.st_dev),
                            .parentInode = static_cast<uint64_t>(parent.st_ino)},
                {}};
    }
    struct stat st{};
    if (::stat(target.c_str(), &st) != 0) return failure("Inspect target");
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return failure("Not a regular file");
    }
    return {FileStatus::Success, version(target, st), {}};
}

FileResult readFile(const fs::path& path, std::string& value) {
    auto initial = inspectFile(path);
    if (!initial) return initial;
    if (!initial.version.exists) {
        errno = ENOENT;
        return failure("Open");
    }
    Descriptor input{::open(initial.version.target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (input.fd < 0) return failure("Open");
    struct stat st{};
    if (::fstat(input.fd, &st) != 0) return failure("Inspect open file");
    if (version(initial.version.target, st) != initial.version) return conflict(initial.version);
    std::string read;
    char buffer[65536];
    for (;;) {
        auto count = ::read(input.fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return failure("Read");
        if (!count) break;
        read.append(buffer, static_cast<size_t>(count));
    }
    if (::fstat(input.fd, &st) != 0) return failure("Inspect read file");
    auto final = inspectFile(path);
    if (!final) return final;
    if (final.version != initial.version || version(initial.version.target, st) != initial.version)
        return conflict(final.version);
    value = std::move(read);
    return final;
}

FileResult saveFile(const fs::path& path, std::string_view value, const std::optional<FileVersion>& baseline,
                    const SaveFault& fault) {
    auto current = inspectFile(path);
    if (!current) return current;
    if ((baseline && current.version != *baseline) || (!baseline && current.version.exists))
        return conflict(current.version);
    if (current.version.links > 1) return {FileStatus::HardLink, current.version, {}};
    const auto& target = current.version.target;
    Descriptor directory{::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0) return failure("Open directory");
    struct stat parent{};
    if (::fstat(directory.fd, &parent) != 0) return failure("Inspect directory");
    if (static_cast<uint64_t>(parent.st_dev) != current.version.parentDevice ||
        static_cast<uint64_t>(parent.st_ino) != current.version.parentInode)
        return conflict(current.version);
    std::string pattern = (target.parent_path() / ".forma-save-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    Descriptor fd{::mkstemp(name.data())};
    if (fd.fd < 0) return failure("Create temporary");
    Temporary temporary{fs::path(name.data()), nullptr};
    if (::fcntl(fd.fd, F_SETFD, FD_CLOEXEC) < 0) return failure("Protect temporary descriptor");
    temporary.file = ::fdopen(fd.fd, "wb");
    if (!temporary.file) return failure("Open temporary stream");
    fd.fd = -1;  // ownership moved to FILE
    auto fails = [&](SaveStage stage) {
        if (fault && fault(stage)) {
            errno = EIO;
            return true;
        }
        return false;
    };
    if (fails(SaveStage::Write) || std::fwrite(value.data(), 1, value.size(), temporary.file) != value.size() ||
        std::ferror(temporary.file))
        return failure("Write");
    if (fails(SaveStage::Flush) || std::fflush(temporary.file) != 0) return failure("Flush");
    struct stat ownership{};
    if (::fstat(::fileno(temporary.file), &ownership) != 0) return failure("Inspect temporary permissions");
    if (current.version.exists &&
        (ownership.st_uid != current.version.owner || ownership.st_gid != current.version.group) &&
        ::fchown(::fileno(temporary.file), current.version.owner, current.version.group) != 0)
        return failure("Preserve ownership");
    // Apply permissions after writing/chown (both can clear set-ID bits). New documents
    // stay private (0600); existing documents retain their permission bits.
    if (current.version.exists && ::fchmod(::fileno(temporary.file), current.version.mode) != 0)
        return failure("Preserve permissions");
    if (fails(SaveStage::Sync) || ::fsync(::fileno(temporary.file)) != 0) return failure("Sync");
    struct stat prepared{};
    if (::fstat(::fileno(temporary.file), &prepared) != 0) return failure("Inspect temporary");
    auto committedVersion = version(target, prepared);
    const bool closeFault = fails(SaveStage::Close);
    FILE* stream = temporary.file;
    temporary.file = nullptr;
    if (std::fclose(stream) != 0 || closeFault) return failure("Close");
    // Re-resolve the requested name, not only the target, so retargeted symlinks
    // and replaced/deleted files are conflicts too. POSIX cannot compare-and-rename
    // against an uncooperative writer; keep this check immediately before commit.
    auto checked = inspectFile(path);
    if (!checked) return checked;
    if (checked.version != current.version) return conflict(checked.version);
    if (fails(SaveStage::Rename)) return failure("Rename");
    if (current.version.exists) {
        if (::renameat(directory.fd, temporary.path.filename().c_str(), directory.fd, target.filename().c_str()) != 0)
            return failure("Rename");
    } else {
        // linkat is an atomic no-replace commit for new documents. rename() would
        // clobber a file created between the last inspection and this operation.
        if (::linkat(directory.fd, temporary.path.filename().c_str(), directory.fd, target.filename().c_str(), 0) !=
            0) {
            if (errno == EEXIST) return conflict(inspectFile(path).version);
            return failure("Create destination");
        }
        ::unlinkat(directory.fd, temporary.path.filename().c_str(), 0);
    }
    // No fallible operation after commit may turn a completed save into a failure.
    // A concurrent later write will be detected against this inode's baseline.
    struct stat saved{};
    if (::fstatat(directory.fd, target.filename().c_str(), &saved, AT_SYMLINK_NOFOLLOW) == 0) {
        auto observed = version(target, saved);
        // Commit changes ctime/nlink. Never adopt a later external replacement.
        if (observed.device == committedVersion.device && observed.inode == committedVersion.inode &&
            observed.size == committedVersion.size && observed.mode == committedVersion.mode &&
            observed.modifiedSeconds == committedVersion.modifiedSeconds &&
            observed.modifiedNanos == committedVersion.modifiedNanos)
            committedVersion = observed;
    }
    return {FileStatus::Success, std::move(committedVersion), {}};
}
}  // namespace forma::text
