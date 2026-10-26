#include "applications.hpp"
#include "archives.hpp"
#include "file_model.hpp"
#include "file_panels.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char**) {
    finder::classifyInProcessOnly(true);
    if (argc > 1) {
        require(finder::defaultApplicationId(std::string(std::getenv("XDG_CONFIG_HOME")) + "/sample.txt") == "test.desktop",
                "default must survive a new process");
        return 0;
    }
    char temp[] = "/tmp/forma-finder-test-XXXXXX";
    require(::mkdtemp(temp) != nullptr, "temporary directory");
    const fs::path root(temp);
    ::setenv("XDG_CONFIG_HOME", temp, 1);
    ::setenv("XDG_CONFIG_DIRS", temp, 1);
    ::setenv("XDG_DATA_HOME", temp, 1);
    ::setenv("XDG_DATA_DIRS", temp, 1);
    ::setenv("XDG_CURRENT_DESKTOP", "Forma", 1);
    fs::create_directories(root / "applications");
    std::ofstream(root / "applications/test.desktop") << "[Desktop Entry]\nType=Application\nName=Test\nExec=/bin/true %f\nMimeType=text/plain;\n";
    const auto text = (root / "sample.txt").string();
    std::ofstream(text) << "hello\n";
    // A desktop's system association must not prevent a user override.
    ::setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
    std::ofstream(root / "applications/kde-mimeapps.list") << "[Default Applications]\ntext/plain=other.desktop;\n";
    require(finder::defaultApplicationProblem(text, "test.desktop").empty(), "system default permits user override");
    struct Host : mayo::test::TestHost {
        int shown = 0;
        void showOverlay(std::shared_ptr<mayo::Widget> overlay) override {
            ++shown;
            TestHost::showOverlay(overlay);
            const auto measured = overlay->measure(windowSize());
            auto* popup = dynamic_cast<mayo::PopupView*>(overlay.get());
            overlay->arrange(popup ? popup->placePopup(windowSize(), measured)
                                   : mayo::Rect{0, 0, measured.width, measured.height});
        }
    } host;
    auto entry = finder::statEntry(text);
    require(entry.has_value(), "file entry");
    auto panel = std::make_shared<finder::OpenWithPanel>(*entry, [] {});
    int opened = 0;
    panel->setOnOpen([&](const std::string& id, bool remember) {
        ++opened;
        require(remember, "checkbox label toggles remember");
        require(finder::rememberProblem(*entry, id).empty(), "choice can be saved");
        return finder::rememberApplication(*entry, id);
    });
    panel->present(host);
    auto click = [&](mayo::Point point) {
        host.dispatcher.pointerDown({point, mayo::MouseButton::Left, 1});
        host.dispatcher.pointerUp({point, mayo::MouseButton::Left, 1});
    };
    auto frame = panel->frame();
    click({frame.x + 110, frame.bottom() - 58});
    click({frame.x + 50, frame.y + 49});
    require(opened == 1, "clicking application commits once");
    require(!panel->isShown(), "chooser closes after save");
    auto failure = std::make_shared<finder::OpenWithPanel>(*entry, [] {});
    failure->setOnOpen([](const std::string&, bool) { return std::string("test failure"); });
    failure->present(host);
    frame = failure->frame();
    const int shownBefore = host.shown;
    click({frame.x + 50, frame.y + 49});
    require(host.shown == shownBefore + 1, "failed save displays error alert");
    require(finder::defaultApplicationId(text) == "test.desktop", "read saved default");
    const pid_t child = ::fork();
    require(child >= 0, "fork");
    if (child == 0) { ::execl("/proc/self/exe", "finder_open_tests", "verify", nullptr); ::_exit(127); }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh process verifies persistence");
    require(finder::launchWithDefaultApp(text).launched, "open with saved default");
    for (const auto* type : {"application/x-iso9660-appimage", "application/vnd.appimage", "application/x-appimage"})
        require(finder::isExecutableType(type), "AppImage MIME recognized");
    const auto image = (root / "Test.AppImage").string();
    std::ofstream(image) << "#!/bin/sh\nexit 0\n";
    ::chmod(image.c_str(), 0644);
    require(finder::launchWithDefaultApp(image).needsExecutableBit, "non-executable AppImage offers permission prompt");
    require(finder::setExecutable(image).empty(), "grant execute permission");
    require(finder::launchWithDefaultApp(image).launched, "execute AppImage after granting permission");

    // Open in Terminal: our own forma-terminal is first, exactly one terminal opens, and the folder is passed.
    // Every candidate terminal is a stub that records that it ran, since the test machine may have none.
    const auto folder = root / "terminal folder";
    const auto log = (root / "launched.log").string();
    fs::create_directories(folder);
    struct TerminalStub {
        const char* name;
        const char* prefix;
    };
    const TerminalStub stubs[] = {{"forma-terminal", "forma"}, {"xdg-terminal-exec", "xdg"}};
    for (const auto& stub : stubs) {
        const auto path = root / stub.name;
        std::ofstream(path) << "#!/bin/sh\nprintf '" << stub.prefix << " %s\\n' \"$PWD\" >> " << log << "\n";
        ::chmod(path.c_str(), 0755);
    }
    const std::string folderStr = folder.string();
    auto launchedLines = [&] {
        std::ifstream in(log);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    };
    auto waitForLaunch = [&](size_t count) {
        std::vector<std::string> lines;
        for (int i = 0; i < 100; ++i) {  // the terminal runs detached
            lines = launchedLines();
            if (lines.size() >= count) break;
            ::usleep(20000);
        }
        return lines;
    };
    ::setenv("PATH", root.c_str(), 1);  // only the stubs: nothing else can launch
    ::unsetenv("TERMINAL");
    require(finder::openTerminal(folderStr).empty(), "open in terminal reports success");
    auto lines = waitForLaunch(1);
    ::usleep(100000);
    lines = launchedLines();
    require(lines.size() == 1, "exactly one terminal opens, not one per installed terminal");
    require(lines.front() == "forma " + folderStr, "forma-terminal starts in the requested folder");
    fs::remove(log);
    fs::remove(root / "forma-terminal");
    require(finder::openTerminal(folderStr).empty(), "falls back to another terminal");
    lines = waitForLaunch(1);
    require(lines.size() == 1, "still exactly one terminal");
    require(lines.front() == "xdg " + folderStr, "the next terminal on PATH is used, in the folder");
    fs::remove(root / "xdg-terminal-exec");
    require(!finder::openTerminal(folderStr).empty(), "no terminal at all says why");

    // Extraction uses Ark's batch API, preserving names and rejecting mixed selections before launching.
    const auto zip = (root / "中文 空格 ' $() ;.zip").string();
    const auto rar = (root / "sample.rar").string();
    std::ofstream(zip) << "PK\x03\x04";
    std::ofstream(rar) << "Rar!\x1a\x07";
    fs::create_directory(root / "folder.zip");
    require(finder::canExtractArchives({zip, rar}), "zip and rar selection offers extraction");
    require(!finder::canExtractArchives({}), "empty selection has no extraction menu");
    require(!finder::canExtractArchives({zip, text}), "mixed selection has no extraction menu");
    require(!finder::canExtractArchives({(root / "folder.zip").string()}), "archive-named folder is not an archive");
    require(!finder::canExtractArchives({(root / "missing.zip").string()}), "missing archive is rejected");
    fs::create_directories(root / "Trash/files");
    const auto trashed = (root / "Trash/files/deleted.zip").string();
    std::ofstream(trashed) << "PK\x03\x04";
    require(!finder::canExtractArchives({trashed}), "trash has no extraction menu");
    const auto arkLog = root / "ark.log";
    const auto ark = root / "ark";
    std::ofstream(ark) << "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" << arkLog.string() << "'\n";
    ::chmod(ark.c_str(), 0755);
    auto arkArgs = [&](size_t count) {
        std::vector<std::string> args;
        for (int i = 0; i < 100; ++i) {
            args.clear();
            std::ifstream in(arkLog);
            for (std::string line; std::getline(in, line);) args.push_back(line);
            if (args.size() >= count) break;
            ::usleep(20000);
        }
        return args;
    };
    require(finder::extractArchives({zip}).empty(), "extract here launches Ark");
    require(arkArgs(5) == std::vector<std::string>{"--batch", "--destination", root.string(), "--", zip},
            "extract here uses the archive parent and preserves shell characters literally");
    fs::remove(arkLog);
    require(finder::extractArchives({zip, rar}, folderStr).empty(), "multiple archives extract to chosen folder");
    require(arkArgs(6) == std::vector<std::string>{"--batch", "--destination", folderStr, "--", zip, rar},
            "extract to uses the selected destination and all archives");
    fs::remove(arkLog);
    const auto nested = (folder / "--another.rar").string();
    fs::copy_file(rar, nested);
    require(finder::extractArchives({nested}).empty(), "extract here works outside the current folder");
    require(arkArgs(5) == std::vector<std::string>{"--batch", "--destination", folderStr, "--", nested},
            "extract here derives destination from the selected archive, not the view");
    require(!finder::extractArchives({zip}, text).empty(), "non-directory destination is rejected");
    require(!finder::extractArchives({text}).empty(), "ordinary file cannot launch extraction");
    fs::remove(ark);
    require(!finder::extractArchives({zip}).empty(), "missing Ark produces a visible error");
    fs::remove_all(root);
}
