// GLib / GIO glue for the Settings example: GSettings keys, D-Bus calls and subprocesses, with every
// callback delivered on the UI thread.
//
// GLib's default main context runs on a background thread (start()). GSettings objects, D-Bus signal
// subscriptions and async calls are all created on the UI thread, whose thread-default context is that
// global default, so GLib dispatches their callbacks on the background thread; those only hop over with
// App::post. Panels therefore never see GLib's thread.
#pragma once

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mayo {
class App;
}

namespace gx {

struct VariantUnref {
    void operator()(GVariant* v) const {
        if (v) g_variant_unref(v);
    }
};
/// Owning GVariant reference.
using Variant = std::unique_ptr<GVariant, VariantUnref>;
/// Takes a full reference (and sinks a floating one).
inline Variant hold(GVariant* v) { return Variant(v ? g_variant_ref_sink(v) : nullptr); }

struct ObjectUnref {
    void operator()(gpointer o) const {
        if (o) g_object_unref(o);
    }
};
template <class T>
using Object = std::unique_ptr<T, ObjectUnref>;

// Lifetime -------------------------------------------------------------------------------------------

/// Starts GLib's main loop thread. Call once, after the App exists.
void start(mayo::App& app);
/// Stops subprocess streams and the main loop thread and drops every callback. Call before the App dies.
void stop();
/// Runs `fn` on the UI thread (no-op once stopped).
void toUi(std::function<void()> fn);

/// Read-only mode (screenshot / demo runs): writes are rejected, never reported as successful.
void setReadOnly(bool readOnly);
[[nodiscard]] bool readOnly();

// GSettings ------------------------------------------------------------------------------------------

[[nodiscard]] bool hasSchema(const std::string& schema);

enum class WriteStatus { Success, Failure, ReadOnly, Pending };

struct WriteResult {
    WriteStatus status = WriteStatus::Success;
    std::string error;
    [[nodiscard]] bool ok() const { return status == WriteStatus::Success; }
    [[nodiscard]] bool pending() const { return status == WriteStatus::Pending; }
    static WriteResult failure(std::string reason) { return {WriteStatus::Failure, std::move(reason)}; }
    static WriteResult readOnly(std::string reason = "Settings are read-only") {
        return {WriteStatus::ReadOnly, std::move(reason)};
    }
};
using WriteDone = std::function<void(WriteResult)>;
/// An asynchronous writer returns Pending and calls done exactly once with its final result.
using Writer = std::function<WriteResult(const std::optional<std::string>&, WriteDone)>;

/// Storage for a key that isn't GSettings: a KDE config file entry, a D-Bus property. Values travel as text
/// (KConfig's own format: "true", "12", "1.5", lists joined with commas) and `type` says how to read them.
struct KeyBackend {
    std::string type;      ///< "b", "i", "d", "s" or "as"
    std::string fallback;  ///< value text when nothing is stored
    std::function<std::optional<std::string>()> read;               ///< nullopt: not set (the fallback applies)
    Writer write;                                                   ///< nullopt: reset to the default
    std::function<void(std::function<void()>)> watch;               ///< optional
    std::function<bool()> writable;  ///< optional capability / policy check, independent of availability
    std::optional<std::pair<double, double>> range;
};

/// One key of a GSettings schema. A missing schema or key (another desktop, an older GNOME) makes the key
/// unavailable instead of aborting the process, which is what g_settings_new does. Keys can also be backed by
/// a KeyBackend (KDE), so the rows bound to keys work on either desktop.
class Key {
public:
    Key() = default;
    /// `path` is for relocatable schemas (per-application notification settings).
    Key(const std::string& schema, std::string key, const std::string& path = {}, GSettingsBackend* backend = nullptr);
    explicit Key(std::shared_ptr<const KeyBackend> backend, std::string name = {})
        : m_key(std::move(name)), m_backend(std::move(backend)) {}

    [[nodiscard]] bool available() const { return m_settings != nullptr || m_backend != nullptr; }
    explicit operator bool() const { return available(); }
    [[nodiscard]] bool writable() const;
    [[nodiscard]] bool readOnly() const { return available() && !writable(); }
    [[nodiscard]] bool pending() const;
    [[nodiscard]] const std::string& name() const { return m_key; }
    [[nodiscard]] GSettings* settings() const { return m_settings; }
    /// GVariant type string of the value ("b", "s", "i", "u", "d", "as", "a(ss)"...).
    [[nodiscard]] std::string type() const;

    [[nodiscard]] Variant value() const;
    [[nodiscard]] bool getBool(bool fallback = false) const;
    [[nodiscard]] std::int64_t getInt(std::int64_t fallback = 0) const;  ///< i, u, x, t, y, n, q
    [[nodiscard]] double getDouble(double fallback = 0) const;           ///< d (and integers)
    [[nodiscard]] std::string getString(const std::string& fallback = {}) const;  ///< s and enum nicks
    [[nodiscard]] std::vector<std::string> getStrv() const;
    /// The value as text: strings verbatim, numbers in decimal, booleans "true"/"false".
    [[nodiscard]] std::string getText() const;

    /// GSettings success means backend acceptance (GIO exposes no asynchronous dconf disk-commit result).
    /// External writers return Pending until done; copies of a Key share that pending state.
    WriteResult set(bool v, WriteDone done = {}) const;
    WriteResult setInt(std::int64_t v, WriteDone done = {}) const;
    WriteResult setDouble(double v, WriteDone done = {}) const;
    WriteResult setString(const std::string& v, WriteDone done = {}) const;
    WriteResult setStrv(const std::vector<std::string>& v, WriteDone done = {}) const;
    /// Parses `text` the way getText() writes it, for the key's own type.
    WriteResult setText(const std::string& text, WriteDone done = {}) const;
    /// Takes ownership of a floating variant.
    WriteResult setValue(GVariant* v, WriteDone done = {}) const;
    WriteResult reset(WriteDone done = {}) const;
    [[nodiscard]] bool isDefault() const;

    /// Allowed range of a numeric key ("range" in the schema), if any.
    [[nodiscard]] std::optional<std::pair<double, double>> range() const;
    /// Allowed values of an enum/choices key.
    [[nodiscard]] std::vector<std::string> choices() const;

    /// `fn` runs on the UI thread whenever the key changes, from this app or any other.
    void watch(std::function<void()> fn) const;
    /// UI-thread write lifecycle, including asynchronous completion (separate from value changes).
    void watchWrite(std::function<void(const Key&, const WriteResult&)> fn) const;

private:
    [[nodiscard]] std::optional<std::string> backendText() const;
    WriteResult submit(std::function<WriteResult(WriteDone)> write, WriteDone done) const;
    WriteResult reject(std::string reason, WriteDone done) const;
    struct WriteState;
    std::shared_ptr<WriteState> m_writeState = makeWriteState();
    static std::shared_ptr<WriteState> makeWriteState();

    GSettings* m_settings = nullptr;  // owned by a process-wide cache
    std::string m_key;
    std::shared_ptr<const KeyBackend> m_backend;
};

/// Fallback for legacy hand-built panel actions without a write observer / completion callback.
/// Cleared by stop(); bound rows report their own errors instead.
void setWriteErrorHandler(std::function<void(const Key&, const WriteResult&)> handler);

/// KConfig list syntax: values joined with commas, literal commas escaped.
[[nodiscard]] std::vector<std::string> splitList(const std::string& text);
[[nodiscard]] std::string joinList(const std::vector<std::string>& items);

// D-Bus ----------------------------------------------------------------------------------------------

[[nodiscard]] GDBusConnection* systemBus();
[[nodiscard]] GDBusConnection* sessionBus();

/// Whether a name currently has an owner on `bus` (a service that is running or activatable is not
/// distinguished; activatable names are reported as present).
[[nodiscard]] bool hasName(GDBusConnection* bus, const char* name);

/// Synchronous property read (short timeout); nullptr on any error. Returns the unboxed value.
[[nodiscard]] Variant property(GDBusConnection* bus, const char* name, const char* path, const char* iface,
                               const char* prop);
/// All properties of an interface as a{sv}; nullptr on error.
[[nodiscard]] Variant properties(GDBusConnection* bus, const char* name, const char* path, const char* iface);
/// Synchronous call for quick reads at panel construction; nullptr on error (message in `error`).
[[nodiscard]] Variant callSync(GDBusConnection* bus, const char* name, const char* path, const char* iface,
                               const char* method, GVariant* params, const char* replyType = nullptr,
                               std::string* error = nullptr, int timeoutMs = 3000);

using Reply = std::function<void(Variant reply, const std::string& error)>;
/// Asynchronous call that only reads. `done` runs on the UI thread.
void call(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
          GVariant* params, Reply done, int timeoutMs = 25000);
/// Asynchronous call that changes something; skipped in read-only mode. Allows polkit to ask for a password.
void perform(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* method,
             GVariant* params, Reply done = {}, int timeoutMs = 120000);
void setProperty(GDBusConnection* bus, const char* name, const char* path, const char* iface, const char* prop,
                 GVariant* value, Reply done = {});

using SignalFn = std::function<void(const std::string& path, const std::string& member, GVariant* params)>;
/// Subscribes to a signal (null arguments are wildcards); `fn` runs on the UI thread.
void subscribe(GDBusConnection* bus, const char* sender, const char* iface, const char* member, const char* path,
               SignalFn fn);

// Variant helpers --------------------------------------------------------------------------------------

[[nodiscard]] std::string str(GVariant* v);
[[nodiscard]] Variant lookup(GVariant* dict, const char* key);  ///< value of an a{sv} entry
[[nodiscard]] std::string lookupString(GVariant* dict, const char* key, const std::string& fallback = {});
[[nodiscard]] bool lookupBool(GVariant* dict, const char* key, bool fallback = false);
[[nodiscard]] double lookupDouble(GVariant* dict, const char* key, double fallback = 0);
[[nodiscard]] std::int64_t lookupInt(GVariant* dict, const char* key, std::int64_t fallback = 0);
[[nodiscard]] std::vector<std::string> strv(GVariant* v);

// Desktop entries -------------------------------------------------------------------------------------

/// Display name of an installed application ("org.gnome.Nautilus.desktop" -> "Files"); empty if unknown.
[[nodiscard]] std::string appName(const std::string& desktopId);

// Subprocesses ---------------------------------------------------------------------------------------

struct ProcessResult {
    int status = -1;  ///< exit status, -1 when the program could not be started
    std::string out;
    std::string err;
    [[nodiscard]] bool ok() const { return status == 0; }
};

[[nodiscard]] bool haveProgram(const char* name);
/// Blocking run for quick reads while building a panel.
[[nodiscard]] ProcessResult runSync(const std::vector<std::string>& argv);
/// Runs asynchronously; `done` on the UI thread. `mutates` runs are skipped in read-only mode.
void run(const std::vector<std::string>& argv, std::function<void(ProcessResult)> done = {}, bool mutates = true);
/// Mutation with an explicit final write result (stderr / exit status on failure).
WriteResult runWrite(const std::vector<std::string>& argv, WriteDone done);
/// Long-running process (pactl subscribe, nmcli monitor) whose stdout lines arrive on the UI thread.
/// Killed by stop().
void stream(const std::vector<std::string>& argv, std::function<void(const std::string& line)> onLine);

/// Runs `work` on a background worker thread (decoding thumbnails, slow files); deliver results with toUi().
/// Pending work is dropped by stop(), which waits for the running item.
void background(std::function<void()> work);

// UI-thread helpers -----------------------------------------------------------------------------------

/// Coalesces bursts of change notifications into one call `delay` seconds after the first.
class Debounce {
public:
    Debounce(double delay, std::function<void()> fn);
    void operator()();

private:
    struct State;
    std::shared_ptr<State> m_state;
};

}  // namespace gx
