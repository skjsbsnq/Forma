#include "applications.hpp"
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
    fs::remove_all(root);
}
