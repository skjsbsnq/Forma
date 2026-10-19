// Standalone test translation unit: compile this file, not backend.cpp alongside it.
// Real GObject listener/session signals, with only PAM helper execution suppressed.
#define FORMA_BACKEND_TEST
#include <cassert>
#include <iostream>
#include "backend.cpp"
namespace forma::polkit {
struct BackendTestAccess {
    static auto impl(Backend& backend) { return backend.impl_; }
};
}  // namespace forma::polkit
using namespace forma::polkit;
static void drain() {
    while (g_main_context_iteration(nullptr, false)) {
    }
}
struct Result {
    int done = 0;
    bool success = false;
    int error = 0;
};
static void completed(GObject* object, GAsyncResult* result, gpointer data) {
    auto& out = *static_cast<Result*>(data);
    GError* error = nullptr;
    out.success = polkit_agent_listener_initiate_authentication_finish(POLKIT_AGENT_LISTENER(object), result, &error);
    out.done++;
    out.error = error ? error->code : 0;
    g_clear_error(&error);
}
static void submit(Backend::Impl& impl, Result& result, GCancellable* cancel = nullptr,
                   const char* message = "Authorize") {
    auto* identity = polkit_unix_user_new(getuid());
    GList* ids = g_list_append(nullptr, identity);
    polkit_agent_listener_initiate_authentication(POLKIT_AGENT_LISTENER(impl.listener), "test.action", message, "",
                                                  nullptr, "test-cookie", ids, cancel, completed, &result);
    g_list_free(ids);
    g_object_unref(identity);
}
int main() {
    Backend backend([](const State&) {});
    auto impl = BackendTestAccess::impl(backend);
    impl->listener = reinterpret_cast<Listener*>(g_object_new(forma_listener_get_type(), nullptr));
    impl->listener->owner = impl.get();
    impl->mockSessions = true;
    Result first, second;
    submit(*impl, first);
    auto* cancel = g_cancellable_new();
    submit(*impl, second, cancel);
    assert(impl->current && impl->queue.size() == 1);
    g_cancellable_cancel(cancel);
    impl->pollCancellation();
    drain();
    assert(second.done == 1 && second.error == G_IO_ERROR_CANCELLED);
    assert(first.done == 0 && impl->queue.empty());
    g_object_unref(cancel);
    g_signal_emit_by_name(impl->session, "request", "Password:", FALSE);
    assert(!backend.state().waiting && !backend.state().echo);
    backend.respond("never-a-real-secret");
    assert(backend.state().waiting);
    // PAM request("") is a real challenge, independent of whether it supplied a label.
    g_signal_emit_by_name(impl->session, "request", "", FALSE);
    assert(backend.state().awaitingResponse && !backend.state().waiting && backend.state().prompt.empty());
    backend.respond("empty-prompt-response");
    assert(!backend.state().awaitingResponse && backend.state().waiting);
    g_signal_emit_by_name(impl->session, "request", "Verification code:", TRUE);
    assert(!backend.state().waiting && backend.state().echo);
    g_signal_emit_by_name(impl->session, "show-info", "Use your second factor");
    assert(backend.state().info == "Use your second factor");
    g_signal_emit_by_name(impl->session, "completed", FALSE);
    assert(backend.state().canRetry && !impl->session);
    backend.retry();
    assert(impl->session && !backend.state().canRetry && backend.state().info.empty());
    backend.cancel();
    backend.cancel();
    drain();
    assert(first.done == 1 && first.error == G_IO_ERROR_CANCELLED);
    Result success, following;
    submit(*impl, success);
    submit(*impl, following);
    g_signal_emit_by_name(impl->session, "completed", TRUE);
    drain();
    assert(success.done == 1 && success.success && impl->current);
    assert(following.done == 0);
    backend.cancel();
    drain();
    assert(following.done == 1);
    Result oversized;
    std::string longMessage(10000, 'x');
    submit(*impl, oversized, nullptr, longMessage.c_str());
    assert(backend.state().message.size() == 2048);
    Result queued;
    submit(*impl, queued);
    backend.shutdown();
    backend.shutdown();
    drain();
    assert(oversized.done == 1 && queued.done == 1);
    // A pending discovery must be invalidated both when the daemon vanishes
    // and on shutdown, without clearing a newer request's cancellable.
    Backend pending([](const State&) {});
    auto pendingImpl = BackendTestAccess::impl(pending);
    pendingImpl->daemonAlive = true;
    pendingImpl->registerAgent();
    auto generation = pendingImpl->generation;
    pendingImpl->daemonAlive = false;
    pendingImpl->abandonDiscovery();
    assert(pendingImpl->generation > generation && !pendingImpl->discovering);
    pending.shutdown();
    drain();
    assert(!pending.state().registered);
    std::cout << "Backend lifecycle tests passed\n";
}
