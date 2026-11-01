// Exercise the real controller and IPC with synthetic pixels; never capture the desktop.
#include <fstream>
#include "applications/screenshot/capture_view.hpp"
#include "core/app_impl.hpp"
#include "core/window_impl.hpp"
namespace shot {
Image fixtureCapture(bool, std::stop_token = {});
}
#define capture fixtureCapture
#define main screenshotMain
#include "applications/screenshot/main.cpp"
#undef main
#undef capture
namespace shot {
Image fixtureCapture(bool, std::stop_token stop) {
    if (stop.stop_requested()) throw CaptureCancelled{};
    std::ofstream(std::getenv("SHOT_FIXTURE_TRACE"), std::ios::app) << "capture\n";
    mayo::App::current().after(.35, [] {
        auto& app = mayo::App::current();
        if (!app.impl().windows.empty()) {
            auto& window = *app.impl().windows.front();
            mayo::platform::PlatformEvent event;
            event.type = mayo::platform::EventType::WindowClose;
            event.window = window.impl().platformWindow->id();
            app.impl().dispatch(event);
        }
    });
    return {2560, 1600, 1.0, std::vector<std::uint8_t>(2560 * 1600 * 4, 255)};
}
}  // namespace shot
int main(int argc, char** argv) {
    std::ofstream(std::getenv("SHOT_FIXTURE_TRACE"), std::ios::app) << "start\n";
    return screenshotMain(argc, argv);
}
