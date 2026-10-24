#include "volumes_dbus.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace finder {
namespace {
constexpr auto kService = "org.freedesktop.UDisks2";
constexpr auto kRoot = "/org/freedesktop/UDisks2";
constexpr auto kBlock = "org.freedesktop.UDisks2.Block";
constexpr auto kDrive = "org.freedesktop.UDisks2.Drive";
constexpr auto kFilesystem = "org.freedesktop.UDisks2.Filesystem";
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
Variant variant(GVariant* v) { return {v, g_variant_unref}; }
std::string stringProperty(GVariant* props, const char* key, const char* type = "&s") {
    const char* value = nullptr;
    return props && g_variant_lookup(props, key, type, &value) ? value : "";
}
bool boolProperty(GVariant* props, const char* key) {
    gboolean value = false;
    return props && g_variant_lookup(props, key, "b", &value) && value;
}
std::string errorText(GError* error) {
    if (!error) return "无法连接磁盘服务。";
    gchar* remote = g_dbus_error_get_remote_error(error);
    const std::string message = volumeError(remote ? remote : "", error->message);
    g_free(remote);
    g_error_free(error);
    return message;
}
}  // namespace

bool sameDevice(const DeviceVolume& a, const DeviceVolume& b) {
    return (!a.drive.empty() && a.drive != "/" && a.drive == b.drive) ||
           (!a.siblingId.empty() && a.siblingId == b.siblingId);
}

std::vector<VolumeCall> removalPlan(const std::vector<DeviceVolume>& volumes, const std::string& id,
                                    std::string& error) {
    error.clear();
    const auto selected = std::find_if(volumes.begin(), volumes.end(), [&](const auto& v) { return v.id == id; });
    if (selected == volumes.end()) {
        error = "设备已断开，请重新插入后再试。";
        return {};
    }
    if (!selected->canRemove()) {
        error = "此磁盘不支持安全弹出。";
        return {};
    }
    std::vector<VolumeCall> calls;
    for (const auto& v : volumes) {
        if (!sameDevice(*selected, v)) continue;
        if (v.system) {
            error = "此设备包含系统正在使用的分区，无法安全弹出。";
            return {};
        }
        if (v.mountable && !v.mountPoint.empty()) calls.push_back({v.id, kFilesystem, "Unmount"});
    }
    if (selected->canPowerOff)
        calls.push_back({selected->drive, kDrive, "PowerOff"});
    else if (selected->ejectable)
        calls.push_back({selected->drive, kDrive, "Eject"});
    return calls;
}

std::string volumeError(const std::string& name, const std::string& detail) {
    if (name.ends_with("DeviceBusy")) return "设备仍在使用中。请关闭正在访问它的文件、终端或应用后重试。";
    if (name.find("NotAuthorized") != std::string::npos)
        return "未获得操作授权。请完成系统验证后重试，并确认身份验证代理正在运行。";
    if (name.ends_with("ServiceUnknown") || name.ends_with("NameHasNoOwner"))
        return "磁盘服务不可用，请安装并启动 UDisks2。";
    if (name.ends_with("UnknownObject")) return "设备已断开，请重新插入后再试。";
    if (name.ends_with("NotSupported")) return "系统不支持此文件系统或设备操作。";
    return detail.empty() ? "磁盘操作失败，请检查设备连接后重试。" : detail;
}

std::vector<DeviceVolume> parseVolumeObjects(GVariant* objects) {
    std::vector<DeviceVolume> out;
    if (!objects || !g_variant_is_of_type(objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"))) return out;
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const char* path = nullptr;
    GVariant* interfaces = nullptr;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        auto ifaces = variant(interfaces);
        auto block = variant(g_variant_lookup_value(ifaces.get(), kBlock, G_VARIANT_TYPE_VARDICT));
        if (!block) continue;
        DeviceVolume v;
        v.id = path;
        v.drive = stringProperty(block.get(), "Drive", "&o");
        v.name = stringProperty(block.get(), "HintName", "&s");
        if (v.name.empty()) v.name = stringProperty(block.get(), "IdLabel", "&s");
        v.filesystem = stringProperty(block.get(), "IdType", "&s");
        v.system = boolProperty(block.get(), "HintSystem");
        v.ignored = boolProperty(block.get(), "HintIgnore");
        v.readOnly = boolProperty(block.get(), "ReadOnly");
        g_variant_lookup(block.get(), "Size", "t", &v.size);
        auto device = variant(g_variant_lookup_value(block.get(), "PreferredDevice", G_VARIANT_TYPE_BYTESTRING));
        if (device) v.device = g_variant_get_bytestring(device.get());
        auto driveIfaces =
            variant(v.drive.empty() ? nullptr : g_variant_lookup_value(objects, v.drive.c_str(), nullptr));
        auto drive =
            variant(driveIfaces ? g_variant_lookup_value(driveIfaces.get(), kDrive, G_VARIANT_TYPE_VARDICT) : nullptr);
        v.external = stringProperty(drive.get(), "ConnectionBus", "&s") == "usb" ||
                     boolProperty(drive.get(), "Removable") || boolProperty(drive.get(), "MediaRemovable");
        v.canPowerOff = boolProperty(drive.get(), "CanPowerOff");
        v.ejectable = boolProperty(drive.get(), "Ejectable");
        v.siblingId = stringProperty(drive.get(), "SiblingId", "&s");
        if (v.name.empty()) v.name = stringProperty(drive.get(), "Model", "&s");
        if (v.name.empty()) v.name = v.device.empty() ? "未命名磁盘" : v.device.substr(v.device.find_last_of('/') + 1);
        auto fs = variant(g_variant_lookup_value(ifaces.get(), kFilesystem, G_VARIANT_TYPE_VARDICT));
        v.mountable = fs != nullptr;
        auto mounts = variant(fs ? g_variant_lookup_value(fs.get(), "MountPoints", G_VARIANT_TYPE("aay")) : nullptr);
        if (mounts && g_variant_n_children(mounts.get()) > 0) {
            auto mount = variant(g_variant_get_child_value(mounts.get(), 0));
            v.mountPoint = g_variant_get_bytestring(mount.get());
        }
        out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return out;
}

std::vector<DeviceVolume> readVolumeObjects(GDBusConnection* bus, GCancellable* cancel, std::string& error) {
    error.clear();
    GError* failure = nullptr;
    auto reply = variant(g_dbus_connection_call_sync(bus, kService, kRoot, "org.freedesktop.DBus.ObjectManager",
                                                     "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                     G_DBUS_CALL_FLAGS_NONE, 5000, cancel, &failure));
    if (!reply) {
        error = errorText(failure);
        return {};
    }
    auto objects = variant(g_variant_get_child_value(reply.get(), 0));
    return parseVolumeObjects(objects.get());
}
VolumeManager::Result performVolumeAction(GDBusConnection* bus, GCancellable* cancel, const std::string& id,
                                          VolumeManager::Action action) {
    VolumeManager::Result result;
    auto volumes = readVolumeObjects(bus, cancel, result.error);
    if (!result.error.empty()) return result;
    std::vector<VolumeCall> calls;
    if (action == VolumeManager::Action::Remove) {
        calls = removalPlan(volumes, id, result.error);
    } else {
        auto v = std::find_if(volumes.begin(), volumes.end(), [&](const auto& item) { return item.id == id; });
        if (v == volumes.end())
            result.error = "设备已断开，请重新插入后再试。";
        else if (!v->visible())
            result.error = "此分区无法挂载。";
        else if (!v->mountPoint.empty())
            result.mountPoint = v->mountPoint;
        else
            calls.push_back({v->id, kFilesystem, "Mount"});
    }
    if (!result.error.empty()) return result;
    for (const auto& call : calls) {
        GError* failure = nullptr;
        auto reply = variant(g_dbus_connection_call_sync(bus, kService, call.object.c_str(), call.interface.c_str(),
                                                         call.method.c_str(), g_variant_new("(a{sv})", nullptr),
                                                         nullptr, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                                                         120000, cancel, &failure));
        if (!reply) {
            result.error = errorText(failure);
            return result;
        }  // never force-unmount or power off after failure
        if (call.method == "Mount") {
            const char* mount = nullptr;
            g_variant_get(reply.get(), "(&s)", &mount);
            result.mountPoint = mount;
        }
        if (call.method == "PowerOff" || call.method == "Eject") result.poweredOff = true;
    }
    return result;
}

struct VolumeManager::Impl {
    mayo::App& app;
    Changed changed;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    GCancellable* cancel = g_cancellable_new();
    std::mutex mutex;
    std::condition_variable cv;
    struct Task {
        std::string id;
        Action action;
        std::function<void(Result)> done;
    };
    std::optional<Task> task;
    bool stop = false;
    bool busy = false;  // UI thread only
    std::thread thread;

    Impl(mayo::App& owner, Changed fn) : app(owner), changed(std::move(fn)), thread([this] { run(); }) {}
    ~Impl() {
        *alive = false;
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        g_cancellable_cancel(cancel);
        cv.notify_one();
        thread.join();
        g_object_unref(cancel);
    }
    void run() {
        std::vector<DeviceVolume> previous;
        std::string previousError;
        bool first = true;
        GDBusConnection* bus = nullptr;
        for (;;) {
            std::optional<Task> work;
            {
                std::lock_guard lock(mutex);
                if (stop) break;
                work.swap(task);
            }
            std::string error;
            if (!bus || g_dbus_connection_is_closed(bus)) {
                if (bus) g_object_unref(bus);
                GError* failure = nullptr;
                bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancel, &failure);
                if (!bus) error = errorText(failure);
            }
            if (work) {
                auto result = bus ? performVolumeAction(bus, cancel, work->id, work->action) : Result{error, {}, false};
                app.post([this, guard = alive, done = std::move(work->done), result = std::move(result)]() mutable {
                    if (!*guard) return;
                    busy = false;
                    if (done) done(std::move(result));
                });
            }
            auto volumes = bus ? readVolumeObjects(bus, cancel, error) : std::vector<DeviceVolume>{};
            if (first || volumes != previous || error != previousError) {
                previous = volumes;
                previousError = error;
                first = false;
                app.post(
                    [guard = alive, fn = changed, volumes = std::move(volumes), error = std::move(error)]() mutable {
                        if (*guard && fn) fn(std::move(volumes), std::move(error));
                    });
            }
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return stop || task.has_value(); });
        }
        if (bus) g_object_unref(bus);
    }
};

VolumeManager::VolumeManager(mayo::App& app, Changed changed)
    : m_impl(std::make_unique<Impl>(app, std::move(changed))) {}
VolumeManager::~VolumeManager() = default;
bool VolumeManager::busy() const { return m_impl->busy; }
void VolumeManager::perform(std::string id, Action action, std::function<void(Result)> done) {
    if (m_impl->busy) return;
    m_impl->busy = true;
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->task = Impl::Task{std::move(id), action, std::move(done)};
    }
    m_impl->cv.notify_one();
}
}  // namespace finder
