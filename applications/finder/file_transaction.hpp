#pragma once

// UI-free transactions. Private sibling directories own every unpublished object; publishing and
// restoring always use RENAME_NOREPLACE. No fallback may turn a failed rename into an overwrite.
#include "file_model.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace finder::transaction {
namespace fs = std::filesystem;

enum class Phase { Queued, Scanning, Copying, Committing, RemovingSource, Restoring, Finished };
enum class Fault { Rename, Write, Commit, RemoveSource, Restore };
struct Progress {
    Phase phase = Phase::Queued;
    std::string item;
    std::uintmax_t bytes = 0, total = 0;
};
struct Control {
    std::atomic<bool> cancel{false};
    Progress progress;
    std::function<void(const Progress&)> onProgress;
    // Explicit test seam; production never reads fault-injection environment variables.
    std::function<int(Fault, const std::string&, std::uintmax_t)> fault;
    void report(Phase phase, const std::string& item) {
        progress.phase = phase;
        progress.item = item;
        if (onProgress) onProgress(progress);
    }
    void checkpoint() const {
        if (cancel) throw std::runtime_error("操作已取消");
    }
    void inject(Fault point, const std::string& path) const {
        if (fault) {
            const int error = fault(point, path, progress.bytes);
            if (error) throw std::runtime_error(std::strerror(error));
        }
    }
};

class Fd {
public:
    explicit Fd(int fd = -1) : m_fd(fd) {}
    ~Fd() {
        if (m_fd >= 0) ::close(m_fd);
    }
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (m_fd >= 0) ::close(m_fd);
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }
    void closeChecked() {
        const int fd = std::exchange(m_fd, -1);
        if (::close(fd) != 0) throw std::runtime_error(std::strerror(errno));
    }

private:
    int m_fd;
};
inline void check(bool ok) {
    if (!ok) throw std::runtime_error(std::strerror(errno));
}
inline Fd directory(const std::string& path) {
    Fd fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    check(fd.get() >= 0);
    return fd;
}
inline Fd childDirectory(int parent, const std::string& name) {
    Fd fd(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    check(fd.get() >= 0);
    return fd;
}
inline int renameNoReplace(int fromFd, const std::string& from, int toFd, const std::string& to) {
    return static_cast<int>(::syscall(SYS_renameat2, fromFd, from.c_str(), toFd, to.c_str(), 1u));
}
inline bool renameNoReplace(const std::string& from, const std::string& to, std::string& error) {
    try {
        auto src = directory(fs::path(from).parent_path().string());
        auto dst = directory(fs::path(to).parent_path().string());
        check(renameNoReplace(src.get(), fs::path(from).filename().string(), dst.get(),
                              fs::path(to).filename().string()) == 0);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

struct Identity {
    std::uintmax_t device = 0, inode = 0, size = 0;
    unsigned mode = 0;
    timespec modified{};
    bool sameObject(const Identity& other) const {
        return device == other.device && inode == other.inode && (mode & S_IFMT) == (other.mode & S_IFMT);
    }
    bool sameVersion(const Identity& other) const {
        return sameObject(other) && mode == other.mode &&
               (S_ISDIR(mode) || (size == other.size && modified.tv_sec == other.modified.tv_sec &&
                                  modified.tv_nsec == other.modified.tv_nsec));
    }
};
inline Identity identity(const struct stat& st) {
    return {static_cast<std::uintmax_t>(st.st_dev), static_cast<std::uintmax_t>(st.st_ino),
            static_cast<std::uintmax_t>(st.st_size), static_cast<unsigned>(st.st_mode), st.st_mtim};
}
inline Identity identityAt(int parent, const std::string& name) {
    struct stat st{};
    check(::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0);
    return identity(st);
}
inline std::vector<std::string> children(int fd) {
    // dup() would share a directory cursor with fd and silently skip entries on a second scan.
    Fd copy(::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    check(copy.get() >= 0);
    DIR* raw = ::fdopendir(::dup(copy.get()));
    check(raw != nullptr);
    std::unique_ptr<DIR, int (*)(DIR*)> dir(raw, ::closedir);
    std::vector<std::string> names;
    errno = 0;
    while (auto* e = ::readdir(dir.get())) {
        std::string name(e->d_name);
        if (name != "." && name != "..") names.push_back(std::move(name));
        errno = 0;
    }
    check(errno == 0);
    std::sort(names.begin(), names.end());
    return names;
}
struct Node {
    std::string path;
    Identity id;
};
using Stamp = std::vector<Node>;
inline void scanAt(int parent, const std::string& name, const std::string& relative, Stamp& out, Control* control) {
    if (control) control->checkpoint();
    const auto id = identityAt(parent, name);
    out.push_back({relative, id});
    if (S_ISDIR(id.mode)) {
        auto fd = childDirectory(parent, name);
        struct stat opened{};
        check(::fstat(fd.get(), &opened) == 0);
        if (!id.sameObject(identity(opened))) throw std::runtime_error("文件夹在操作期间已改变");
        for (const auto& child : children(fd.get())) scanAt(fd.get(), child, relative + "/" + child, out, control);
    } else if (!S_ISREG(id.mode) && !S_ISLNK(id.mode)) {
        throw std::runtime_error("不支持复制设备、套接字或命名管道");
    }
}
inline Stamp stampAt(int parent, const std::string& name, Control* control = nullptr, bool recursive = true) {
    Stamp out;
    if (recursive)
        scanAt(parent, name, "", out, control);
    else
        out.push_back({"", identityAt(parent, name)});
    return out;
}
inline Stamp stamp(const std::string& path, bool recursive = true) {
    auto parent = directory(fs::path(path).parent_path().string());
    return stampAt(parent.get(), fs::path(path).filename().string(), nullptr, recursive);
}
inline bool matches(const Stamp& expected, const Stamp& actual) {
    if (expected.size() != actual.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (expected[i].path != actual[i].path || !expected[i].id.sameVersion(actual[i].id)) return false;
    return true;
}
inline void requireMatch(const Stamp& expected, const Stamp& actual) {
    if (!matches(expected, actual)) throw std::runtime_error("项目已被修改或替换；为保护其他文件，未继续操作");
}

// Only used below an exclusive 0700 sibling directory owned by this transaction.
inline void cleanPrivate(int parent, const std::string& name) {
    struct stat st{};
    if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        check(::fchmodat(parent, name.c_str(), 0700, 0) == 0);
        auto fd = childDirectory(parent, name);
        for (const auto& child : children(fd.get())) cleanPrivate(fd.get(), child);
        check(::unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0);
    } else
        check(::unlinkat(parent, name.c_str(), 0) == 0);
}
class Stage {
public:
    explicit Stage(const std::string& parent) : m_parent(directory(parent)) {
        // mkdirat is exclusive, and the open directory pins the selected destination even if renamed.
        static std::atomic<unsigned long> serial{0};
        for (;;) {
            m_name = ".forma-transaction-";
            m_name += std::to_string(::getpid());
            m_name += '-';
            m_name += std::to_string(++serial);
            if (::mkdirat(m_parent.get(), m_name.c_str(), 0700) == 0) break;
            if (errno != EEXIST) check(false);
        }
        m_fd = childDirectory(m_parent.get(), m_name);
        m_path = (fs::path(parent) / m_name).string();
    }
    ~Stage() {
        if (m_preserve) return;
        try {
            struct stat opened{};
            check(::fstat(m_fd.get(), &opened) == 0);
            if (identity(opened).sameObject(identityAt(m_parent.get(), m_name))) cleanPrivate(m_parent.get(), m_name);
        } catch (...) { /* Never clean a substituted sibling directory. */
        }
    }
    int fd() const { return m_fd.get(); }
    int parentFd() const { return m_parent.get(); }
    const std::string& path() const { return m_path; }
    void preserve() { m_preserve = true; }

private:
    Fd m_parent, m_fd;
    std::string m_name, m_path;
    bool m_preserve = false;
};
inline void copyAt(int srcParent, const std::string& name, int dstParent, const std::string& dstName,
                   Control& control) {
    control.checkpoint();
    const auto before = identityAt(srcParent, name);
    struct stat st{};
    check(::fstatat(srcParent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0);
    const timespec times[2] = {st.st_atim, st.st_mtim};
    if (S_ISDIR(before.mode)) {
        auto src = childDirectory(srcParent, name);
        check(::mkdirat(dstParent, dstName.c_str(), 0700) == 0);
        auto dst = childDirectory(dstParent, dstName);
        for (const auto& child : children(src.get())) copyAt(src.get(), child, dst.get(), child, control);
        check(::fchmod(dst.get(), before.mode & 07777) == 0);
        check(::futimens(dst.get(), times) == 0);
    } else if (S_ISLNK(before.mode)) {
        std::vector<char> text(static_cast<size_t>(before.size) + 2);
        const auto n = ::readlinkat(srcParent, name.c_str(), text.data(), text.size());
        check(n >= 0);
        if (static_cast<size_t>(n) >= text.size()) throw std::runtime_error("符号链接在复制期间已改变");
        text[static_cast<size_t>(n)] = '\0';
        check(::symlinkat(text.data(), dstParent, dstName.c_str()) == 0);
        check(::utimensat(dstParent, dstName.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
    } else if (S_ISREG(before.mode)) {
        Fd src(::openat(srcParent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        check(src.get() >= 0);
        struct stat opened{};
        check(::fstat(src.get(), &opened) == 0);
        if (!before.sameVersion(identity(opened))) throw std::runtime_error("来源在复制期间已改变");
        Fd dst(::openat(dstParent, dstName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        check(dst.get() >= 0);
        std::array<char, 256 * 1024> buffer{};
        for (;;) {
            control.checkpoint();
            ssize_t n = ::read(src.get(), buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            check(n >= 0);
            if (n == 0) break;
            size_t offset = 0;
            while (offset < static_cast<size_t>(n)) {
                control.checkpoint();
                control.inject(Fault::Write, name);
                const auto written = ::write(dst.get(), buffer.data() + offset, static_cast<size_t>(n) - offset);
                if (written < 0 && errno == EINTR) continue;
                check(written > 0);
                offset += static_cast<size_t>(written);
                control.progress.bytes += static_cast<std::uintmax_t>(written);
                control.report(Phase::Copying, control.progress.item);
            }
        }
        check(::fstat(src.get(), &opened) == 0);
        if (!before.sameVersion(identity(opened))) throw std::runtime_error("来源在复制期间已改变");
        check(::fchmod(dst.get(), before.mode & 07777) == 0);
        check(::futimens(dst.get(), times) == 0);
        check(::fsync(dst.get()) == 0);
        dst.closeChecked();
    } else
        throw std::runtime_error("不支持复制设备、套接字或命名管道");
    if (!before.sameVersion(identityAt(srcParent, name))) throw std::runtime_error("来源在复制期间已改变");
}

struct Record {
    std::string source, target;
    Stamp targetStamp;
    std::optional<TrashedItem> replaced;
    Stamp replacedStamp;
    bool moved = false;
    bool shallowIdentity = false;  // synchronous rename pins only the root; it never deletes children
    bool incompleteMove = false;
    bool targetUndone = false;
};
struct Outcome {
    bool committed = false, sourceRemoved = false, cancelled = false;
    std::string error;
    std::optional<Record> record;
};
inline void verifiedRename(const std::string& from, const std::string& to, const Stamp& expected,
                           bool recursive = true) {
    auto src = directory(fs::path(from).parent_path().string());
    auto dst = directory(fs::path(to).parent_path().string());
    const auto a = fs::path(from).filename().string(), b = fs::path(to).filename().string();
    requireMatch(expected, stampAt(src.get(), a, nullptr, recursive));
    check(renameNoReplace(src.get(), a, dst.get(), b) == 0);
    try {
        requireMatch(expected, stampAt(dst.get(), b, nullptr, recursive));
    } catch (...) {
        // If a concurrent writer won the original path, retain the quarantined item, never overwrite it.
        if (renameNoReplace(dst.get(), b, src.get(), a) != 0)
            throw std::runtime_error("项目身份已改变，且原位置被占用；项目保留在 " + to);
        throw;
    }
}
inline void removeSourceAt(int parent, const std::string& name, const std::string& relative, const Stamp& expected,
                           Control& control) {
    const auto found =
        std::find_if(expected.begin(), expected.end(), [&](const Node& n) { return n.path == relative; });
    if (found == expected.end() || !found->id.sameVersion(identityAt(parent, name)))
        throw std::runtime_error("来源出现新文件或已改变，未删除它");
    control.inject(Fault::RemoveSource, relative);
    if (S_ISDIR(found->id.mode)) {
        auto fd = childDirectory(parent, name);
        for (const auto& child : children(fd.get()))
            removeSourceAt(fd.get(), child, relative + "/" + child, expected, control);
        check(::unlinkat(parent, name.c_str(), AT_REMOVEDIR) == 0);
    } else
        check(::unlinkat(parent, name.c_str(), 0) == 0);
}
inline void removeSource(const std::string& source, const Stamp& expected, Control& control) {
    Stage quarantine(fs::path(source).parent_path().string());
    const std::string saved = quarantine.path() + "/source";
    verifiedRename(source, saved, expected);
    // Source deletion is a non-cancellable safe-finishing phase. A failure retains all survivors.
    quarantine.preserve();
    try {
        requireMatch(expected, stampAt(quarantine.fd(), "source"));
        removeSourceAt(quarantine.fd(), "source", "", expected, control);
        cleanPrivate(quarantine.parentFd(), fs::path(quarantine.path()).filename().string());
    } catch (const std::exception& e) {
        std::string error = e.what();
        if (renameNoReplace(quarantine.fd(), "source", quarantine.parentFd(), fs::path(source).filename().string()) !=
            0)
            error += "；剩余来源保留在 " + saved;
        else {
            (void)::unlinkat(quarantine.parentFd(), fs::path(quarantine.path()).filename().c_str(), AT_REMOVEDIR);
        }
        throw std::runtime_error(error);
    }
}
inline Outcome transfer(const std::string& source, const std::string& target, bool move, bool replace, Control& control,
                        std::optional<Identity> expectedTarget = std::nullopt) {
    Outcome out;
    Record record;
    record.source = source;
    record.target = target;
    record.moved = move;
    try {
        control.report(Phase::Scanning, source);
        control.checkpoint();
        auto src = directory(fs::path(source).parent_path().string());
        const auto sourceName = fs::path(source).filename().string();
        const auto sourceStamp = stampAt(src.get(), sourceName, &control);
        control.progress.total = 0;
        control.progress.bytes = 0;
        for (const auto& n : sourceStamp)
            if (S_ISREG(n.id.mode)) control.progress.total += n.id.size;
        auto dst = directory(fs::path(target).parent_path().string());
        const auto targetName = fs::path(target).filename().string();
        // Reject aliasing through symlinked parents, not just lexically nested names.
        const auto sourceReal = fs::weakly_canonical(source);
        const auto targetReal = fs::weakly_canonical(fs::path(target).parent_path()) / targetName;
        auto a = sourceReal.begin(), b = targetReal.begin();
        for (; a != sourceReal.end() && b != targetReal.end() && *a == *b; ++a, ++b) {
        }
        if (a == sourceReal.end()) throw std::runtime_error("不能复制或移动到项目自身中");
        if (move && !replace) {
            control.checkpoint();
            control.report(Phase::Committing, source);
            int injected = control.fault ? control.fault(Fault::Rename, target, control.progress.bytes) : 0;
            if (!injected && renameNoReplace(src.get(), sourceName, dst.get(), targetName) == 0) {
                record.targetStamp = sourceStamp;
                out.committed = out.sourceRemoved = true;
                out.record = record;
                try {
                    requireMatch(sourceStamp, stampAt(dst.get(), targetName));
                } catch (const std::exception& e) {
                    out.error = e.what();
                    // Keep the identity-bound record and report the actual move even if a writer raced us.
                }
                return out;
            }
            const int error = injected ? injected : errno;
            if (error != EXDEV) throw std::runtime_error(std::strerror(error));
        }
        Stage stage(fs::path(target).parent_path().string());
        control.report(Phase::Copying, source);
        copyAt(src.get(), sourceName, stage.fd(), "payload", control);
        requireMatch(sourceStamp, stampAt(src.get(), sourceName, &control));
        record.targetStamp = stampAt(stage.fd(), "payload");
        control.checkpoint();
        control.report(Phase::Committing, source);
        if (replace) {
            struct stat existing{};
            if (::fstatat(dst.get(), targetName.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
                const auto actual = identity(existing);
                if (expectedTarget && (!expectedTarget->sameVersion(actual) ||
                                       expectedTarget->modified.tv_sec != actual.modified.tv_sec ||
                                       expectedTarget->modified.tv_nsec != actual.modified.tv_nsec))
                    throw std::runtime_error("确认后目标已改变，未替换它；请重新确认");
                const auto oldStamp = stampAt(dst.get(), targetName);
                std::string error;
                auto old = moveToTrash(target, &error);
                if (!old) throw std::runtime_error("无法保护被替换项目：" + error);
                record.replaced = *old;
                record.replacedStamp = stamp(old->trashed);
                requireMatch(oldStamp, record.replacedStamp);
            } else if (errno != ENOENT)
                check(false);
        }
        // Once the old target is protected, cancellation cannot interrupt commit or rollback.
        control.inject(Fault::Commit, target);
        check(renameNoReplace(stage.fd(), "payload", dst.get(), targetName) == 0);
        out.committed = true;
        // Rename preserves the staged object's identity; no fallible work may lose its undo record.
        out.record = record;
        if (move) {
            control.report(Phase::RemovingSource, source);
            try {
                removeSource(source, sourceStamp, control);
                out.sourceRemoved = true;
            } catch (const std::exception& e) {
                record.incompleteMove = true;
                out.error = "已复制，但来源未完全移走：" + std::string(e.what());
            }
        }
        out.record = record;
    } catch (const std::exception& e) {
        out.cancelled = std::string(e.what()) == "操作已取消";
        out.error = e.what();
        if (record.replaced) {
            control.report(Phase::Restoring, target);
            try {
                verifiedRename(record.replaced->trashed, target, record.replacedStamp);
                (void)::unlink(record.replaced->infoFile.c_str());
            } catch (const std::exception& restore) {
                out.error += "；旧目标仍保留在回收站，恢复失败：" + std::string(restore.what());
                record.targetUndone = true;
                out.record = record;
            }
        }
    }
    return out;
}
inline bool undo(Record& record, Control& control, std::string& error) {
    try {
        control.report(Phase::Restoring, record.target);
        control.checkpoint();
        if (!record.targetUndone) {
            if (record.incompleteMove)
                throw std::runtime_error("来源未完全移走；请先核对两处文件，不能安全撤销此部分移动");
            requireMatch(record.targetStamp, stamp(record.target, !record.shallowIdentity));
            if (record.moved) {
                struct stat occupied{};
                if (::lstat(record.source.c_str(), &occupied) == 0)
                    throw std::runtime_error("原位置已有其他项目，未覆盖它");
                if (errno != ENOENT) check(false);
                control.inject(Fault::Restore, record.source);
                std::string renameError;
                // Returning across file systems uses the same staged, no-clobber transaction.
                auto returned = transfer(record.target, record.source, true, false, control);
                if (!returned.committed || !returned.sourceRemoved) throw std::runtime_error(returned.error);
            } else {
                control.inject(Fault::Restore, record.target);
                std::string trashError;
                if (!moveToTrash(record.target, &trashError)) throw std::runtime_error(trashError);
            }
            record.targetUndone = true;
        }
        if (record.replaced) {
            control.inject(Fault::Restore, record.replaced->original);
            verifiedRename(record.replaced->trashed, record.replaced->original, record.replacedStamp);
            (void)::unlink(record.replaced->infoFile.c_str());
            record.replaced.reset();
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}  // namespace finder::transaction
