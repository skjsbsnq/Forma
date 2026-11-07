#pragma once

// Background file operations with undo, a folder watcher (inotify) and recursive search. Work happens on
// worker threads; every callback runs on the UI thread (App::post).

#include <mayonnaise/mayonnaise.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "file_model.hpp"
#include "file_transaction.hpp"

namespace finder {

class FileOps {
public:
    using Done = std::function<void(std::vector<std::string> results)>;  ///< new paths of the affected items
    using ErrorFn = std::function<void(const std::string& message)>;

    FileOps(mayo::App& app, ErrorFn onError);
    ~FileOps();
    FileOps(const FileOps&) = delete;
    FileOps& operator=(const FileOps&) = delete;

    /// Copies into `folder`. Items copied into their own folder get "copy" names (Duplicate), name clashes
    /// elsewhere keep both ("name 2").
    void copy(std::vector<std::string> sources, std::string folder, Done done = {});
    void move(std::vector<std::string> sources, std::string folder, Done done = {});
    struct Replacement {
        std::string target;
        transaction::Identity identity;
    };
    /// One compound transaction per item. Replacement identities come from the confirmation dialog.
    void transfer(std::vector<std::string> sources, std::string folder, bool move, bool replace, Done done = {},
                  std::vector<Replacement> replacements = {});
    void trash(std::vector<std::string> paths, Done done = {});
    void putBack(std::vector<std::string> trashedPaths, Done done = {});
    void emptyTrash(Done done = {});
    /// Permanent; only offered for items already in the Trash.
    void deleteImmediately(std::vector<std::string> paths, Done done = {});
    /// Synchronous (a rename is a single syscall); returns the new path or "" after reporting the error.
    std::string rename(const std::string& path, const std::string& newName);
    std::string newFolder(const std::string& folder);
    /// Total size and item count of a folder, computed in the background.
    void folderSize(const std::string& path, std::function<void(std::uintmax_t bytes, size_t items)> done);

    [[nodiscard]] bool canUndo() const { return !busy() && !m_undo.empty(); }
    [[nodiscard]] std::string undoTitle() const;
    void undo(Done done = {});

    [[nodiscard]] bool busy() const { return m_running > 0; }
    [[nodiscard]] const std::string& status() const { return m_status; }
    [[nodiscard]] const transaction::Progress& progress() const { return m_progress; }
    [[nodiscard]] bool cancelling() const { return m_cancelling; }
    /// Valid in Done: source paths whose transfer completed, excluding partial cross-device moves.
    [[nodiscard]] const std::vector<std::string>& completedSources() const { return m_completedSources; }
    /// Cancels queued work and asks the active operation to stop at its next safe checkpoint.
    void cancel();
    /// Called (UI thread) when busy()/status()/progress() change and after every operation that touched files.
    std::function<void()> onChanged;

private:
    struct Undo {
        std::vector<transaction::Record> items;
        std::string title;
    };
    struct Result {
        std::vector<std::string> paths;
        std::vector<std::string> sources;
        std::vector<std::string> errors;
        std::optional<Undo> undo;
        bool updatingUndo = false;
    };
    void run(std::string status, std::function<Result(transaction::Control&)> work, Done done);
    void worker();

    mayo::App& m_app;
    ErrorFn m_onError;
    std::vector<Undo> m_undo;
    int m_running = 0;
    std::string m_status;
    transaction::Progress m_progress;
    std::vector<std::string> m_completedSources;
    bool m_cancelling = false;
    std::uint64_t m_nextTask = 0, m_activeTask = 0;
    std::vector<std::shared_ptr<transaction::Control>> m_controls;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    bool m_stop = false;
    std::thread m_thread;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

/// Watches one folder and reports changes (coalesced) on the UI thread.
class FolderWatcher {
public:
    FolderWatcher(mayo::App& app, std::function<void()> onChange);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;
    void watch(const std::string& folder);

private:
    void run();
    mayo::App& m_app;
    std::function<void()> m_onChange;
    int m_fd = -1;
    int m_wake = -1;
    int m_wd = -1;
    std::atomic<bool> m_pending{false};
    std::thread m_thread;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

/// Recursive name search below a folder. Results arrive in batches; starting a new search (or
/// destroying the searcher) cancels the previous one.
class Searcher {
public:
    explicit Searcher(mayo::App& app) : m_app(app) {}
    ~Searcher();
    Searcher(const Searcher&) = delete;
    Searcher& operator=(const Searcher&) = delete;

    void start(const std::string& folder, const std::string& query, bool showHidden,
               std::function<void(std::vector<FileEntry> batch)> onBatch, std::function<void()> onFinished);
    void cancel();
    [[nodiscard]] bool running() const { return m_running; }

private:
    mayo::App& m_app;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::thread m_thread;
    bool m_running = false;
};

}  // namespace finder
