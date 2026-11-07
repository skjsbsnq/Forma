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
std::string quoted(const std::string& name) { return "“" + name + "”"; }
std::string describe(const std::vector<std::string>& paths) {
    return paths.size() == 1 ? quoted(fs::path(paths.front()).filename().string())
                             : std::to_string(paths.size()) + " 个项目";
}
transaction::Record movedRecord(const std::string& from, const std::string& to, bool shallowIdentity = false) {
    transaction::Record record;
    record.source = from;
    record.target = to;
    record.moved = true;
    record.shallowIdentity = shallowIdentity;
    record.targetStamp = transaction::stamp(to, !shallowIdentity);
    return record;
}
}  // namespace

FileOps::FileOps(App& app, ErrorFn onError) : m_app(app), m_onError(std::move(onError)) {
    m_thread = std::thread([this] { worker(); });
}
FileOps::~FileOps() {
    *m_alive = false;
    cancel();
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}
void FileOps::cancel() {
    for (const auto& control : m_controls) control->cancel = true;
    m_cancelling = busy();
    if (m_cancelling) m_status = "正在取消；等待安全收尾…";
    if (*m_alive && onChanged) onChanged();
}
void FileOps::worker() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        task();
    }
}
void FileOps::run(std::string status, std::function<Result(transaction::Control&)> work, Done done) {
    auto control = std::make_shared<transaction::Control>();
    control->cancel = m_cancelling;
    const auto id = ++m_nextTask;
    ++m_running;
    m_controls.push_back(control);
    if (m_running == 1) {
        m_status = status;
        m_progress = {};
        m_cancelling = false;
    }
    if (onChanged) onChanged();
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back([this, alive = m_alive, control, id, status, work = std::move(work), done = std::move(done)] {
            m_app.post([this, alive, id, status] {
                if (!*alive) return;
                m_activeTask = id;
                m_status = m_cancelling ? "正在取消；等待安全收尾…" : status;
                m_progress = {};
                if (onChanged) onChanged();
            });
            auto last = std::chrono::steady_clock::time_point{};
            auto phase = transaction::Phase::Queued;
            control->onProgress = [this, alive, id, &last, &phase](const transaction::Progress& progress) {
                const auto now = std::chrono::steady_clock::now();
                if (progress.phase == phase && now - last < std::chrono::milliseconds(80)) return;
                last = now;
                phase = progress.phase;
                m_app.post([this, alive, id, progress] {
                    if (!*alive || m_activeTask != id) return;
                    m_progress = progress;
                    if (onChanged) onChanged();
                });
            };
            Result result;
            try {
                // Queued cancellation still delivers Done and updates busy, without touching the disk.
                control->checkpoint();
                result = work(*control);
            } catch (const std::exception& e) {
                if (std::string(e.what()) != "操作已取消") result.errors.push_back(e.what());
            }
            control->onProgress = {};
            m_app.post([this, alive, control, result = std::move(result), done] {
                if (!*alive) return;
                --m_running;
                std::erase(m_controls, control);
                if (m_running == 0) {
                    m_status.clear();
                    m_progress = {};
                    m_cancelling = false;
                }
                if (result.updatingUndo && !m_undo.empty()) {
                    // Consume only successes; failed items (including half-restored replacements) remain.
                    if (result.undo && !result.undo->items.empty())
                        m_undo.back() = std::move(*result.undo);
                    else
                        m_undo.pop_back();
                } else if (result.undo)
                    m_undo.push_back(std::move(*result.undo));
                if (m_undo.size() > 50) m_undo.erase(m_undo.begin());
                if (!result.errors.empty() && m_onError) {
                    std::string msg = result.errors.front();
                    if (result.errors.size() > 1)
                        msg += "（另有 " + std::to_string(result.errors.size() - 1) + " 个错误）";
                    m_onError(msg);
                }
                m_completedSources = std::move(result.sources);
                if (done) done(result.paths);
                if (onChanged) onChanged();
            });
        });
    }
    m_cv.notify_one();
}
void FileOps::copy(std::vector<std::string> sources, std::string folder, Done done) {
    transfer(std::move(sources), std::move(folder), false, false, std::move(done));
}
void FileOps::move(std::vector<std::string> sources, std::string folder, Done done) {
    transfer(std::move(sources), std::move(folder), true, false, std::move(done));
}
void FileOps::transfer(std::vector<std::string> sources, std::string folder, bool move, bool replace, Done done,
                       std::vector<Replacement> replacements) {
    const std::string status = std::string(move ? "正在移动" : "正在复制") + describe(sources) + "…";
    run(
        status,
        [sources, folder, move, replace, replacements](transaction::Control& control) {
            Result result;
            Undo undo{{}, replace ? "撤销替换" : move ? "撤销移动" : "撤销复制"};
            for (const auto& source : sources) {
                if (control.cancel) break;
                const auto name = fs::path(source).filename().string();
                if (move && parentPath(source) == folder) {
                    result.paths.push_back(source);
                    result.sources.push_back(source);
                    continue;
                }
                const auto targetName = parentPath(source) == folder ? copyName(folder, name)
                                        : replace                    ? name
                                                                     : uniqueName(folder, name);
                const auto target = (fs::path(folder) / targetName).string();
                const auto old = std::find_if(replacements.begin(), replacements.end(),
                                              [&](const Replacement& item) { return item.target == target; });
                const bool doReplace = replace && old != replacements.end() && parentPath(source) != folder;
                const auto expected = doReplace ? std::optional<transaction::Identity>(old->identity) : std::nullopt;
                auto outcome = transaction::transfer(source, target, move, doReplace, control, expected);
                // Only complete moves count as clipboard successes. Incomplete copies remain visible on reload.
                if (outcome.committed && (!move || outcome.sourceRemoved)) {
                    result.paths.push_back(target);
                    result.sources.push_back(source);
                }
                if (outcome.record) undo.items.push_back(std::move(*outcome.record));
                if (!outcome.error.empty() && !outcome.cancelled)
                    result.errors.push_back("无法完成" + quoted(name) + "：" + outcome.error);
                if (outcome.cancelled) break;
            }
            if (!undo.items.empty()) result.undo = std::move(undo);
            return result;
        },
        std::move(done));
}
void FileOps::trash(std::vector<std::string> paths, Done done) {
    run(
        "正在移动" + describe(paths) + "到回收站…",
        [paths](transaction::Control& control) {
            Result result;
            Undo undo{{}, "撤销移到回收站"};
            for (const auto& path : paths) {
                if (control.cancel) break;
                control.report(transaction::Phase::Committing, path);
                std::string error;
                if (auto item = moveToTrash(path, &error)) {
                    undo.items.push_back(movedRecord(item->original, item->trashed));
                    result.paths.push_back(item->trashed);
                } else
                    result.errors.push_back("无法移到回收站：" + error);
            }
            if (!undo.items.empty()) result.undo = std::move(undo);
            return result;
        },
        std::move(done));
}
void FileOps::putBack(std::vector<std::string> paths, Done done) {
    run(
        "正在还原" + describe(paths) + "…",
        [paths](transaction::Control& control) {
            Result result;
            Undo undo{{}, "撤销还原"};
            for (const auto& path : paths) {
                if (control.cancel) break;
                auto original = trashOriginalPath(path);
                if (!original) {
                    result.errors.push_back("无法确定原始位置：" + path);
                    continue;
                }
                // Do not recreate removed ancestors or silently pick a new original location.
                auto outcome = transaction::transfer(path, *original, true, false, control);
                if (outcome.record) undo.items.push_back(std::move(*outcome.record));
                if (outcome.committed && outcome.sourceRemoved) {
                    result.paths.push_back(*original);
                    const fs::path info = fs::path(path).parent_path().parent_path() / "info" /
                                          (fs::path(path).filename().string() + ".trashinfo");
                    (void)::unlink(info.c_str());
                }
                if (!outcome.error.empty() && !outcome.cancelled) result.errors.push_back("无法还原：" + outcome.error);
            }
            if (!undo.items.empty()) result.undo = std::move(undo);
            return result;
        },
        std::move(done));
}
void FileOps::emptyTrash(Done done) {
    // Preserve undo records until actual deletion; stale records fail identity checks without data loss.
    run(
        "正在清空回收站…",
        [](transaction::Control& control) {
            Result result;
            std::error_code ec;
            const fs::path files(trashFilesDirectory());
            fs::directory_iterator it(files, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) result.errors.push_back(ec.message());
            std::vector<fs::path> paths;
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (control.cancel) break;
                paths.push_back(it->path());
            }
            if (ec) result.errors.push_back(ec.message());
            // A recovery quarantine created after this snapshot is never fed back into permanent deletion.
            for (const auto& path : paths) {
                if (control.cancel) break;
                control.report(transaction::Phase::RemovingSource, path.string());
                try {
                    transaction::removeSource(path.string(), transaction::stamp(path.string()), control);
                } catch (const std::exception& e) {
                    result.errors.push_back(e.what());
                    continue;
                }
                fs::remove(files.parent_path() / "info" / (path.filename().string() + ".trashinfo"), ec);
                if (ec) {
                    result.errors.push_back(ec.message());
                    ec.clear();
                }
            }
            return result;
        },
        std::move(done));
}
void FileOps::deleteImmediately(std::vector<std::string> paths, Done done) {
    run(
        "正在删除" + describe(paths) + "…",
        [paths](transaction::Control& control) {
            Result result;
            for (const auto& path : paths) {
                if (control.cancel) break;
                if (!isInTrash(path)) {
                    result.errors.push_back("永久删除只允许用于回收站中的项目");
                    continue;
                }
                control.report(transaction::Phase::RemovingSource, path);
                std::error_code ec;
                try {
                    transaction::removeSource(path, transaction::stamp(path), control);
                } catch (const std::exception& e) {
                    result.errors.push_back("无法删除：" + std::string(e.what()));
                    continue;
                }
                const auto info = fs::path(path).parent_path().parent_path() / "info" /
                                  (fs::path(path).filename().string() + ".trashinfo");
                fs::remove(info, ec);
                if (ec) result.errors.push_back(ec.message());
            }
            return result;
        },
        std::move(done));
}
std::string FileOps::rename(const std::string& path, const std::string& newName) {
    if (busy()) {
        if (m_onError) m_onError("请先等待当前文件操作完成。");
        return {};
    }
    if (newName.empty() || newName.find('\0') != std::string::npos || newName.find('/') != std::string::npos ||
        newName == "." || newName == "..") {
        if (m_onError) m_onError("名称不可为空、为“.”或“..”，也不能包含“/”或空字符。");
        return {};
    }
    const auto target = (fs::path(path).parent_path() / newName).string();
    try {
        const auto expected = transaction::stamp(path, false);
        transaction::verifiedRename(path, target, expected, false);
        m_undo.push_back({{movedRecord(path, target, true)}, "撤销重命名"});
    } catch (const std::exception& e) {
        if (m_onError) m_onError("无法重命名：" + std::string(e.what()));
        return {};
    }
    if (onChanged) onChanged();
    return target;
}
std::string FileOps::newFolder(const std::string& folder) {
    if (busy()) {
        if (m_onError) m_onError("请先等待当前文件操作完成。");
        return {};
    }
    const auto target = (fs::path(folder) / uniqueName(folder, "未命名文件夹")).string();
    std::error_code ec;
    if (!fs::create_directory(target, ec)) {
        if (m_onError) m_onError("无法创建文件夹：" + (ec ? ec.message() : std::string("名称已被其他项目使用")));
        return {};
    }
    transaction::Record record;
    record.target = target;
    record.targetStamp = transaction::stamp(target);
    m_undo.push_back({{std::move(record)}, "撤销新建文件夹"});
    if (onChanged) onChanged();
    return target;
}
void FileOps::folderSize(const std::string& path, std::function<void(std::uintmax_t, size_t)> done) {
    run("正在计算文件夹大小…",
        [path, done, this](transaction::Control& control) {
            std::uintmax_t bytes = 0;
            size_t items = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end && !control.cancel; it.increment(ec)) {
                ++items;
                if (it->is_regular_file(ec) && !it->is_symlink(ec)) bytes += it->file_size(ec);
            }
            if (!control.cancel)
                m_app.post([alive = m_alive, done, bytes, items] {
                    if (*alive) done(bytes, items);
                });
            return Result{};
        },
        {});
}
std::string FileOps::undoTitle() const { return m_undo.empty() ? "撤销" : m_undo.back().title; }
void FileOps::undo(Done done) {
    if (!canUndo()) return;
    const Undo saved = m_undo.back();
    run(
        "正在撤销…",
        [saved](transaction::Control& control) {
            Result result;
            result.updatingUndo = true;
            Undo remaining{{}, saved.title};
            for (auto item : saved.items) {
                std::string error;
                if (control.cancel || !transaction::undo(item, control, error)) {
                    remaining.items.push_back(std::move(item));
                    if (!error.empty() && error != "操作已取消") result.errors.push_back("无法撤销：" + error);
                } else {
                    if (item.moved)
                        result.paths.push_back(item.source);
                    else if (saved.title == "撤销替换")
                        result.paths.push_back(item.target);
                    // Clean metadata only after its tracked object has successfully left the Trash.
                    if (isInTrash(item.target)) {
                        const auto info = fs::path(item.target).parent_path().parent_path() / "info" /
                                          (fs::path(item.target).filename().string() + ".trashinfo");
                        (void)::unlink(info.c_str());
                    }
                }
            }
            if (!remaining.items.empty()) result.undo = std::move(remaining);
            return result;
        },
        std::move(done));
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
        const auto written = ::write(m_wake, &one, sizeof one);
        (void)written;
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
                if ((hidden && !showHidden) || p == "/proc" || p == "/sys" || p == "/dev" || p == "/run" ||
                    it.depth() > 14)
                    it.disable_recursion_pending();
            }
            if (hidden && !showHidden) continue;
            if (!containsNoCase(name, query)) continue;
            if (auto e = statEntry(it->path().string())) {
                batch.push_back(std::move(*e));
                if (++found >= kMaxResults) break;
            }
            if (batch.size() >= 200 || std::chrono::steady_clock::now() - last > std::chrono::milliseconds(120))
                flush();
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
