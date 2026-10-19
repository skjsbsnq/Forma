#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include "backend.hpp"
#include <polkitagent/polkitagent.h>
#include <pwd.h>
#include <unistd.h>
#include <cstring>
#include <deque>
#include <utility>

namespace forma::polkit {
static std::string bounded(const char* text, size_t limit = 2048) {
    if (!text) return {};
    size_t size = strnlen(text, limit);
    while (size && !g_utf8_validate(text, size, nullptr)) --size;
    return std::string(text, size);
}
struct Listener {
    PolkitAgentListener parent;
    Backend::Impl* owner;
};
struct ListenerClass {
    PolkitAgentListenerClass parent;
};
G_DEFINE_TYPE(Listener, forma_listener, POLKIT_AGENT_TYPE_LISTENER)
struct Request {
    GTask* task{};
    std::string message, icon, action, cookie;
    std::vector<PolkitIdentity*> identities;
    ~Request() {
        for (auto* i : identities) g_object_unref(i);
        g_clear_object(&task);
    }
};
struct Backend::Impl : std::enable_shared_from_this<Impl> {
    State state;
    std::function<void(const State&)> changed;
    Listener* listener{};
    gpointer registration{};
    guint watch{}, timer{}, cancellationPoll{};
    bool stopped = false, discovering = false, daemonAlive = false;
    unsigned generation = 0;
#ifdef FORMA_BACKEND_TEST
    bool mockSessions = false;
#endif
    struct Discovery {
        std::weak_ptr<Impl> owner;
        unsigned generation;
    };
    void abandonDiscovery() {
        ++generation;
        discovering = false;
        if (discovery) g_cancellable_cancel(discovery);
        g_clear_object(&discovery);
    }
    GCancellable* discovery{};
    std::deque<std::unique_ptr<Request>> queue;
    std::unique_ptr<Request> current;
    PolkitAgentSession* session{};
    explicit Impl(std::function<void(const State&)> cb) : changed(std::move(cb)) {}
    void notify() {
        if (changed && !stopped) changed(state);
    }
    void clearSession() {
        if (!session) return;
        auto* old = std::exchange(session, nullptr);
        g_signal_handlers_disconnect_by_data(old, this);
        polkit_agent_session_cancel(old);
        g_object_unref(old);
    }
    void finish(bool cancelled) {
        clearSession();
        if (current) {
            if (cancelled)
                g_task_return_new_error(current->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Authentication cancelled");
            else
                g_task_return_boolean(current->task, TRUE);
            current.reset();
        }
        state.active = false;
        state.waiting = false;
        state.awaitingResponse = false;
        state.canRetry = false;
        state.prompt.clear();
        state.error.clear();
        notify();
        next();
    }
    void next() {
        if (stopped || current) return;
        while (!queue.empty()) {
            current = std::move(queue.front());
            queue.pop_front();
            if (g_task_return_error_if_cancelled(current->task)) {
                current.reset();
                continue;
            }
            state.info.clear();
            state.message = current->message;
            state.iconName = current->icon;
            state.actionId = current->action;
            state.identities.clear();
            state.selected = 0;
            state.active = true;
            state.error.clear();
            for (auto* id : current->identities) {
                if (POLKIT_IS_UNIX_USER(id)) {
                    auto uid = polkit_unix_user_get_uid(POLKIT_UNIX_USER(id));
                    auto* pw = getpwuid(uid);
                    state.identities.emplace_back(pw ? pw->pw_name : std::to_string(uid));
                } else {
                    gchar* text = polkit_identity_to_string(id);
                    state.identities.emplace_back(text ? text : "Administrator");
                    g_free(text);
                }
            }
            begin();
            break;
        }
    }
    void begin() {
        if (!current || current->identities.empty()) {
            if (current) finish(true);
            return;
        }
        clearSession();
        state.info.clear();
        state.prompt.clear();
        state.error.clear();
        state.canRetry = false;
        state.waiting = true;
        state.awaitingResponse = false;
        session = polkit_agent_session_new(current->identities[state.selected], current->cookie.c_str());
        g_signal_connect(session, "request",
                         G_CALLBACK(+[](PolkitAgentSession*, const gchar* prompt, gboolean echo, gpointer p) {
                             auto& s = *static_cast<Impl*>(p);
                             s.state.prompt = bounded(prompt ? prompt : "Password:", 256);
                             s.state.echo = echo;
                             s.state.waiting = false;
                             s.state.awaitingResponse = true;
                             s.notify();
                         }),
                         this);
        g_signal_connect(session, "show-error", G_CALLBACK(+[](PolkitAgentSession*, const gchar* msg, gpointer p) {
                             auto& s = *static_cast<Impl*>(p);
                             s.state.error = bounded(msg ? msg : "Authentication failed");
                             s.notify();
                         }),
                         this);
        g_signal_connect(session, "show-info", G_CALLBACK(+[](PolkitAgentSession*, const gchar* msg, gpointer p) {
                             auto& s = *static_cast<Impl*>(p);
                             s.state.info = bounded(msg);
                             s.notify();
                         }),
                         this);
        g_signal_connect(session, "completed",
                         G_CALLBACK(+[](PolkitAgentSession* source, gboolean success, gpointer p) {
                             auto& s = *static_cast<Impl*>(p);
                             // completed is terminal; detach before destroying to prevent reentry.
                             g_signal_handlers_disconnect_by_data(source, p);
                             s.session = nullptr;
                             s.state.awaitingResponse = false;
                             // Retain the emitting object until its signal emission has unwound.
                             g_idle_add_full(
                                 G_PRIORITY_DEFAULT_IDLE, +[](gpointer) -> gboolean { return G_SOURCE_REMOVE; }, source,
                                 g_object_unref);
                             if (success)
                                 s.finish(false);
                             else {
                                 s.state.waiting = false;
                                 s.state.canRetry = true;
                                 s.state.prompt.clear();
                                 if (s.state.error.empty()) s.state.error = "Authentication failed. Try again.";
                                 s.notify();
                             }
                         }),
                         this);
        auto* started = POLKIT_AGENT_SESSION(g_object_ref(session));
        notify();
        if (session == started) {
#ifdef FORMA_BACKEND_TEST
            if (!mockSessions)
#endif
                polkit_agent_session_initiate(started);
        }
        g_object_unref(started);
    }
    void pollCancellation() {
        for (auto it = queue.begin(); it != queue.end();) {
            if (g_task_return_error_if_cancelled((*it)->task))
                it = queue.erase(it);
            else
                ++it;
        }
        if (current && g_task_get_cancellable(current->task) &&
            g_cancellable_is_cancelled(g_task_get_cancellable(current->task)))
            finish(true);
    }
    void registerAgent() {
        if (stopped || !daemonAlive || discovering || registration) return;
        discovering = true;
        discovery = g_cancellable_new();
        auto* discoveryRequest = new Discovery{shared_from_this(), generation};
        polkit_unix_session_new_for_process(
            getpid(), discovery,
            +[](GObject*, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Discovery> pending(static_cast<Discovery*>(data));
                GError* error = nullptr;
                auto* subject = polkit_unix_session_new_for_process_finish(result, &error);
                auto self = pending->owner.lock();
                if (self && self->generation == pending->generation) {
                    self->discovering = false;
                    g_clear_object(&self->discovery);
                    if (!self->stopped && self->daemonAlive && subject)
                        self->registration = polkit_agent_listener_register(POLKIT_AGENT_LISTENER(self->listener),
                                                                            POLKIT_AGENT_REGISTER_FLAGS_NONE, subject,
                                                                            nullptr, nullptr, &error);
                    self->state.registered = self->registration != nullptr;
                    self->state.status = error ? error->message
                                               : (self->registration ? "Authentication agent ready"
                                                                     : "Authentication agent unavailable");
                    self->notify();
                }
                g_clear_object(&subject);
                g_clear_error(&error);
            },
            discoveryRequest);
    }
};
static void initiate(PolkitAgentListener* listener, const gchar* action, const gchar* message, const gchar* icon,
                     PolkitDetails*, const gchar* cookie, GList* ids, GCancellable* cancel, GAsyncReadyCallback cb,
                     gpointer data) {
    auto* owner = reinterpret_cast<Listener*>(listener)->owner;
    auto request = std::make_unique<Request>();
    request->task = g_task_new(listener, cancel, cb, data);
    if (!owner || owner->stopped || !ids) {
        g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Authentication agent unavailable");
        return;
    }
    if (owner->queue.size() >= 8 || (cookie && strnlen(cookie, 4097) > 4096)) {
        g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_BUSY, "Authentication queue is full");
        return;
    }
    request->action = bounded(action, 512);
    request->message = bounded(message ? message : "Authentication is required.");
    request->icon = bounded(icon, 256);
    request->cookie = cookie ? cookie : "";
    for (auto* i = ids; i && request->identities.size() < 32; i = i->next)
        request->identities.push_back(POLKIT_IDENTITY(g_object_ref(i->data)));
    owner->queue.push_back(std::move(request));
    owner->next();
}
static void forma_listener_class_init(ListenerClass* cls) {
    auto* listener = POLKIT_AGENT_LISTENER_CLASS(cls);
    listener->initiate_authentication = initiate;
    listener->initiate_authentication_finish =
        +[](PolkitAgentListener*, GAsyncResult* result, GError** error) -> gboolean {
        return g_task_propagate_boolean(G_TASK(result), error);
    };
}
static void forma_listener_init(Listener* listener) { listener->owner = nullptr; }
Backend::Backend(std::function<void(const State&)> cb) : impl_(std::make_shared<Impl>(std::move(cb))) {}
Backend::~Backend() { shutdown(); }
const State& Backend::state() const { return impl_->state; }
void Backend::start() {
    auto& owner = *impl_;
    if (owner.listener || owner.stopped) return;
    owner.listener = reinterpret_cast<Listener*>(g_object_new(forma_listener_get_type(), nullptr));
    owner.listener->owner = &owner;
    owner.watch = g_bus_watch_name(
        G_BUS_TYPE_SYSTEM, "org.freedesktop.PolicyKit1", G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer p) {
            auto& s = *static_cast<Impl*>(p);
            s.daemonAlive = true;
            s.registerAgent();
        },
        +[](GDBusConnection*, const gchar*, gpointer p) {
            auto& s = *static_cast<Impl*>(p);
            s.daemonAlive = false;
            s.abandonDiscovery();
            if (s.registration) {
                polkit_agent_listener_unregister(s.registration);
                s.registration = nullptr;
            }
            s.state.registered = false;
            for (auto& request : s.queue) {
                g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                        "Authorization service disconnected");
            }
            s.queue.clear();
            if (s.current) s.finish(true);
            s.state.status = "Waiting for the authorization service";
            s.notify();
        },
        &owner, nullptr);
    owner.timer = g_timeout_add_seconds(
        5,
        +[](gpointer p) -> gboolean {
            static_cast<Impl*>(p)->registerAgent();
            return G_SOURCE_CONTINUE;
        },
        &owner);
    owner.cancellationPoll = g_timeout_add(
        50,
        +[](gpointer p) -> gboolean {
            static_cast<Impl*>(p)->pollCancellation();
            return G_SOURCE_CONTINUE;
        },
        &owner);
}
void Backend::selectIdentity(int index) {
    auto& s = *impl_;
    if (s.current && index >= 0 && index < static_cast<int>(s.current->identities.size()) &&
        index != s.state.selected) {
        s.state.selected = index;
        s.begin();
    }
}
void Backend::respond(std::string response) {
    auto& s = *impl_;
    if (s.session && s.state.awaitingResponse) {
        s.state.waiting = true;
        s.state.awaitingResponse = false;
        s.state.prompt.clear();
        auto* responding = POLKIT_AGENT_SESSION(g_object_ref(s.session));
        s.notify();
        if (s.session == responding) {
#ifdef FORMA_BACKEND_TEST
            if (!s.mockSessions)
#endif
                polkit_agent_session_response(responding, response.c_str());
        }
        g_object_unref(responding);
    }
    volatile char* bytes = response.empty() ? nullptr : response.data();
    for (size_t i = 0; i < response.size(); ++i) bytes[i] = 0;
}
void Backend::retry() {
    if (impl_->current && impl_->state.canRetry) impl_->begin();
}
void Backend::cancel() {
    if (impl_->current) impl_->finish(true);
}
void Backend::shutdown() {
    auto& s = *impl_;
    if (s.stopped) return;
    s.stopped = true;
    s.changed = {};
    if (s.watch) {
        g_bus_unwatch_name(s.watch);
        s.watch = 0;
    }
    if (s.timer) {
        g_source_remove(s.timer);
        s.timer = 0;
    }
    if (s.cancellationPoll) {
        g_source_remove(s.cancellationPoll);
        s.cancellationPoll = 0;
    }
    s.abandonDiscovery();
    s.clearSession();
    if (s.current) s.finish(true);
    for (auto& request : s.queue) {
        g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Authentication agent stopped");
    }
    s.queue.clear();
    if (s.registration) {
        polkit_agent_listener_unregister(s.registration);
        s.registration = nullptr;
    }
    if (s.listener) {
        s.listener->owner = nullptr;
        g_clear_object(&s.listener);
    }
    g_clear_object(&s.discovery);
}
}  // namespace forma::polkit
