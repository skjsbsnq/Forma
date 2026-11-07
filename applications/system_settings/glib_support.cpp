#include "glib_support.hpp"

#include <gio/gdesktopappinfo.h>

#include <mayonnaise/app.hpp>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace gx {

namespace {

std::atomic<mayo::App*> g_app{nullptr};
std::atomic<bool> g_readOnly{false};
GMainLoop* g_loop = nullptr;
std::thread g_thread;
std::function<void(const Key&, const WriteResult&)> g_writeError;

// Background worker.
std::mutex g_workMutex;
std::condition_variable g_workCv;
std::deque<std::function<void()>> g_work;
std::thread g_worker;
bool g_workStop = false;

// Streams are started on the UI thread and killed from it in stop(); the list is only touched there.
std::vector<GSubprocess*> g_streams;

struct ErrorFree {
    void operator()(GError* e) const {
        if (e) g_error_free(e);
    }
};
using Error = std::unique_ptr<GError, ErrorFree>;

std::string message(GError* e) {
    if (!e) return {};
    if (g_dbus_error_is_remote_error(e)) g_dbus_error_strip_remote_error(e);
    return e->message ? e->message : "Unknown error";
}

// GSettings ---------------------------------------------------------------------------------------

struct WatchKey {
    GSettings* settings;
    std::string key;
    bool operator<(const WatchKey& o) const { return settings != o.settings ? settings < o.settings : key < o.key; }
};

std::map<std::string, GSettings*>& settingsCache() {
    static std::map<std::string, GSettings*> cache;
    return cache;
}
std::multimap<WatchKey, std::function<void()>>& watchers() {
    static std::multimap<WatchKey, std::function<void()>> w;
    return w;
}

void onSettingsChanged(GSettings* settings, const char* key, gpointer /*data*/) {
    toUi([settings, k = std::string(key ? key : "")] {
        auto range = k.empty() ? std::pair{watchers().lower_bound({settings, {}}), watchers().end()}
                               : watchers().equal_range({settings, k});
        // Copy first: a watcher may add another watcher (rebuilding a list).
        std::vector<std::function<void()>> fns;
        for (auto it = range.first; it != range.second && it->first.settings == settings; ++it) fns.push_back(it->second);
        for (auto& fn : fns) fn();
    });
}

GSettingsSchema* lookupSchema(const std::string& schema) {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    return source ? g_settings_schema_source_lookup(source, schema.c_str(), TRUE) : nullptr;
}

GSettings* settingsFor(const std::string& schema, const std::string& path, GSettingsBackend* backend) {
    auto& cache = settingsCache();
    const std::string id = schema + "|" + path + "|" + std::to_string(reinterpret_cast<std::uintptr_t>(backend));
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    GSettings* s = nullptr;
    if (GSettingsSchema* sc = lookupSchema(schema)) {
        // A relocatable schema needs a path, a fixed one must not get one.
        const char* fixed = g_settings_schema_get_path(sc);
        if (fixed || !path.empty()) {
            s = g_settings_new_full(sc, backend, fixed ? nullptr : path.c_str());
            g_signal_connect(s, "changed", G_CALLBACK(onSettingsChanged), nullptr);
            g_signal_connect(s, "writable-changed", G_CALLBACK(onSettingsChanged), nullptr);
        }
        g_settings_schema_unref(sc);
    }
    cache.emplace(id, s);
    return s;
}

struct SchemaKey {
    GSettingsSchemaKey* key = nullptr;
    SchemaKey(GSettings* s, const std::string& name) {
        GSettingsSchema* schema = nullptr;
        g_object_get(s, "settings-schema", &schema, nullptr);
        if (schema) {
            if (g_settings_schema_has_key(schema, name.c_str())) key = g_settings_schema_get_key(schema, name.c_str());
            g_settings_schema_unref(schema);
        }
    }
    ~SchemaKey() {
        if (key) g_settings_schema_key_unref(key);
    }
    SchemaKey(const SchemaKey&) = delete;
    SchemaKey& operator=(const SchemaKey&) = delete;
};

// D-Bus ----------------------------------------------------------------------------------------------

struct CallData {
    Reply done;
};

void onCallDone(GObject* source, GAsyncResult* res, gpointer data) {
    std::unique_ptr<CallData> cd(static_cast<CallData*>(data));
    GError* raw = nullptr;
    GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &raw);
    Error err(raw);
    if (!cd->done) {
        if (reply) g_variant_unref(reply);
        return;
    }
    toUi([done = std::move(cd->done), v = std::make_shared<Variant>(reply), e = message(err.get())]() mutable {
        done(std::move(*v), e);
    });
}

void callImpl(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
              GVariant* params, Reply done, int timeoutMs, GDBusCallFlags flags) {
    if (!bus) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        if (done) toUi([done] { done(nullptr, "The message bus is not available."); });
        return;
    }
    g_dbus_connection_call(bus, name, path, iface, method, params, nullptr, flags, timeoutMs, nullptr, onCallDone,
                           new CallData{std::move(done)});
}

void onSignal(GDBusConnection* /*bus*/, const char* /*sender*/, const char* path, const char* /*iface*/,
              const char* member, GVariant* params, gpointer data) {
    auto* fn = static_cast<SignalFn*>(data);
    toUi([fn, p = std::string(path ? path : ""), m = std::string(member ? member : ""),
          v = std::make_shared<Variant>(params ? g_variant_ref(params) : nullptr)] { (*fn)(p, m, v->get()); });
}

// Subprocesses ------------------------------------------------------------------------------------

GSubprocessLauncher* launcher(GSubprocessFlags flags) {
    GSubprocessLauncher* l = g_subprocess_launcher_new(flags);
    // Parsing relies on untranslated output; pactl's JSON writer also chokes on some locales.
    g_subprocess_launcher_setenv(l, "LC_ALL", "C.UTF-8", TRUE);
    return l;
}

std::vector<const char*> cArgv(const std::vector<std::string>& argv) {
    std::vector<const char*> out;
    for (const auto& a : argv) out.push_back(a.c_str());
    out.push_back(nullptr);
    return out;
}

GSubprocess* spawn(const std::vector<std::string>& argv, GSubprocessFlags flags, std::string& error) {
    Object<GSubprocessLauncher> l(launcher(flags));
    auto args = cArgv(argv);
    GError* raw = nullptr;
    GSubprocess* p = g_subprocess_launcher_spawnv(l.get(), args.data(), &raw);
    Error err(raw);
    if (!p) error = message(err.get());
    return p;
}

std::string bytesToString(GBytes* b) {
    if (!b) return {};
    gsize n = 0;
    const auto* data = static_cast<const char*>(g_bytes_get_data(b, &n));
    std::string s(data ? data : "", n);
    g_bytes_unref(b);
    return s;
}

int exitStatus(GSubprocess* p) { return g_subprocess_get_if_exited(p) ? g_subprocess_get_exit_status(p) : 128; }

struct RunData {
    std::function<void(ProcessResult)> done;
};

void onRunDone(GObject* source, GAsyncResult* res, gpointer data) {
    std::unique_ptr<RunData> rd(static_cast<RunData*>(data));
    auto* p = G_SUBPROCESS(source);
    GBytes* out = nullptr;
    GBytes* errOut = nullptr;
    GError* raw = nullptr;
    g_subprocess_communicate_finish(p, res, &out, &errOut, &raw);
    Error err(raw);
    ProcessResult r;
    r.out = bytesToString(out);
    r.err = bytesToString(errOut);
    r.status = err ? -1 : exitStatus(p);
    if (err && r.err.empty()) r.err = message(err.get());
    g_object_unref(p);
    if (rd->done) toUi([done = std::move(rd->done), r = std::move(r)] { done(r); });
}

struct StreamData {
    GDataInputStream* in;
    std::function<void(const std::string&)> onLine;
};

void readNextLine(StreamData* sd);

void onLine(GObject* source, GAsyncResult* res, gpointer data) {
    auto* sd = static_cast<StreamData*>(data);
    gsize len = 0;
    char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), res, &len, nullptr);
    if (!line) {  // end of stream or error: the process is gone
        g_object_unref(sd->in);
        delete sd;
        return;
    }
    toUi([fn = sd->onLine, s = std::string(line, len)] { fn(s); });
    g_free(line);
    readNextLine(sd);
}

void readNextLine(StreamData* sd) {
    g_data_input_stream_read_line_async(sd->in, G_PRIORITY_DEFAULT, nullptr, onLine, sd);
}

}  // namespace

// Lifetime ---------------------------------------------------------------------------------------------

void start(mayo::App& app) {
    g_app = &app;
    g_loop = g_main_loop_new(nullptr, FALSE);
    g_thread = std::thread([] { g_main_loop_run(g_loop); });
}

void stop() {
    {
        std::lock_guard lock(g_workMutex);
        g_workStop = true;
        g_work.clear();
    }
    g_workCv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    for (GSubprocess* p : g_streams) {
        g_subprocess_force_exit(p);
        g_object_unref(p);
    }
    g_streams.clear();
    // Flush pending dconf writes before the process exits.
    g_settings_sync();
    g_app = nullptr;
    if (g_loop) {
        g_main_loop_quit(g_loop);
        if (g_thread.joinable()) g_thread.join();
        g_main_loop_unref(g_loop);
        g_loop = nullptr;
    }
    watchers().clear();
    g_writeError = {};
}

void toUi(std::function<void()> fn) {
    if (mayo::App* app = g_app.load()) app->post(std::move(fn));
}

void setReadOnly(bool readOnly) { g_readOnly = readOnly; }
bool readOnly() { return g_readOnly; }
void setWriteErrorHandler(std::function<void(const Key&, const WriteResult&)> handler) { g_writeError = std::move(handler); }

// GSettings ------------------------------------------------------------------------------------------

bool hasSchema(const std::string& schema) {
    GSettingsSchema* s = lookupSchema(schema);
    if (s) g_settings_schema_unref(s);
    return s != nullptr;
}

Key::Key(const std::string& schema, std::string key, const std::string& path, GSettingsBackend* backend)
    : m_key(std::move(key)) {
    GSettings* s = settingsFor(schema, path, backend);
    if (s && SchemaKey(s, m_key).key) m_settings = s;
}

std::string Key::type() const {
    if (m_backend) return m_backend->type;
    if (!m_settings) return {};
    SchemaKey k(m_settings, m_key);
    return k.key ? g_variant_type_peek_string(g_settings_schema_key_get_value_type(k.key)) : std::string();
}

std::optional<std::string> Key::backendText() const {
    return m_backend && m_backend->read ? m_backend->read() : std::nullopt;
}

Variant Key::value() const {
    if (m_backend) {
        const std::string text = backendText().value_or(m_backend->fallback);
        const std::string& t = m_backend->type;
        if (t == "b") {
            const std::string l = [&] {
                std::string s = text;
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            }();
            return hold(g_variant_new_boolean(l == "true" || l == "1" || l == "yes" || l == "on"));
        }
        if (t == "i") return hold(g_variant_new_int32(static_cast<gint32>(std::strtoll(text.c_str(), nullptr, 10))));
        if (t == "d") return hold(g_variant_new_double(g_ascii_strtod(text.c_str(), nullptr)));
        if (t == "as") {
            const auto items = splitList(text);
            std::vector<const char*> c;
            for (const auto& i : items) c.push_back(i.c_str());
            return hold(g_variant_new_strv(c.data(), static_cast<gssize>(c.size())));
        }
        return hold(g_variant_new_string(text.c_str()));
    }
    return m_settings ? Variant(g_settings_get_value(m_settings, m_key.c_str())) : nullptr;
}

bool Key::getBool(bool fallback) const {
    auto v = value();
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_BOOLEAN) ? g_variant_get_boolean(v.get()) : fallback;
}

std::int64_t Key::getInt(std::int64_t fallback) const {
    auto v = value();
    if (!v) return fallback;
    switch (g_variant_classify(v.get())) {
        case G_VARIANT_CLASS_INT32: return g_variant_get_int32(v.get());
        case G_VARIANT_CLASS_UINT32: return g_variant_get_uint32(v.get());
        case G_VARIANT_CLASS_INT64: return g_variant_get_int64(v.get());
        case G_VARIANT_CLASS_UINT64: return static_cast<std::int64_t>(g_variant_get_uint64(v.get()));
        case G_VARIANT_CLASS_INT16: return g_variant_get_int16(v.get());
        case G_VARIANT_CLASS_UINT16: return g_variant_get_uint16(v.get());
        case G_VARIANT_CLASS_BYTE: return g_variant_get_byte(v.get());
        case G_VARIANT_CLASS_DOUBLE: return std::llround(g_variant_get_double(v.get()));
        default: return fallback;
    }
}

double Key::getDouble(double fallback) const {
    auto v = value();
    if (!v) return fallback;
    if (g_variant_is_of_type(v.get(), G_VARIANT_TYPE_DOUBLE)) return g_variant_get_double(v.get());
    return static_cast<double>(getInt(static_cast<std::int64_t>(fallback)));
}

std::string Key::getString(const std::string& fallback) const {
    auto v = value();
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_STRING) ? g_variant_get_string(v.get(), nullptr)
                                                                      : fallback;
}

std::vector<std::string> Key::getStrv() const {
    auto v = value();
    return v ? strv(v.get()) : std::vector<std::string>{};
}

std::string Key::getText() const {
    auto v = value();
    if (!v) return {};
    switch (g_variant_classify(v.get())) {
        case G_VARIANT_CLASS_STRING: return g_variant_get_string(v.get(), nullptr);
        case G_VARIANT_CLASS_BOOLEAN: return g_variant_get_boolean(v.get()) ? "true" : "false";
        case G_VARIANT_CLASS_ARRAY: return joinList(strv(v.get()));
        case G_VARIANT_CLASS_DOUBLE: {
            char buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd(buf, sizeof buf, "%g", g_variant_get_double(v.get()));
            return buf;
        }
        default: return std::to_string(getInt());
    }
}

struct Key::WriteState {
    bool pending = false;
    bool writing = false;
    std::vector<std::function<void(const Key&, const WriteResult&)>> observers;
};

std::shared_ptr<Key::WriteState> Key::makeWriteState() { return std::make_shared<WriteState>(); }
bool Key::pending() const { return m_writeState->pending; }

bool Key::writable() const {
    if (gx::readOnly() || !available()) return false;
    if (m_backend) return m_backend->write && (!m_backend->writable || m_backend->writable());
    return g_settings_is_writable(m_settings, m_key.c_str());
}

void Key::watchWrite(std::function<void(const Key&, const WriteResult&)> fn) const {
    m_writeState->observers.push_back(std::move(fn));
}

WriteResult Key::submit(std::function<WriteResult(WriteDone)> write, WriteDone done) const {
    if (m_writeState->writing) {
        auto result = WriteResult::failure("A settings change is still pending");
        if (done) done(result);
        return result;
    }
    auto finished = std::make_shared<std::optional<WriteResult>>();
    auto finish = [key = *this, done = std::move(done), finished](WriteResult result) {
        if (*finished) return;
        *finished = result;
        key.m_writeState->pending = false;
        key.m_writeState->writing = false;
        const auto observers = key.m_writeState->observers;
        for (const auto& fn : observers) fn(key, result);
        if (done) done(result);
        else if (observers.empty() && !result.ok() && !result.pending() && g_writeError) g_writeError(key, result);
    };
    if (!available()) {
        auto r = WriteResult::failure("This setting is not available on this system");
        finish(r);
        return r;
    }
    if (!writable()) {
        auto r =
            WriteResult::readOnly(gx::readOnly() ? "Settings are read-only" : "This setting is locked or not writable");
        finish(r);
        return r;
    }
    m_writeState->writing = true;
    auto result = write(finish);
    if (!result.pending())
        finish(result);
    else if (!*finished) {
        // Synchronous writes never disable the field (and steal its editing focus) for one keystroke.
        m_writeState->pending = true;
        const auto observers = m_writeState->observers;
        for (const auto& fn : observers) fn(*this, result);
    }
    return *finished ? **finished : result;
}

WriteResult Key::reject(std::string reason, WriteDone done) const {
    return submit([reason = std::move(reason)](WriteDone) { return WriteResult::failure(reason); }, std::move(done));
}

WriteResult Key::setValue(GVariant* v, WriteDone done) const {
    auto held = std::shared_ptr<GVariant>(hold(v).release(), VariantUnref{});
    return submit(
        [key = *this, held](WriteDone complete) {
            if (!held) return WriteResult::failure("No value was supplied");
            const std::string t = key.type();
            if (!g_variant_is_of_type(held.get(), G_VARIANT_TYPE(t.c_str())))
                return WriteResult::failure("Wrong value type for " + key.name());
            if (key.m_backend) {
                std::string text;
                switch (g_variant_classify(held.get())) {
                    case G_VARIANT_CLASS_STRING: text = g_variant_get_string(held.get(), nullptr); break;
                    case G_VARIANT_CLASS_BOOLEAN: text = g_variant_get_boolean(held.get()) ? "true" : "false"; break;
                    case G_VARIANT_CLASS_DOUBLE: {
                        char buf[G_ASCII_DTOSTR_BUF_SIZE];
                        g_ascii_formatd(buf, sizeof buf, "%.6g", g_variant_get_double(held.get()));
                        text = buf;
                        break;
                    }
                    case G_VARIANT_CLASS_ARRAY: text = joinList(strv(held.get())); break;
                    case G_VARIANT_CLASS_INT32: text = std::to_string(g_variant_get_int32(held.get())); break;
                    case G_VARIANT_CLASS_UINT32: text = std::to_string(g_variant_get_uint32(held.get())); break;
                    case G_VARIANT_CLASS_INT64: text = std::to_string(g_variant_get_int64(held.get())); break;
                    case G_VARIANT_CLASS_UINT64: text = std::to_string(g_variant_get_uint64(held.get())); break;
                    default: return WriteResult::failure("Unsupported value type for " + key.name());
                }
                return key.m_backend->write(text, std::move(complete));
            }
            SchemaKey schema(key.m_settings, key.m_key);
            if (!schema.key || !g_settings_schema_key_range_check(schema.key, held.get()))
                return WriteResult::failure("Value is outside the allowed range for " + key.name());
            return g_settings_set_value(key.m_settings, key.m_key.c_str(), held.get())
                       ? WriteResult{}
                       : WriteResult::failure("GSettings rejected the change to " + key.name());
        },
        std::move(done));
}

WriteResult Key::set(bool v, WriteDone done) const { return setValue(g_variant_new_boolean(v), std::move(done)); }

WriteResult Key::setInt(std::int64_t v, WriteDone done) const {
    const std::string t = type();
    GVariant* value = nullptr;
    if (t == "i" && v >= G_MININT32 && v <= G_MAXINT32)
        value = g_variant_new_int32(static_cast<gint32>(v));
    else if (t == "u" && v >= 0 && v <= G_MAXUINT32)
        value = g_variant_new_uint32(static_cast<guint32>(v));
    else if (t == "x")
        value = g_variant_new_int64(v);
    else if (t == "t" && v >= 0)
        value = g_variant_new_uint64(static_cast<guint64>(v));
    else if (t == "n" && v >= G_MININT16 && v <= G_MAXINT16)
        value = g_variant_new_int16(static_cast<gint16>(v));
    else if (t == "q" && v >= 0 && v <= G_MAXUINT16)
        value = g_variant_new_uint16(static_cast<guint16>(v));
    else if (t == "y" && v >= 0 && v <= 255)
        value = g_variant_new_byte(static_cast<guchar>(v));
    else if (t == "d")
        value = g_variant_new_double(static_cast<double>(v));
    return value ? setValue(value, std::move(done)) : reject("Invalid integer value for " + name(), std::move(done));
}

WriteResult Key::setDouble(double v, WriteDone done) const {
    if (!std::isfinite(v)) return reject("Invalid numeric value for " + name(), std::move(done));
    if (type() == "d") return setValue(g_variant_new_double(v), std::move(done));
    if (v < static_cast<double>(G_MININT64) || v >= static_cast<double>(G_MAXINT64))
        return reject("Invalid numeric value for " + name(), std::move(done));
    return setInt(std::llround(v), std::move(done));
}

WriteResult Key::setString(const std::string& v, WriteDone done) const {
    if (v.find('\0') != std::string::npos || !g_utf8_validate(v.data(), static_cast<gssize>(v.size()), nullptr))
        return reject("Invalid UTF-8 text for " + name(), std::move(done));
    return setValue(g_variant_new_string(v.c_str()), std::move(done));
}

WriteResult Key::setStrv(const std::vector<std::string>& v, WriteDone done) const {
    std::vector<const char*> c;
    for (const auto& s : v) {
        if (s.find('\0') != std::string::npos || !g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr))
            return reject("Invalid UTF-8 list item for " + name(), std::move(done));
        c.push_back(s.c_str());
    }
    return setValue(g_variant_new_strv(c.data(), static_cast<gssize>(c.size())), std::move(done));
}

WriteResult Key::setText(const std::string& text, WriteDone done) const {
    const std::string t = type();
    if (t == "s") return setString(text, std::move(done));
    if (t == "as") return setStrv(splitList(text), std::move(done));
    if (t == "b") {
        if (text != "true" && text != "false") return reject("Expected true or false for " + name(), std::move(done));
        return set(text == "true", std::move(done));
    }
    char* end = nullptr;
    if (t == "d") {
        const double v = g_ascii_strtod(text.c_str(), &end);
        if (!text.empty() && end == text.c_str() + text.size()) return setDouble(v, std::move(done));
    } else {
        errno = 0;
        const auto v = std::strtoll(text.c_str(), &end, 10);
        if (!text.empty() && end == text.c_str() + text.size() && errno != ERANGE) return setInt(v, std::move(done));
    }
    return reject("Invalid numeric value for " + name() + ": " + text, std::move(done));
}

WriteResult Key::reset(WriteDone done) const {
    return submit(
        [key = *this](WriteDone complete) {
            if (key.m_backend) return key.m_backend->write(std::nullopt, std::move(complete));
            g_settings_reset(key.m_settings, key.m_key.c_str());
            Variant user(g_settings_get_user_value(key.m_settings, key.m_key.c_str()));
            return !user ? WriteResult{} : WriteResult::failure("GSettings could not reset " + key.name());
        },
        std::move(done));
}

bool Key::isDefault() const {
    if (m_backend) return !backendText();
    if (!m_settings) return true;
    Variant user(g_settings_get_user_value(m_settings, m_key.c_str()));
    return !user;
}

std::optional<std::pair<double, double>> Key::range() const {
    if (m_backend) return m_backend->range;
    if (!m_settings) return std::nullopt;
    SchemaKey k(m_settings, m_key);
    if (!k.key) return std::nullopt;
    Variant r(g_settings_schema_key_get_range(k.key));
    const char* kind = nullptr;
    GVariant* detail = nullptr;
    g_variant_get(r.get(), "(&sv)", &kind, &detail);
    Variant d(detail);
    if (std::string(kind) != "range") return std::nullopt;
    Variant lo(g_variant_get_child_value(d.get(), 0));
    Variant hi(g_variant_get_child_value(d.get(), 1));
    auto num = [](GVariant* v) -> double {
        switch (g_variant_classify(v)) {
            case G_VARIANT_CLASS_DOUBLE: return g_variant_get_double(v);
            case G_VARIANT_CLASS_INT32: return g_variant_get_int32(v);
            case G_VARIANT_CLASS_UINT32: return g_variant_get_uint32(v);
            case G_VARIANT_CLASS_INT64: return static_cast<double>(g_variant_get_int64(v));
            case G_VARIANT_CLASS_UINT64: return static_cast<double>(g_variant_get_uint64(v));
            default: return 0;
        }
    };
    return std::pair{num(lo.get()), num(hi.get())};
}

std::vector<std::string> Key::choices() const {
    if (!m_settings) return {};
    SchemaKey k(m_settings, m_key);
    if (!k.key) return {};
    Variant r(g_settings_schema_key_get_range(k.key));
    const char* kind = nullptr;
    GVariant* detail = nullptr;
    g_variant_get(r.get(), "(&sv)", &kind, &detail);
    Variant d(detail);
    const std::string type = kind;
    return type == "enum" || type == "flags" ? strv(d.get()) : std::vector<std::string>{};
}

void Key::watch(std::function<void()> fn) const {
    if (m_backend) {
        if (m_backend->watch) m_backend->watch(std::move(fn));
        return;
    }
    if (m_settings) watchers().emplace(WatchKey{m_settings, m_key}, std::move(fn));
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == ',') {
            cur.push_back(',');
            ++i;
        } else if (text[i] == ',') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(text[i]);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out.push_back(',');
        for (char c : items[i]) {
            if (c == ',') out.push_back('\\');
            out.push_back(c);
        }
    }
    return out;
}

// D-Bus ----------------------------------------------------------------------------------------------

GDBusConnection* systemBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    return bus;
}

GDBusConnection* sessionBus() {
    static GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    return bus;
}

bool hasName(GDBusConnection* bus, const char* name) {
    auto owned = callSync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "NameHasOwner", g_variant_new("(s)", name), "(b)");
    if (owned) {
        gboolean b = FALSE;
        g_variant_get(owned.get(), "(b)", &b);
        if (b) return true;
    }
    auto activatable = callSync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                "ListActivatableNames", nullptr, "(as)");
    if (!activatable) return false;
    Variant list(g_variant_get_child_value(activatable.get(), 0));
    for (const auto& n : strv(list.get())) {
        if (n == name) return true;
    }
    return false;
}

Variant callSync(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
                 GVariant* params, const char* replyType, std::string* error, int timeoutMs) {
    if (!bus) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        if (error) *error = "The message bus is not available.";
        return nullptr;
    }
    GError* raw = nullptr;
    GVariant* r = g_dbus_connection_call_sync(bus, name, path, iface, method, params,
                                              replyType ? G_VARIANT_TYPE(replyType) : nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, timeoutMs, nullptr, &raw);
    Error err(raw);
    if (error) *error = message(err.get());
    return Variant(r);
}

Variant property(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* prop) {
    auto r = callSync(bus, name, path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, prop),
                      "(v)", nullptr, 1500);
    if (!r) return nullptr;
    GVariant* inner = nullptr;
    g_variant_get(r.get(), "(v)", &inner);
    return Variant(inner);
}

Variant properties(GDBusConnection* bus, const char* name, const char* path, const char* iface) {
    auto r = callSync(bus, name, path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", iface),
                      "(a{sv})", nullptr, 1500);
    return r ? Variant(g_variant_get_child_value(r.get(), 0)) : nullptr;
}

void call(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
          GVariant* params, Reply done, int timeoutMs) {
    callImpl(bus, name, path, iface, method, params, std::move(done), timeoutMs, G_DBUS_CALL_FLAGS_NONE);
}

void perform(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
             GVariant* params, Reply done, int timeoutMs) {
    if (g_readOnly) {
        if (params) g_variant_unref(g_variant_ref_sink(params));
        if (done) toUi([done] { done(nullptr, "Settings are read-only"); });
        return;
    }
    callImpl(bus, name, path, iface, method, params, std::move(done), timeoutMs,
             G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION);
}

void setProperty(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* prop,
                 GVariant* value, Reply done) {
    perform(bus, name, path, "org.freedesktop.DBus.Properties", "Set", g_variant_new("(ssv)", iface, prop, value),
            std::move(done));
}

void subscribe(GDBusConnection* bus, const char* sender, const char* iface, const char* member, const char* path,
               SignalFn fn) {
    if (!bus) return;
    g_dbus_connection_signal_subscribe(
        bus, sender, iface, member, path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onSignal, new SignalFn(std::move(fn)),
        [](gpointer p) { delete static_cast<SignalFn*>(p); });
}

// Variant helpers --------------------------------------------------------------------------------------

std::string str(GVariant* v) {
    if (!v) return {};
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
        Variant inner(g_variant_get_variant(v));
        return str(inner.get());
    }
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
        return g_variant_get_string(v, nullptr);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) return g_variant_get_bytestring(v);
    return {};
}

Variant lookup(GVariant* dict, const char* key) {
    if (!dict || !g_variant_is_of_type(dict, G_VARIANT_TYPE("a{sv}"))) return nullptr;
    return Variant(g_variant_lookup_value(dict, key, nullptr));
}

std::string lookupString(GVariant* dict, const char* key, const std::string& fallback) {
    auto v = lookup(dict, key);
    return v ? str(v.get()) : fallback;
}

bool lookupBool(GVariant* dict, const char* key, bool fallback) {
    auto v = lookup(dict, key);
    return v && g_variant_is_of_type(v.get(), G_VARIANT_TYPE_BOOLEAN) ? g_variant_get_boolean(v.get()) : fallback;
}

double lookupDouble(GVariant* dict, const char* key, double fallback) {
    auto v = lookup(dict, key);
    if (!v) return fallback;
    if (g_variant_is_of_type(v.get(), G_VARIANT_TYPE_DOUBLE)) return g_variant_get_double(v.get());
    return static_cast<double>(lookupInt(dict, key, static_cast<std::int64_t>(fallback)));
}

std::int64_t lookupInt(GVariant* dict, const char* key, std::int64_t fallback) {
    auto v = lookup(dict, key);
    if (!v) return fallback;
    switch (g_variant_classify(v.get())) {
        case G_VARIANT_CLASS_INT32: return g_variant_get_int32(v.get());
        case G_VARIANT_CLASS_UINT32: return g_variant_get_uint32(v.get());
        case G_VARIANT_CLASS_INT64: return g_variant_get_int64(v.get());
        case G_VARIANT_CLASS_UINT64: return static_cast<std::int64_t>(g_variant_get_uint64(v.get()));
        case G_VARIANT_CLASS_INT16: return g_variant_get_int16(v.get());
        case G_VARIANT_CLASS_UINT16: return g_variant_get_uint16(v.get());
        case G_VARIANT_CLASS_BYTE: return g_variant_get_byte(v.get());
        case G_VARIANT_CLASS_DOUBLE: return std::llround(g_variant_get_double(v.get()));
        default: return fallback;
    }
}

std::vector<std::string> strv(GVariant* v) {
    std::vector<std::string> out;
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) return out;
    gsize n = 0;
    const char** items = g_variant_get_strv(v, &n);
    for (gsize i = 0; i < n; ++i) out.emplace_back(items[i]);
    g_free(static_cast<gpointer>(items));
    return out;
}

// Desktop entries -------------------------------------------------------------------------------------

std::string appName(const std::string& desktopId) {
    std::string id = desktopId;
    if (id.size() < 8 || id.compare(id.size() - 8, 8, ".desktop") != 0) id += ".desktop";
    Object<GDesktopAppInfo> info(g_desktop_app_info_new(id.c_str()));
    if (!info) return {};
    const char* name = g_app_info_get_display_name(G_APP_INFO(info.get()));
    return name ? name : std::string();
}

// Subprocesses ---------------------------------------------------------------------------------------

bool haveProgram(const char* name) {
    char* p = g_find_program_in_path(name);
    g_free(p);
    return p != nullptr;
}

ProcessResult runSync(const std::vector<std::string>& argv) {
    ProcessResult r;
    std::string error;
    GSubprocess* p = spawn(argv, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
                           error);
    if (!p) {
        r.err = error;
        return r;
    }
    GBytes* out = nullptr;
    GBytes* errOut = nullptr;
    GError* raw = nullptr;
    g_subprocess_communicate(p, nullptr, nullptr, &out, &errOut, &raw);
    Error err(raw);
    r.out = bytesToString(out);
    r.err = bytesToString(errOut);
    r.status = err ? -1 : exitStatus(p);
    g_object_unref(p);
    return r;
}

void run(const std::vector<std::string>& argv, std::function<void(ProcessResult)> done, bool mutates) {
    if (mutates && g_readOnly) {
        if (done) toUi([done] { done(ProcessResult{-1, {}, "Settings are read-only"}); });
        return;
    }
    std::string error;
    GSubprocess* p = spawn(argv, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
                           error);
    if (!p) {
        if (done) toUi([done, error] { done(ProcessResult{-1, {}, error}); });
        return;
    }
    g_subprocess_communicate_async(p, nullptr, nullptr, onRunDone, new RunData{std::move(done)});
}

WriteResult runWrite(const std::vector<std::string>& argv, WriteDone done) {
    if (gx::readOnly()) return WriteResult::readOnly();
    if (argv.empty()) return WriteResult::failure("No settings tool was supplied");
    run(argv, [done = std::move(done), tool = argv.front()](ProcessResult result) {
        if (done)
            done(result.ok()
                     ? WriteResult{}
                     : WriteResult::failure(
                           tool + ": " +
                           (result.err.empty() ? "exited with status " + std::to_string(result.status) : result.err)));
    });
    return {WriteStatus::Pending, {}};
}

void stream(const std::vector<std::string>& argv, std::function<void(const std::string& line)> onLineFn) {
    std::string error;
    GSubprocess* p = spawn(argv, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                           error);
    if (!p) return;
    g_streams.push_back(p);
    auto* sd = new StreamData{g_data_input_stream_new(g_subprocess_get_stdout_pipe(p)), std::move(onLineFn)};
    readNextLine(sd);
}

void background(std::function<void()> work) {
    std::lock_guard lock(g_workMutex);
    if (g_workStop) return;
    g_work.push_back(std::move(work));
    if (!g_worker.joinable()) {
        g_worker = std::thread([] {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock wait(g_workMutex);
                    g_workCv.wait(wait, [] { return g_workStop || !g_work.empty(); });
                    if (g_workStop) return;
                    job = std::move(g_work.front());
                    g_work.pop_front();
                }
                job();
            }
        });
    }
    g_workCv.notify_one();
}

// Debounce ---------------------------------------------------------------------------------------------

struct Debounce::State {
    double delay;
    std::function<void()> fn;
    bool pending = false;
};

Debounce::Debounce(double delay, std::function<void()> fn)
    : m_state(std::make_shared<State>(State{delay, std::move(fn)})) {}

void Debounce::operator()() {
    if (m_state->pending) return;
    m_state->pending = true;
    mayo::App::current().after(m_state->delay, [s = m_state] {
        s->pending = false;
        s->fn();
    });
}

}  // namespace gx
