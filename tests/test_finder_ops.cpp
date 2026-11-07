#include "file_transaction.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace tx = finder::transaction;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    require(!stream.fail(), "scratch write");
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
bool present(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
struct Scratch {
    fs::path path;
    std::string previousData;
    bool hadData = false;
    explicit Scratch(const fs::path& parent) {
        std::string pattern = (parent / "forma-finder-ops-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "mkdtemp scratch");
        path = pattern;
        if (const char* value = std::getenv("XDG_DATA_HOME")) {
            hadData = true;
            previousData = value;
        }
        fs::create_directory(path / "data");
        require(::setenv("XDG_DATA_HOME", (path / "data").c_str(), 1) == 0, "isolated Trash");
    }
    ~Scratch() {
        if (hadData)
            ::setenv("XDG_DATA_HOME", previousData.c_str(), 1);
        else
            ::unsetenv("XDG_DATA_HOME");
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
void noStages(const fs::path& path) {
    for (const auto& entry : fs::directory_iterator(path))
        require(entry.path().filename().string().find(".forma-transaction-") != 0, "unpublished staging cleaned");
}
}  // namespace

// The caller passes its own mkdtemp root. This function creates a second exclusive subtree and
// temporarily isolates XDG_DATA_HOME; EXDEV and I/O failures are hooks, never real devices/full disks.
void testFinderOps(const std::filesystem::path& root) {
    Scratch scratch(root);
    const auto folder = scratch.path / "files";
    fs::create_directory(folder);
    const auto source = folder / "source", target = folder / "target";
    auto reset = [&] {
        fs::remove_all(folder);
        fs::create_directory(folder);
        write(source, std::string(700000, 's'));
    };
    reset();
    {
        write(target, "stranger");
        tx::Control control;
        auto result = tx::transfer(source, target, true, false, control);
        require(!result.committed && read(target) == "stranger" && present(source),
                "move cannot clobber existing target");
        fs::remove(target);
        fs::create_symlink("missing", target);
        require(finder::uniqueName(folder, "target") != "target", "broken links reserve names");
        result = tx::transfer(source, target, false, false, control);
        require(!result.committed && fs::is_symlink(target) && present(source), "copy cannot clobber broken link");
        noStages(folder);
    }
    reset();
    {
        tx::Control control;
        control.fault = [](tx::Fault fault, const std::string&, std::uintmax_t bytes) {
            return fault == tx::Fault::Write && bytes > 0 ? ENOSPC : 0;
        };
        auto result = tx::transfer(source, target, false, false, control);
        require(!result.committed && !result.error.empty() && !present(target) && read(source).size() == 700000,
                "injected short-copy failure preserves source and removes staging");
        noStages(folder);
    }
    reset();
    {
        tx::Control control;
        control.onProgress = [&](const tx::Progress& progress) {
            if (progress.bytes) control.cancel = true;
        };
        auto result = tx::transfer(source, target, false, false, control);
        require(result.cancelled && !result.committed && present(source) && !present(target),
                "chunk cancellation is safe");
        noStages(folder);
        tx::Control queued;
        queued.cancel = true;
        result = tx::transfer(source, target, true, false, queued);
        require(result.cancelled && present(source) && !present(target), "pre-start cancellation touches nothing");
    }
    reset();
    {
        tx::Control control;
        bool changed = false;
        control.onProgress = [&](const tx::Progress& progress) {
            if (progress.bytes && !changed) {
                changed = true;
                write(source, "external edit");
            }
        };
        auto result = tx::transfer(source, target, false, false, control);
        require(!result.committed && read(source) == "external edit" && !present(target),
                "source mutation aborts publication");
        noStages(folder);
    }
    reset();
    {
        tx::Control control;
        control.fault = [](tx::Fault fault, const std::string&, std::uintmax_t) {
            return fault == tx::Fault::Rename ? EXDEV : 0;
        };
        auto result = tx::transfer(source, target, true, false, control);
        require(result.committed && result.sourceRemoved && !present(source) && read(target).size() == 700000,
                "injected EXDEV move completes");
        auto record = *result.record;
        write(source, "other new file");
        std::string error;
        require(!tx::undo(record, control, error) && read(source) == "other new file" && present(target),
                "undo conflict preserves both objects");
        require(!record.targetUndone, "failed undo remains retryable");
        fs::remove(source);
        require(tx::undo(record, control, error) && present(source) && !present(target), "undo retry after conflict");
        noStages(folder);
    }
    reset();
    {
        tx::Control control;
        control.fault = [](tx::Fault fault, const std::string&, std::uintmax_t) {
            if (fault == tx::Fault::Rename) return EXDEV;
            return fault == tx::Fault::RemoveSource ? EACCES : 0;
        };
        auto result = tx::transfer(source, target, true, false, control);
        require(result.committed && !result.sourceRemoved && result.record->incompleteMove &&
                    result.error.find("已复制") != std::string::npos && present(source) && present(target),
                "delete failure is not a successful move");
        std::string error;
        auto record = *result.record;
        require(!tx::undo(record, control, error) && present(source) && present(target),
                "incomplete move cannot discard the surviving full copy");
        noStages(folder);
    }
    reset();
    {
        fs::remove(source);
        fs::create_directory(source);
        write(source / "a", "copied");
        auto sourceFd = tx::directory(source);
        tx::Control control;
        bool inserted = false;
        control.fault = [&](tx::Fault fault, const std::string&, std::uintmax_t) {
            if (fault == tx::Fault::Rename) return EXDEV;
            if (fault == tx::Fault::RemoveSource && !inserted) {
                inserted = true;
                tx::Fd file(::openat(sourceFd.get(), "new", O_WRONLY | O_CREAT | O_EXCL, 0600));
                require(file.get() >= 0, "insert only through scratch directory fd");
                require(::write(file.get(), "other", 5) == 5, "scratch concurrent writer");
            }
            return 0;
        };
        auto result = tx::transfer(source, target, true, false, control);
        require(result.committed && !result.sourceRemoved && read(source / "new") == "other" && present(target / "a"),
                "source deletion preserves concurrently inserted child");
        noStages(folder);
    }
    reset();
    {
        ::chmod(source.c_str(), 0751);
        const timespec times[2] = {{1700000000, 123456789}, {1700000000, 123456789}};
        require(::utimensat(AT_FDCWD, source.c_str(), times, 0) == 0, "scratch timestamp");
        tx::Control control;
        auto result = tx::transfer(source, target, false, false, control);
        struct stat st{};
        require(::stat(target.c_str(), &st) == 0 && (st.st_mode & 07777) == 0751 && st.st_mtim.tv_nsec == 123456789,
                "permissions and nanosecond mtime preserved");
        auto record = *result.record;
        fs::rename(target, folder / "saved-copy");
        write(target, "other identity");
        std::string error;
        require(!tx::undo(record, control, error) && read(target) == "other identity",
                "undo refuses a substituted target identity");
        require(result.committed, "copy published");
    }
    reset();
    {
        fs::remove(source);
        fs::create_directory(source);
        write(source / "file", "content");
        fs::create_symlink("file", source / "link");
        fs::create_symlink("absent", source / "broken");
        tx::Control control;
        auto result = tx::transfer(source, target, false, false, control);
        require(result.committed && fs::read_symlink(target / "link") == "file" &&
                    fs::read_symlink(target / "broken") == "absent",
                "symbolic links copied as links without dereferencing");
        write(target / "new", "external child");
        auto record = *result.record;
        std::string error;
        require(!tx::undo(record, control, error) && read(target / "new") == "external child",
                "copy undo preserves new directory children");
        auto nested = tx::transfer(source, source / "nested", false, false, control);
        require(!nested.committed && !present(source / "nested"), "cannot copy directory inside itself");
    }
    reset();
    {
        fs::remove(source);
        require(::mkfifo(source.c_str(), 0600) == 0, "scratch fifo");
        tx::Control control;
        auto result = tx::transfer(source, target, false, false, control);
        require(!result.committed && present(source) && !present(target), "special files refused without blocking");
    }
    reset();
    {
        tx::Control control;
        control.fault = [&](tx::Fault fault, const std::string&, std::uintmax_t) {
            if (fault == tx::Fault::Commit) write(target, "concurrent commit winner");
            return 0;
        };
        auto result = tx::transfer(source, target, false, false, control);
        require(!result.committed && read(target) == "concurrent commit winner" && present(source),
                "no-clobber publication protects concurrent target");
        noStages(folder);
    }
    reset();
    {
        write(target, "confirmed target");
        const auto confirmed = tx::stamp(target).front().id;
        fs::rename(target, folder / "old-confirmed");
        write(target, "new unconfirmed target");
        tx::Control control;
        auto result = tx::transfer(source, target, false, true, control, confirmed);
        require(!result.committed && read(target) == "new unconfirmed target" && present(source),
                "replacement pins confirmed target identity");
        noStages(folder);
    }
    reset();
    {
        write(target, "old target");
        tx::Control control;
        control.onProgress = [&](const tx::Progress& progress) {
            if (progress.bytes) control.cancel = true;
        };
        auto result = tx::transfer(source, target, true, true, control);
        require(result.cancelled && read(target) == "old target" && present(source),
                "replacement copy cancellation leaves old target untouched");
        noStages(folder);
    }
    for (bool move : {false, true}) {
        reset();
        write(target, "old target");
        tx::Control control;
        control.fault = [](tx::Fault fault, const std::string&, std::uintmax_t) {
            return fault == tx::Fault::Commit ? EIO : 0;
        };
        auto result = tx::transfer(source, target, move, true, control);
        require(!result.committed && read(target) == "old target" && read(source).size() == 700000,
                "replacement failure restores old target");
        noStages(folder);
    }
    reset();
    {
        write(target, "old target");
        tx::Control control;
        control.fault = [&](tx::Fault fault, const std::string&, std::uintmax_t) {
            if (fault == tx::Fault::Commit) {
                write(target, "stranger after preservation");
                return EIO;
            }
            return 0;
        };
        auto result = tx::transfer(source, target, false, true, control);
        require(!result.committed && result.record && result.record->targetUndone && present(source) &&
                    read(target) == "stranger after preservation" &&
                    read(result.record->replaced->trashed) == "old target",
                "failed replacement rollback cannot overwrite concurrent target");
        auto record = *result.record;
        tx::Control retry;
        std::string error;
        require(!tx::undo(record, retry, error) && read(target) == "stranger after preservation",
                "retained rollback record refuses conflict");
        fs::remove(target);
        require(tx::undo(record, retry, error) && read(target) == "old target",
                "retained rollback record restores on retry");
        noStages(folder);
    }
    for (bool move : {false, true}) {
        reset();
        write(target, "old target");
        tx::Control control;
        auto result = tx::transfer(source, target, move, true, control);
        require(result.committed && (!move || result.sourceRemoved) &&
                    read(result.record->replaced->trashed) == "old target",
                "successful replacement keeps old target in isolated Trash");
        auto record = *result.record;
        bool failRestore = true;
        control.fault = [&](tx::Fault fault, const std::string& path, std::uintmax_t) {
            return fault == tx::Fault::Restore && path == target.string() && failRestore ? EIO : 0;
        };
        std::string error;
        require(!tx::undo(record, control, error), "replacement undo fault retained");
        failRestore = false;
        require(tx::undo(record, control, error) && read(target) == "old target" && present(source),
                "replacement undo retry restores both source and old target");
    }
    reset();
    {
        write(target, "old target");
        tx::Control control;
        control.fault = [&](tx::Fault fault, const std::string&, std::uintmax_t) {
            if (fault == tx::Fault::Commit) control.cancel = true;
            return 0;
        };
        auto result = tx::transfer(source, target, true, true, control);
        require(result.committed && result.sourceRemoved && !present(source) && read(target).size() == 700000,
                "late cancellation never interrupts protected commit and source cleanup");
        noStages(folder);
    }
    std::cout << "Finder transaction scratch tests passed\n";
}

#ifdef FINDER_OPS_STANDALONE
int main() {
    try {
        testFinderOps("/tmp");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Finder ops test failed: " << e.what() << '\n';
        return 1;
    }
}
#endif
