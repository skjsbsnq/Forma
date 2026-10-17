#include "file_ops.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace mayo;

namespace finder {

namespace {

std::string plural(size_t n, const char* what) {
    return std::to_string(n) + (std::string(what) == "item" ? " 个项目" : " 个其他错误");
}

std::string quoted(const std::string& name) { return "“" + name + "”"; }

std::string describe(const std::vector<std::string>& paths) {
    return paths.size() == 1 ? quoted(fs::path(paths.front()).filename().string()) : plural(paths.size(), "item");
}

/// rename(), falling back to copy + delete across file systems.
bool moveItem(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    if (ec != std::errc::cross_device_link) {
        error = ec.message();
        return false;
    }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        error = ec.message();
        fs::remove_all(to, ec);
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

}  // namespace

// --- FileOps -----------------------------------------------------------------------------------------

FileOps::FileOps(App& app, ErrorFn onError) : m_app(app), m_onError(std::move(onError)) {
    m_thread = std::thread([this] { worker(); });
}

FileOps::~FileOps() {
    *m_alive = false;
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void FileOps::worker() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            // Pending work still runs on shutdown: half-finished moves are worse than a short wait.
            if (m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}

void FileOps::run(std::string status, std::function<Result()> work, Done done) {
    ++m_running;
    m_status = std::move(status);
    if (onChanged) onChanged();
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back([this, alive = m_alive, work = std::move(work), done = std::move(done)] {
            Result result = work();
            m_app.post([this, alive, result = std::move(result), done] {
                if (!*alive) return;
                --m_running;
                if (m_running == 0) m_status.clear();
                if (result.undo) m_undo.push_back(*result.undo);
                if (m_undo.size() > 50) m_undo.erase(m_undo.begin());
                if (!result.errors.empty() && m_onError) {
                    std::string msg = result.errors.front();
                    if (result.errors.size() > 1) msg += "（另有 " + plural(result.errors.size() - 1, "more error") + "）";
                    m_onError(msg);
                }
                if (done) done(result.paths);
                if (onChanged) onChanged();
            });
        });
    }
    m_cv.notify_one();
}

void FileOps::copy(std::vector<std::string> sources, std::string folder, Done done) {
    std::string status = "正在复制" + describe(sources) + "…";
    run(std::move(status), [sources, folder] {
        Result r;
        Undo undo{Undo::Kind::Copy, {}, {}, {}, "撤销复制"};
        for (const auto& src : sources) {
            const std::string name = fs::path(src).filename().string();
            // Same folder: "photo copy.jpg" (Duplicate); elsewhere a clash keeps both ("photo 2.jpg").
            const std::string target = (fs::path(folder) / (parentPath(src) == folder ? copyName(folder, name)
                                                                                      : uniqueName(folder, name)))
                                           .string();
            if (isInside(folder, src)) {
                r.errors.push_back("无法将" + quoted(name) + "复制或移动到其自身中。");
                continue;
            }
            std::error_code ec;
            fs::copy(src, target, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            if (ec) {
                r.errors.push_back("无法复制" + quoted(name) + ": " + ec.message() + ".");
                continue;
            }
            r.paths.push_back(target);
            undo.created.push_back(target);
        }
        if (!undo.created.empty()) r.undo = undo;
        return r;
    }, std::move(done));
}

void FileOps::move(std::vector<std::string> sources, std::string folder, Done done) {
    std::string status = "正在移动" + describe(sources) + "…";
    run(std::move(status), [sources, folder] {
        Result r;
        Undo undo{Undo::Kind::Move, {}, {}, {}, "撤销移动"};
        for (const auto& src : sources) {
            const std::string name = fs::path(src).filename().string();
            if (parentPath(src) == folder) continue;
            if (isInside(folder, src)) {
                r.errors.push_back("无法将" + quoted(name) + "复制或移动到其自身中。");
                continue;
            }
            const std::string target = (fs::path(folder) / uniqueName(folder, name)).string();
            std::string error;
            if (!moveItem(src, target, error)) {
                r.errors.push_back("无法移动" + quoted(name) + ": " + error + ".");
                continue;
            }
            r.paths.push_back(target);
            undo.moves.emplace_back(src, target);
        }
        if (!undo.moves.empty()) r.undo = undo;
        return r;
    }, std::move(done));
}

void FileOps::trash(std::vector<std::string> paths, Done done) {
    std::string status = "正在移动" + describe(paths) + "到回收站…";
    run(std::move(status), [paths] {
        Result r;
        Undo undo{Undo::Kind::Trash, {}, {}, {}, "撤销移到回收站"};
        for (const auto& p : paths) {
            std::string error;
            if (auto item = moveToTrash(p, &error)) {
                undo.trashed.push_back(*item);
                r.paths.push_back(item->trashed);
            } else {
                r.errors.push_back("无法移动" + quoted(fs::path(p).filename().string()) + "到回收站：" + error + ".");
            }
        }
        if (!undo.trashed.empty()) r.undo = undo;
        return r;
    }, std::move(done));
}

void FileOps::putBack(std::vector<std::string> trashedPaths, Done done) {
    run("正在还原" + describe(trashedPaths) + "…", [trashedPaths] {
        Result r;
        Undo undo{Undo::Kind::Move, {}, {}, {}, "撤销还原"};
        for (const auto& p : trashedPaths) {
            auto original = trashOriginalPath(p);
            const std::string name = fs::path(p).filename().string();
            if (!original) {
                r.errors.push_back("无法确定" + quoted(name) + "的原始位置。");
                continue;
            }
            std::error_code ec;
            const fs::path dest(*original);
            fs::create_directories(dest.parent_path(), ec);
            const std::string target = (dest.parent_path() / uniqueName(dest.parent_path().string(), dest.filename().string())).string();
            std::string error;
            if (!moveItem(p, target, error)) {
                r.errors.push_back("无法还原" + quoted(name) + ": " + error + ".");
                continue;
            }
            const fs::path info = fs::path(p).parent_path().parent_path() / "info" / (name + ".trashinfo");
            fs::remove(info, ec);
            r.paths.push_back(target);
        }
        return r;
    }, std::move(done));
}

void FileOps::emptyTrash(Done done) {
    m_undo.erase(std::remove_if(m_undo.begin(), m_undo.end(), [](const Undo& u) { return u.kind == Undo::Kind::Trash; }),
                 m_undo.end());
    run("正在清空回收站…", [] {
        finder::emptyTrash();
        return Result{};
    }, std::move(done));
}

void FileOps::deleteImmediately(std::vector<std::string> paths, Done done) {
    run("正在删除" + describe(paths) + "…", [paths] {
        Result r;
        for (const auto& p : paths) {
            std::error_code ec;
            fs::remove_all(p, ec);
            if (ec) {
                r.errors.push_back("无法删除" + quoted(fs::path(p).filename().string()) + ": " + ec.message() + ".");
                continue;
            }
            const fs::path info = fs::path(p).parent_path().parent_path() / "info" / (fs::path(p).filename().string() + ".trashinfo");
            fs::remove(info, ec);
        }
        return r;
    }, std::move(done));
}

std::string FileOps::rename(const std::string& path, const std::string& newName) {
    if (newName.find('/') != std::string::npos || newName == "." || newName == "..") {
        if (m_onError) m_onError("名称" + quoted(newName) + "不可用。名称不能包含“/”。");
        return {};
    }
    const fs::path target = fs::path(path).parent_path() / newName;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        if (m_onError) m_onError("名称" + quoted(newName) + "已被使用，请选择其他名称。");
        return {};
    }
    fs::rename(path, target, ec);
    if (ec) {
        if (m_onError) m_onError("无法重命名" + quoted(fs::path(path).filename().string()) + ": " + ec.message() + ".");
        return {};
    }
    m_undo.push_back({Undo::Kind::Rename, {{path, target.string()}}, {}, {}, "撤销重命名"});
    if (onChanged) onChanged();
    return target.string();
}

std::string FileOps::newFolder(const std::string& folder) {
    const fs::path target = fs::path(folder) / uniqueName(folder, "未命名文件夹");
    std::error_code ec;
    fs::create_directory(target, ec);
    if (ec) {
        if (m_onError) m_onError("无法在此处创建文件夹：" + ec.message() + ".");
        return {};
    }
    m_undo.push_back({Undo::Kind::NewFolder, {}, {}, {target.string()}, "撤销新建文件夹"});
    if (onChanged) onChanged();
    return target.string();
}

void FileOps::folderSize(const std::string& path, std::function<void(std::uintmax_t, size_t)> done) {
    std::lock_guard lock(m_mutex);
    m_queue.push_back([this, alive = m_alive, path, done = std::move(done)] {
        std::uintmax_t bytes = 0;
        size_t items = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            ++items;
            if (it->is_regular_file(ec) && !it->is_symlink(ec)) bytes += it->file_size(ec);
        }
        m_app.post([alive, done, bytes, items] {
            if (*alive) done(bytes, items);
        });
    });
    m_cv.notify_one();
}

std::string FileOps::undoTitle() const { return m_undo.empty() ? "撤销" : m_undo.back().title; }

void FileOps::undo(Done done) {
    if (m_undo.empty()) return;
    Undo u = m_undo.back();
    m_undo.pop_back();
    run("正在撤销…", [u] {
        Result r;
        std::string error;
        switch (u.kind) {
            case Undo::Kind::Rename:
            case Undo::Kind::Move:
                for (const auto& [from, to] : u.moves) {
                    if (moveItem(to, from, error)) r.paths.push_back(from);
                    else r.errors.push_back("无法撤销：" + error + ".");
                }
                break;
            case Undo::Kind::Copy:
                // The Finder undoes a copy by moving the copies to the Trash.
                for (const auto& p : u.created) {
                    if (!moveToTrash(p, &error)) r.errors.push_back("无法撤销：" + error + ".");
                }
                break;
            case Undo::Kind::Trash:
                for (const auto& item : u.trashed) {
                    std::error_code ec;
                    fs::create_directories(fs::path(item.original).parent_path(), ec);
                    if (moveItem(item.trashed, item.original, error)) {
                        fs::remove(item.infoFile, ec);
                        r.paths.push_back(item.original);
                    } else {
                        r.errors.push_back("无法还原" + quoted(fs::path(item.original).filename().string()) + ": " + error + ".");
                    }
                }
                break;
            case Undo::Kind::NewFolder:
                for (const auto& p : u.created) {
                    std::error_code ec;
                    if (!fs::remove(p, ec) && !moveToTrash(p, &error)) r.errors.push_back("无法撤销：" + error + ".");
                }
                break;
        }
        return r;
    }, std::move(done));
}

// --- FolderWatcher -------------------------------------------------------------------------------------

FolderWatcher::FolderWatcher(App& app, std::function<void()> onChange) : m_app(app), m_onChange(std::move(onChange)) {
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    m_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_fd >= 0 && m_wake >= 0) m_thread = std::thread([this] { run(); });
}

FolderWatcher::~FolderWatcher() {
    *m_alive = false;
    if (m_wake >= 0) {
        std::uint64_t one = 1;
        (void)::write(m_wake, &one, sizeof one);
    }
    if (m_thread.joinable()) m_thread.join();
    if (m_fd >= 0) ::close(m_fd);
    if (m_wake >= 0) ::close(m_wake);
}

void FolderWatcher::watch(const std::string& folder) {
    if (m_fd < 0) return;
    if (m_wd >= 0) inotify_rm_watch(m_fd, m_wd);
    m_wd = folder == kRecents ? -1
                              : inotify_add_watch(m_fd, folder.c_str(),
                                                  IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
                                                      IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
}

void FolderWatcher::run() {
    alignas(inotify_event) char buf[4096];
    for (;;) {
        pollfd fds[2] = {{m_fd, POLLIN, 0}, {m_wake, POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0 && errno != EINTR) return;
        if (fds[1].revents) return;
        if (!(fds[0].revents & POLLIN)) continue;
        while (::read(m_fd, buf, sizeof buf) > 0) {
        }
        // Coalesce bursts (a large copy fires thousands of events) into one refresh a little later.
        if (!m_pending.exchange(true)) {
            m_app.after(0.15, [this, alive = m_alive] {
                if (!*alive) return;
                m_pending = false;
                m_onChange();
            });
        }
    }
}

// --- Searcher -----------------------------------------------------------------------------------------

Searcher::~Searcher() { cancel(); }

void Searcher::cancel() {
    if (m_cancel) *m_cancel = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void Searcher::start(const std::string& folder, const std::string& query, bool showHidden,
                     std::function<void(std::vector<FileEntry>)> onBatch, std::function<void()> onFinished) {
    cancel();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = true;
    auto cancelled = m_cancel;
    m_thread = std::thread([this, cancelled, folder, query, showHidden, onBatch, onFinished] {
        constexpr size_t kMaxResults = 2000;
        std::vector<FileEntry> batch;
        size_t found = 0;
        auto last = std::chrono::steady_clock::now();
        auto flush = [&] {
            if (batch.empty()) return;
            m_app.post([cancelled, onBatch, items = std::move(batch)] {
                if (!*cancelled) onBatch(items);
            });
            batch.clear();
            last = std::chrono::steady_clock::now();
        };
        std::error_code ec;
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
        for (fs::recursive_directory_iterator end; !ec && it != end && !*cancelled; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            const bool hidden = !name.empty() && name[0] == '.';
            if (it->is_directory(ec) && !it->is_symlink(ec)) {
                const std::string p = it->path().string();
                // Skip hidden trees and the kernel's pseudo file systems.
                if ((hidden && !showHidden) || p == "/proc" || p == "/sys" || p == "/dev" || p == "/run" || it.depth() > 14) {
                    it.disable_recursion_pending();
                }
            }
            if (hidden && !showHidden) continue;
            if (!containsNoCase(name, query)) continue;
            if (auto e = statEntry(it->path().string())) {
                batch.push_back(std::move(*e));
                if (++found >= kMaxResults) break;
            }
            if (batch.size() >= 200 || std::chrono::steady_clock::now() - last > std::chrono::milliseconds(120)) flush();
        }
        flush();
        m_app.post([this, cancelled, onFinished] {
            if (*cancelled) return;
            m_running = false;
            onFinished();
        });
    });
}

}  // namespace finder
