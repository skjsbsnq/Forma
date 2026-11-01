#include <doctest/doctest.h>

#include <thread>

#include "core/app_impl.hpp"
#include "mayonnaise/app.hpp"

TEST_CASE("A background app receives posted work without creating graphics services") {
    // No display is needed: the resident IPC/polkit loop must work before its first UI request.
    mayo::App app("forma-event-only-test", {}, {.background = true});
    app.settings().setReadOnly(true);
    app.setKeepAlive(true);
    bool received = false;
    std::thread sender([&] {
        app.post([&] {
            received = true;
            CHECK(app.impl().windows.empty());
            CHECK_FALSE(app.impl().device);
            CHECK_FALSE(app.impl().renderer);
            CHECK_FALSE(app.impl().text);
            app.quit();
        });
    });
    CHECK(app.run() == 0);
    sender.join();
    CHECK(received);
}
