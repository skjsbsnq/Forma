#include <doctest/doctest.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <sys/socket.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "platform/wayland_material.hpp"
#include "ext-background-effect-v1-server.h"
#include "kos-surface-shape-v1-server.h"

namespace {
struct Server {
    wl_display* display = wl_display_create();
    wl_display* client = nullptr;
    wl_global* blurGlobal = nullptr;
    wl_global* shapeGlobal = nullptr;
    std::vector<wl_resource*> managers;
    std::atomic<bool> running{true};
    std::thread thread;
    std::mutex mutex;
    std::vector<std::function<void()>> commands;
    unsigned caps = 0;
    bool liveEffect = false;
    int effects = 0, destroyedEffects = 0, regions = 0, shapes = 0, geometry = 0;
    int firstRowInset = -1, firstRowWidth = 0;
    static Server& get(wl_resource* resource) { return *static_cast<Server*>(wl_resource_get_user_data(resource)); }
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
    static void effectDestroyed(wl_resource* resource) {
        auto& s = get(resource);
        s.liveEffect = false;
        ++s.destroyedEffects;
    }
    static void setBlur(wl_client*, wl_resource* resource, wl_resource*) { ++get(resource).regions; }
    static void createEffect(wl_client* client, wl_resource* manager, uint32_t id, wl_resource*) {
        auto& s = get(manager);
        if (s.liveEffect) {
            wl_resource_post_error(manager, EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
                                   "duplicate surface extension");
            return;
        }
        auto* resource = wl_resource_create(client, &ext_background_effect_surface_v1_interface, 1, id);
        static const struct ext_background_effect_surface_v1_interface impl{destroy, setBlur};
        wl_resource_set_implementation(resource, &impl, &s, effectDestroyed);
        s.liveEffect = true;
        ++s.effects;
    }
    static void managerDestroyed(wl_resource* resource) {
        auto& list = get(resource).managers;
        std::erase(list, resource);
    }
    static void bindBlur(wl_client* client, void* data, uint32_t, uint32_t id) {
        auto& s = *static_cast<Server*>(data);
        auto* resource = wl_resource_create(client, &ext_background_effect_manager_v1_interface, 1, id);
        static const struct ext_background_effect_manager_v1_interface impl{destroy, createEffect};
        wl_resource_set_implementation(resource, &impl, &s, managerDestroyed);
        s.managers.push_back(resource);
        ext_background_effect_manager_v1_send_capabilities(resource, s.caps);
    }
    static void shapeGeometry(wl_client*, wl_resource* resource, int32_t, int32_t, int32_t, int32_t) { ++get(resource).geometry; }
    static void shapeCorner(wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t) {}
    static void shapeFlag(wl_client*, wl_resource*, uint32_t) {}
    static void shapeScrim(wl_client*, wl_resource*, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void shapeBlur(wl_client*, wl_resource*, uint32_t, uint32_t) {}
    static void createShape(wl_client* client, wl_resource* manager, uint32_t id, wl_resource*) {
        auto& s = get(manager);
        auto* resource = wl_resource_create(client, &kos_surface_shape_v1_interface, 4, id);
        static const struct kos_surface_shape_v1_interface impl{
            shapeGeometry, shapeCorner, shapeFlag, shapeFlag, destroy, shapeScrim, shapeBlur,
            nullptr, nullptr, nullptr};
        wl_resource_set_implementation(resource, &impl, &s, nullptr);
        ++s.shapes;
    }
    static void bindShape(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* resource = wl_resource_create(client, &kos_surface_shape_manager_v1_interface, static_cast<int>(version), id);
        static const struct kos_surface_shape_manager_v1_interface impl{createShape, destroy};
        wl_resource_set_implementation(resource, &impl, data, nullptr);
    }
    static void regionRect(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t width, int32_t) {
        if (y == 0) {
            get(resource).firstRowInset = x;
            get(resource).firstRowWidth = width;
        }
    }
    static void createRegion(wl_client* client, wl_resource* compositor, uint32_t id) {
        auto* resource = wl_resource_create(client, &wl_region_interface, 1, id);
        static const struct wl_region_interface impl{destroy, regionRect, regionRect};
        wl_resource_set_implementation(resource, &impl, &get(compositor), nullptr);
    }
    static void createSurface(wl_client* client, wl_resource* compositor, uint32_t id) {
        auto* resource = wl_resource_create(client, &wl_surface_interface, 1, id);
        static const struct wl_surface_interface impl = [] {
            struct wl_surface_interface value{};
            value.destroy = destroy;
            return value;
        }();
        wl_resource_set_implementation(resource, &impl, &get(compositor), nullptr);
    }
    static void bindCompositor(wl_client* client, void* data, uint32_t, uint32_t id) {
        auto* resource = wl_resource_create(client, &wl_compositor_interface, 1, id);
        static const struct wl_compositor_interface impl = [] {
            struct wl_compositor_interface value{};
            value.create_surface = createSurface;
            value.create_region = createRegion;
            return value;
        }();
        wl_resource_set_implementation(resource, &impl, data, nullptr);
    }
    void announce() {
        blurGlobal = wl_global_create(display, &ext_background_effect_manager_v1_interface, 1, this, bindBlur);
        shapeGlobal = wl_global_create(display, &kos_surface_shape_manager_v1_interface, 4, this, bindShape);
    }
    Server() {
        int fds[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        REQUIRE(wl_client_create(display, fds[0]) != nullptr);
        client = wl_display_connect_to_fd(fds[1]);
        REQUIRE(client != nullptr);
        wl_global_create(display, &wl_compositor_interface, 1, this, bindCompositor);
        announce();
        thread = std::thread([this] {
            while (running) {
                std::vector<std::function<void()>> work;
                { std::lock_guard lock(mutex); work.swap(commands); }
                for (auto& fn : work) fn();
                wl_event_loop_dispatch(wl_display_get_event_loop(display), 5);
                wl_display_flush_clients(display);
            }
        });
    }
    void command(std::function<void()> fn) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(mutex);
            commands.push_back([fn, promise] { fn(); promise->set_value(); });
        }
        REQUIRE(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    }
    void capability(unsigned value) {
        command([this, value] {
            caps = value;
            for (auto* manager : managers) ext_background_effect_manager_v1_send_capabilities(manager, caps);
        });
    }
    void withdraw() {
        command([this] { wl_global_destroy(blurGlobal); wl_global_destroy(shapeGlobal); blurGlobal = shapeGlobal = nullptr; });
    }
    ~Server() {
        running = false;
        thread.join();
        wl_display_disconnect(client);
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }
};
struct Client {
    wl_compositor* compositor = nullptr;
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        if (std::string(interface) == "wl_compositor")
            static_cast<Client*>(data)->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    }
    static void removed(void*, wl_registry*, uint32_t) {}
};
}

TEST_CASE("Compositor-owned frame receives a full blur surface without a second client contour") {
    Server server;
    server.capability(EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
    Client c;
    auto* registry = wl_display_get_registry(server.client);
    static const wl_registry_listener listener{Client::global, Client::removed};
    wl_registry_add_listener(registry, &listener, &c);
    REQUIRE(wl_display_roundtrip(server.client) >= 0);
    REQUIRE(c.compositor != nullptr);
    auto* surface = wl_compositor_create_surface(c.compositor);
    {
        mayo::platform::WaylandMaterial material(server.client, surface, 28, true);
        REQUIRE(material.available());
        material.update(300, 320, 28);
        REQUIRE(wl_display_roundtrip(server.client) >= 0);
        server.command([&] {
            CHECK(server.effects == 1);
            CHECK(server.regions == 1);
            CHECK(server.shapes == 0);
            CHECK(server.firstRowInset == 0);
            CHECK(server.firstRowWidth == 300);
        });
    }
    wl_surface_destroy(surface);
    wl_compositor_destroy(c.compositor);
    wl_registry_destroy(registry);
    REQUIRE(wl_display_roundtrip(server.client) >= 0);
    CHECK(wl_display_get_error(server.client) == 0);
}

TEST_CASE("Wayland material recovers capability and manager globals without duplicate surface extensions") {
    Server server;
    Client c;
    auto* registry = wl_display_get_registry(server.client);
    static const wl_registry_listener listener{Client::global, Client::removed};
    wl_registry_add_listener(registry, &listener, &c);
    REQUIRE(wl_display_roundtrip(server.client) >= 0);
    REQUIRE(c.compositor != nullptr);
    auto* surface = wl_compositor_create_surface(c.compositor);
    {
        mayo::platform::WaylandMaterial material(server.client, surface, 28);
        auto pump = [&] {
            REQUIRE(wl_display_roundtrip(server.client) >= 0);
            material.update(300, 320, 28);
            REQUIRE(wl_display_roundtrip(server.client) >= 0);
            material.update(300, 320, 28);
            REQUIRE(wl_display_roundtrip(server.client) >= 0);
        };
        CHECK_FALSE(material.available());
        server.capability(EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
        pump();
        CHECK(material.available());
        server.command([&] { CHECK(server.effects == 1); CHECK(server.regions == 1); CHECK(server.shapes == 1); });
        server.capability(0);
        pump();
        CHECK_FALSE(material.available());
        server.capability(EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
        pump();
        CHECK(material.available());
        server.command([&] { CHECK(server.effects == 1); CHECK(server.regions == 2); });
        server.withdraw();
        pump();
        CHECK_FALSE(material.available());
        server.command([&] { CHECK(server.destroyedEffects == 1); server.announce(); });
        pump();
        CHECK(material.available());
        server.command([&] {
            CHECK(server.effects == 2);
            CHECK(server.regions == 3);
            CHECK(server.shapes == 2);
            CHECK(server.geometry == 3);
        });
        CHECK(wl_display_get_error(server.client) == 0);
    }
    wl_surface_destroy(surface);
    wl_compositor_destroy(c.compositor);
    wl_registry_destroy(registry);
    CHECK(wl_display_roundtrip(server.client) >= 0);
}
