#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mayonnaise/mayonnaise.hpp>
#include <mutex>
#include <thread>
#include "test_support.hpp"
#include "volumes_dbus.hpp"

using namespace finder;
static void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// Build real UDisks ObjectManager-shaped data, including UTF-8 labels and byte-string mount paths.
static GVariant* fixture() {
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    auto addDrive = [&](const char* path, bool usb) {
        GVariantBuilder props, interfaces;
        g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&props, "{sv}", "ConnectionBus", g_variant_new_string(usb ? "usb" : "ata"));
        g_variant_builder_add(&props, "{sv}", "CanPowerOff", g_variant_new_boolean(usb));
        g_variant_builder_add(&props, "{sv}", "Model", g_variant_new_string("USB Storage"));
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.freedesktop.UDisks2.Drive", g_variant_builder_end(&props));
        g_variant_builder_add(&objects, "{o@a{sa{sv}}}", path, g_variant_builder_end(&interfaces));
    };
    addDrive("/drives/usb", true);
    addDrive("/drives/system", false);
    auto addBlock = [&](const char* path, const char* drive, const char* label, const char* mount, bool system,
                        bool ignored, bool filesystem = true) {
        GVariantBuilder props, interfaces, fs;
        g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&props, "{sv}", "Drive", g_variant_new_object_path(drive));
        g_variant_builder_add(&props, "{sv}", "IdLabel", g_variant_new_string(label));
        g_variant_builder_add(&props, "{sv}", "HintSystem", g_variant_new_boolean(system));
        g_variant_builder_add(&props, "{sv}", "HintIgnore", g_variant_new_boolean(ignored));
        g_variant_builder_add(&props, "{sv}", "PreferredDevice", g_variant_new_bytestring("/dev/sdb1"));
        g_variant_builder_add(&props, "{sv}", "Size", g_variant_new_uint64(32000000000ULL));
        g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.freedesktop.UDisks2.Block", g_variant_builder_end(&props));
        if (filesystem) {
            g_variant_builder_init(&fs, G_VARIANT_TYPE_VARDICT);
            const char* mounts[] = {mount, nullptr};
            g_variant_builder_add(&fs, "{sv}", "MountPoints", g_variant_new_bytestring_array(mounts, mount ? 1 : 0));
            g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.freedesktop.UDisks2.Filesystem",
                                  g_variant_builder_end(&fs));
        }
        g_variant_builder_add(&objects, "{o@a{sa{sv}}}", path, g_variant_builder_end(&interfaces));
    };
    addBlock("/blocks/a", "/drives/usb", "随行 备份", nullptr, false, false);
    addBlock("/blocks/b", "/drives/usb", "Projects", "/run/media/user/项目 资料", false, false);
    addBlock("/blocks/c", "/drives/usb", "Hidden", "/mnt/hidden", false, true);
    addBlock("/blocks/root", "/drives/system", "Linux", "/", true, false);
    addBlock("/blocks/efi", "/drives/system", "EFI", "/boot", true, true);
    addBlock("/blocks/raw", "/drives/usb", "", nullptr, false, false, false);
    return g_variant_ref_sink(g_variant_builder_end(&objects));
}

// A private message bus: every operation below reaches this fake service, never a physical disk.
class FakeDisks {
public:
    GTestDBus* testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
    GDBusConnection* client = nullptr;
    GMainLoop* loop = nullptr;
    std::thread server;
    std::atomic<bool> failUnmount{false}, failMount{false}, forceSeen{false};
    std::mutex mutex;
    std::vector<std::string> calls;
    FakeDisks() {
        g_test_dbus_up(testBus);
        const auto* address = g_test_dbus_get_bus_address(testBus);
        auto connect = [address] {
            GError* error = nullptr;
            auto* bus = g_dbus_connection_new_for_address_sync(
                address,
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, &error);
            require(bus != nullptr, error ? error->message : "connect private test bus");
            return bus;
        };
        client = connect();
        std::promise<void> ready;
        auto future = ready.get_future();
        server = std::thread([&, connect] {
            auto* context = g_main_context_new();
            g_main_context_push_thread_default(context);
            auto* bus = connect();
            loop = g_main_loop_new(context, false);
            auto* info = g_dbus_node_info_new_for_xml(R"XML(<node>
              <interface name="org.freedesktop.DBus.ObjectManager"><method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method></interface>
              <interface name="org.freedesktop.UDisks2.Filesystem">
                <method name="Mount"><arg type="a{sv}" direction="in"/><arg type="s" direction="out"/></method>
                <method name="Unmount"><arg type="a{sv}" direction="in"/></method>
              </interface>
              <interface name="org.freedesktop.UDisks2.Drive">
                <method name="PowerOff"><arg type="a{sv}" direction="in"/></method>
                <method name="Eject"><arg type="a{sv}" direction="in"/></method>
              </interface></node>)XML",
                                                      nullptr);
            static const GDBusInterfaceVTable table{
                [](GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar* method,
                   GVariant* params, GDBusMethodInvocation* invocation, gpointer data) {
                    auto& self = *static_cast<FakeDisks*>(data);
                    const std::string call(method);
                    if (call == "GetManagedObjects") {
                        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{oa{sa{sv}}})", fixture()));
                        return;
                    }
                    {
                        std::lock_guard lock(self.mutex);
                        self.calls.push_back(std::string(path) + ":" + method);
                    }
                    auto* options = g_variant_get_child_value(params, 0);
                    gboolean force = false;
                    g_variant_lookup(options, "force", "b", &force);
                    if (force) self.forceSeen = true;
                    g_variant_unref(options);
                    if (call == "Unmount" && std::string(path) == "/blocks/c" && self.failUnmount) {
                        g_dbus_method_invocation_return_dbus_error(invocation,
                                                                   "org.freedesktop.UDisks2.Error.DeviceBusy", "busy");
                    } else if (call == "Mount" && self.failMount) {
                        g_dbus_method_invocation_return_dbus_error(
                            invocation, "org.freedesktop.UDisks2.Error.NotAuthorized", "denied");
                    } else if (call == "Mount") {
                        g_dbus_method_invocation_return_value(invocation,
                                                              g_variant_new("(s)", "/run/media/user/随行 备份"));
                    } else
                        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
                },
                nullptr,
                nullptr,
                {nullptr}};
            std::vector<guint> registrations;
            registrations.push_back(g_dbus_connection_register_object(
                bus, "/org/freedesktop/UDisks2", info->interfaces[0], &table, this, nullptr, nullptr));
            for (auto* path : {"/blocks/a", "/blocks/b", "/blocks/c"})
                registrations.push_back(
                    g_dbus_connection_register_object(bus, path, info->interfaces[1], &table, this, nullptr, nullptr));
            registrations.push_back(g_dbus_connection_register_object(bus, "/drives/usb", info->interfaces[2], &table,
                                                                      this, nullptr, nullptr));
            auto* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                      "org.freedesktop.DBus", "RequestName",
                                                      g_variant_new("(su)", "org.freedesktop.UDisks2", 0), nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
            require(reply != nullptr, "register fake UDisks service");
            g_variant_unref(reply);
            ready.set_value();
            g_main_loop_run(loop);
            for (const auto registration : registrations) g_dbus_connection_unregister_object(bus, registration);
            g_dbus_node_info_unref(info);
            g_object_unref(bus);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
        });
        future.wait();
    }
    ~FakeDisks() {
        g_main_loop_quit(loop);
        server.join();
        g_main_loop_unref(loop);
        g_object_unref(client);
        g_test_dbus_down(testBus);
        g_object_unref(testBus);
    }
    std::vector<std::string> takeCalls() {
        std::lock_guard lock(mutex);
        auto result = calls;
        calls.clear();
        return result;
    }
};

static void testBusOperations() {
    FakeDisks disks;
    auto result = performVolumeAction(disks.client, nullptr, "/blocks/a", VolumeManager::Action::Mount);
    require(result.error.empty() && result.mountPoint == "/run/media/user/随行 备份",
            "Mount returns exact service path");
    require(disks.takeCalls() == std::vector<std::string>{"/blocks/a:Mount"}, "Mount calls filesystem interface");
    result = performVolumeAction(disks.client, nullptr, "/blocks/b", VolumeManager::Action::Mount);
    require(result.error.empty() && result.mountPoint == "/run/media/user/项目 资料" && disks.takeCalls().empty(),
            "already mounted volume reuses mount without another call");
    result = performVolumeAction(disks.client, nullptr, "/blocks/a", VolumeManager::Action::Remove);
    require(result.error.empty() && result.poweredOff, "complete safe removal succeeds over D-Bus");
    require(
        disks.takeCalls() == std::vector<std::string>{"/blocks/b:Unmount", "/blocks/c:Unmount", "/drives/usb:PowerOff"},
        "real calls unmount every partition before power-off");
    disks.failUnmount = true;
    result = performVolumeAction(disks.client, nullptr, "/blocks/a", VolumeManager::Action::Remove);
    require(!result.error.empty() && !result.poweredOff, "busy device removal fails");
    require(disks.takeCalls() == std::vector<std::string>{"/blocks/b:Unmount", "/blocks/c:Unmount"},
            "failed unmount prevents power-off");
    require(!disks.forceSeen, "never sends force=true");
    disks.failMount = true;
    result = performVolumeAction(disks.client, nullptr, "/blocks/a", VolumeManager::Action::Mount);
    require(result.error.find("授权") != std::string::npos && result.mountPoint.empty(),
            "remote auth failure delivered to UI");
    disks.takeCalls();
    result = performVolumeAction(disks.client, nullptr, "/blocks/gone", VolumeManager::Action::Mount);
    require(!result.error.empty() && disks.takeCalls().empty(), "disconnected device never receives operation");
    auto* cancelled = g_cancellable_new();
    g_cancellable_cancel(cancelled);
    result = performVolumeAction(disks.client, cancelled, "/blocks/a", VolumeManager::Action::Remove);
    require(!result.error.empty() && disks.takeCalls().empty(), "shutdown cancellation prevents operation");
    g_object_unref(cancelled);
}

int main() {
    auto* objects = fixture();
    auto volumes = parseVolumeObjects(objects);
    g_variant_unref(objects);
    require(volumes.size() == 6, "retain hidden/system/raw blocks for removal safety");
    const auto& a = volumes[0];
    require(a.visible() && a.mountPoint.empty() && a.external && a.canRemove(),
            "unmounted USB is visible and actionable");
    require(a.name == "随行 备份" && a.size == 32000000000ULL && a.device == "/dev/sdb1", "label, size, device decode");
    require(volumes[1].mountPoint == "/run/media/user/项目 资料", "mount paths preserve UTF-8 and spaces");
    require(!volumes[2].visible() && !volumes[3].visible() && !volumes[4].visible() && !volumes[5].visible(),
            "hidden, system and raw devices stay out of the sidebar");
    std::string error;
    auto plan = removalPlan(volumes, a.id, error);
    require(error.empty() && plan.size() == 3, "all mounted USB partitions including hidden ones are removed");
    require(plan[0].method == "Unmount" && plan[1].method == "Unmount" && plan[2].method == "PowerOff",
            "unmount precedes power-off");
    require(plan[0].object == "/blocks/b" && plan[1].object == "/blocks/c", "only selected drive partitions unmounted");
    auto protectedVolumes = volumes;
    protectedVolumes[2].system = true;
    require(removalPlan(protectedVolumes, a.id, error).empty() && !error.empty(),
            "system partition prevents every removal call");
    require(removalPlan(volumes, "/blocks/root", error).empty() && !error.empty(), "root cannot be ejected");
    require(removalPlan(volumes, "/blocks/gone", error).empty() && !error.empty(), "unplug race reported");
    auto optical = volumes;
    optical[0].canPowerOff = false;
    optical[0].ejectable = true;
    plan = removalPlan(optical, a.id, error);
    require(error.empty() && plan.back().method == "Eject", "optical drives use Eject");
    optical[0].ejectable = false;
    plan = removalPlan(optical, a.id, error);
    require(error.empty() && plan.size() == 2 && plan.back().method == "Unmount",
            "no power-off capability falls back to safe unmount");
    auto sibling = volumes;
    sibling[0].siblingId = sibling[2].siblingId = "usb-enclosure";
    sibling[2].drive = "/drives/other_lun";
    plan = removalPlan(sibling, a.id, error);
    require(error.empty() && plan.size() == 3, "sibling logical drives in one enclosure are unmounted together");
    sibling[2].system = true;
    require(removalPlan(sibling, a.id, error).empty() && !error.empty(),
            "system volume in sibling drive blocks power-off");
    require(volumeError("org.freedesktop.UDisks2.Error.DeviceBusy", "raw").find("使用中") != std::string::npos,
            "busy error explained");
    require(
        volumeError("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed", "raw").find("授权") != std::string::npos,
        "auth error explained");
    require(volumeError("org.freedesktop.DBus.Error.ServiceUnknown", "raw").find("UDisks2") != std::string::npos,
            "missing service explained");
    require(parseVolumeObjects(nullptr).empty(), "empty inventory safe");

    // Exercise actual event dispatch: the eject target must never navigate or activate on mouse-down.
    using namespace mayo;
    test::TestHost host;
    SidebarItem item("usb", Icon::HardDrive, "USB");
    item.subtitle = "29.8 GB · 已挂载";
    item.actionIcon = Icon::Eject;
    SidebarItem disabled("hint", Icon::HardDrive, "尚未连接设备");
    disabled.enabled = false;
    auto sidebar = std::make_shared<SidebarView>(std::vector<SidebarItem>{item, disabled});
    int selected = 0, ejected = 0;
    sidebar->setOnSelect([&](const auto&) { ++selected; });
    sidebar->setOnAction([&](const auto&) { ++ejected; });
    host.attach(sidebar, {232, 500});
    host.dispatcher.pointerDown({{205, 23}, MouseButton::Left, 1});
    require(!selected && !ejected, "eject press neither navigates nor starts operation");
    host.dispatcher.pointerUp({{205, 23}, MouseButton::Left, 1});
    require(ejected == 1 && !selected, "eject release acts once without navigation");
    host.dispatcher.pointerDown({{205, 23}, MouseButton::Left, 1});
    host.dispatcher.pointerUp({{80, 23}, MouseButton::Left, 1});
    require(ejected == 1, "release outside action cancels");
    sidebar->setSelection("");
    require(sidebar->selection().empty(), "selection can clear after unplug");
    host.dispatcher.pointerDown({{80, 23}, MouseButton::Left, 1});
    host.dispatcher.pointerUp({{80, 23}, MouseButton::Left, 1});
    require(selected == 1 && sidebar->selection() == "usb", "row selects normally");
    sidebar->setSelection("");
    host.dispatcher.pointerDown({{80, 60}, MouseButton::Left, 1});
    host.dispatcher.pointerUp({{80, 60}, MouseButton::Left, 1});
    require(selected == 1 && sidebar->selection().empty(), "empty state cannot navigate");
    sidebar->setOnSelect([&](const std::string& id) {
        sidebar->setSelection("");
        require(id == "usb", "selection callback keeps device ID when controller changes selection");
    });
    host.dispatcher.pointerDown({{80, 23}, MouseButton::Left, 1});
    host.dispatcher.pointerUp({{80, 23}, MouseButton::Left, 1});
    item.busy = true;
    sidebar->setItems({item});
    host.attach(sidebar, {232, 500});
    host.dispatcher.pointerDown({{205, 23}, MouseButton::Left, 1});
    host.dispatcher.pointerUp({{205, 23}, MouseButton::Left, 1});
    require(ejected == 1, "busy row blocks repeat eject");
    testBusOperations();
    std::cout << "Finder volume, D-Bus and sidebar regressions passed\n";
}
