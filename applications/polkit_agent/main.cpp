#include "backend.hpp"
#include "dialog.hpp"
#include "../app_store/json_lite.hpp"
#include <glib-unix.h>
#include <gio/gio.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
volatile std::sig_atomic_t quitting = 0;
void terminate(int) { quitting = 1; }

// NextKde's active bridge paints a complete window contour (including its own
// outline and shadow). Drawing our circular border inside its continuous curve
// exposes white crescents at the corners. Delegate only when that owner is active;
// other desktops and disabled NextKde appearance keep the client-drawn card.
bool nextKdeOwnsFrame() {
    if (!std::getenv("WAYLAND_DISPLAY")) return false;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return false;
    GVariant* reply = g_dbus_connection_call_sync(bus, "org.kde.KWin", "/Effects",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.kwin.Effects", "loadedEffects"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    g_object_unref(bus);
    if (!reply) return false;
    GVariant* effects = nullptr;
    g_variant_get(reply, "(v)", &effects);
    bool bridge = false;
    if (g_variant_is_of_type(effects, G_VARIANT_TYPE_STRING_ARRAY)) {
        GVariantIter iter;
        g_variant_iter_init(&iter, effects);
        const gchar* name = nullptr;
        while (g_variant_iter_next(&iter, "&s", &name))
            if (!std::strcmp(name, "kos_bridge")) bridge = true;
    }
    g_variant_unref(effects);
    g_variant_unref(reply);
    if (!bridge) return false;
    const std::string path = std::string(g_get_user_config_dir()) + "/kos/window-appearance.json";
    gchar* contents = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
        const bool defaults = g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
        g_clear_error(&error);
        return defaults; // NextKde's missing-file default enables its window appearance.
    }
    const auto document = length <= 1024 * 1024
        ? forma::store::json::Value::parse(std::string_view(contents, length))
        : forma::store::json::Value{};
    g_free(contents);
    return document.isObject() && document["enabled"].boolean(true);
}
}
int main(int argc, char** argv) {
    bool preview = false, dark = false, light = false, failure = false, multi = false, backdrop = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--preview")) preview = true;
        else if (!std::strcmp(argv[i], "--dark")) dark = true;
        else if (!std::strcmp(argv[i], "--light")) light = true;
        else if (!std::strcmp(argv[i], "--preview-error")) { preview = true; failure = true; }
        else if (!std::strcmp(argv[i], "--preview-identities")) { preview = true; multi = true; }
        else if (!std::strcmp(argv[i], "--preview-backdrop")) { preview = true; backdrop = true; }
        else if (!std::strcmp(argv[i], "--help")) {
            std::puts("forma-polkit-agent [--preview [--light|--dark]] [--preview-error|--preview-identities|--preview-backdrop]\n"
                      "Without --preview, registers an authentication agent for the current login session.\n"
                      "Preview never registers with polkit and never authenticates.");
            return 0;
        } else { std::fprintf(stderr, "forma-polkit-agent: unknown argument: %s\n", argv[i]); return 2; }
    }
    // Framebuffer capture must never save a live authentication conversation.
    if (!preview && std::getenv("MAYO_SCREENSHOT")) {
        std::fputs("forma-polkit-agent: screenshot capture is only available in --preview mode\n", stderr);
        return 2;
    }
    if (dark || light) setenv("MAYO_APPEARANCE", dark ? "dark" : "light", 1);
    mayo::App app("forma-polkit-agent", "io.github.skjsbsnq.Forma.PolkitAgent");
    app.setKeepAlive(true);
    if (backdrop) {
        auto& background = app.createWindow({.title = "Forma Material Test Pattern", .width = 800, .height = 700,
            .resizable = false, .dialog = true, .cornerRadius = 20});
        background.setContent(mayo::Canvas([](mayo::Painter& p, const mayo::Rect& r) {
            constexpr mayo::Color colors[] = {mayo::Color::hex(0x2463eb), mayo::Color::hex(0xf2af3c),
                mayo::Color::hex(0xf05175), mayo::Color::hex(0x37b59b)};
            for (int row = 0; row < 24; ++row) for (int col = 0; col < 28; ++col) {
                const float x = r.x + static_cast<float>(col * 30), y = r.y + static_cast<float>(row * 30);
                p.fillRect({x, y, 30, 30}, colors[(col / 5 + row / 5) % 4]);
                p.fillRect({x, y, 15, 15}, mayo::Color::white(.3f));
                p.fillRect({x + 15, y + 15, 15, 15}, mayo::Color::black(.3f));
            }
        }));
        background.onCloseRequested([&] { app.quit(); return false; });
    }
    auto& window = app.createWindow({.title = "Authentication", .width = 300, .height = 390,
        .minWidth = 300, .minHeight = 300, .resizable = false, .initiallyHidden = true,
        .dialog = true, .cornerRadius = 28, .backgroundBlur = true, .backgroundOpacity = .78f,
        .compositorFrame = nextKdeOwnsFrame()});
    auto dialog = std::make_shared<forma::auth::Dialog>();
    mayo::ScrollView scroll{mayo::Element(dialog)};
    window.setContent(scroll);
    bool wasActive = false;
    std::string lastRegistrationStatus;
    forma::polkit::Backend backend([&](const forma::polkit::State& state) {
        if (state.status != lastRegistrationStatus) {
            lastRegistrationStatus = state.status;
            if (!state.status.empty()) std::fprintf(stderr, "forma-polkit-agent: %s\n", state.status.c_str());
        }
        if (!state.active) {
            dialog->clearInput();
            if (wasActive) window.hide();
            wasActive = false;
            return;
        }
        forma::auth::DialogState view;
        view.message = state.message;
        view.iconName = state.iconName;
        view.noticeError = !state.error.empty();
        view.prompt = state.prompt;
        view.notice = state.error.empty() ? state.info : state.error;
        view.identities = state.identities;
        view.selected = state.selected;
        view.echo = state.echo;
        view.waiting = state.awaitingResponse;
        view.retry = state.canRetry;
        dialog->update(std::move(view));
        window.setSize({300, std::min(680.f, dialog->preferredHeight())});
        if (!wasActive) { window.show(); window.raise(); }
        wasActive = true;
        dialog->focusInput();
    });
    if (preview) {
        forma::auth::DialogState state;
        state.preview = true;
        state.title = "Migration Assistant";
        state.message = "Migration Assistant wants to make changes.";
        state.identities = multi ? std::vector<std::string>{"Admin", "Administrator"} : std::vector<std::string>{"Admin"};
        state.retry = failure;
        dialog->update(state);
        window.setSize({300, std::min(680.f, dialog->preferredHeight())});
        dialog->onCancel = [&] { app.quit(); };
        dialog->onResponse = [&](std::string response) {
            // No password check even in the preview. Always show a fabricated failure state.
            volatile char* data = response.data();
            for (size_t i = 0; i < response.size(); ++i) data[i] = 0;
            state.retry = true; dialog->update(state); window.setSize({300, std::min(680.f, dialog->preferredHeight())});
        };
        dialog->onRetry = [&] { state.retry = false; dialog->update(state); dialog->focusInput(); };
        dialog->onIdentity = [&](int i) { state.selected = i; dialog->update(state); dialog->focusInput(); };
        // These callbacks capture state, which must remain alive for the preview event loop.
        window.onCloseRequested([&] { app.quit(); return false; });
        window.onKey([&](const mayo::KeyEvent& e) { if (e.key == mayo::Key::Escape) { app.quit(); return true; } return false; });
        window.show();
        dialog->focusInput();
        return app.run();
    }
    dialog->onResponse = [&](std::string response) { backend.respond(std::move(response)); };
    dialog->onCancel = [&] { backend.cancel(); };
    dialog->onRetry = [&] { backend.retry(); };
    dialog->onIdentity = [&](int i) { backend.selectIdentity(i); };
    window.onCloseRequested([&] { backend.cancel(); return false; });
    window.onKey([&](const mayo::KeyEvent& e) { if (e.key == mayo::Key::Escape) { backend.cancel(); return true; } return false; });
    std::signal(SIGTERM, terminate);
    std::signal(SIGINT, terminate);
    backend.start();
    std::function<void()> pump;
    pump = [&] {
        if (quitting) { backend.shutdown(); app.quit(); return; }
        // Bound each turn so a busy D-Bus peer cannot starve the password window.
        for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, false);
        app.after(.025, pump);
    };
    app.after(0, pump);
    const int result = app.run();
    backend.shutdown();
    dialog->clearInput();
    return result;
}
