#include "wayland_material.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef MAYONNAISE_WAYLAND_MATERIAL
#include <wayland-client.h>
#include "ext-background-effect-v1-client.h"
#include "kos-surface-shape-v1-client.h"
#endif

namespace mayo::platform {
struct WaylandMaterial::Impl {
#ifdef MAYONNAISE_WAYLAND_MATERIAL
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    ext_background_effect_manager_v1* manager = nullptr;
    ext_background_effect_surface_v1* effect = nullptr;
    kos_surface_shape_manager_v1* shapeManager = nullptr;
    kos_surface_shape_v1* shape = nullptr;
    uint32_t capabilities = 0;
    uint32_t compositorName = 0, managerName = 0, shapeManagerName = 0;
    bool failed = false;
    bool compositorFrame = false;
    wl_surface* surface = nullptr;
    void resetGeometry() { width = height = 0; radius = -1; }
    void ensureObjects() {
        if (failed || !compositor || !manager || !surface ||
            !(capabilities & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR)) return;
        if (!effect) {
            effect = ext_background_effect_manager_v1_get_background_effect(manager, surface);
            resetGeometry();
        }
        if (!compositorFrame && shapeManager && !shape) {
            shape = kos_surface_shape_manager_v1_get_shape(shapeManager, surface);
            kos_surface_shape_v1_set_enabled(shape, 1);
            resetGeometry();
        }
    }
    int width = 0, height = 0;
    float radius = -1;
    static void capability(void* data, ext_background_effect_manager_v1*, uint32_t flags) {
        auto& self = *static_cast<Impl*>(data);
        self.capabilities = flags;
        // Capability loss disables rendering but does not destroy a surface extension.
        // Retain it to avoid a duplicate get_background_effect on capability restoration.
        self.resetGeometry();
    }
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto& self = *static_cast<Impl*>(data);
        if (self.failed) return;
        if (std::strcmp(interface, "wl_compositor") == 0 && !self.compositor) {
            self.compositorName = name;
            self.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        }
        else if (std::strcmp(interface, "ext_background_effect_manager_v1") == 0 && !self.manager) {
            self.managerName = name;
            self.manager = static_cast<ext_background_effect_manager_v1*>(
                wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
            static const ext_background_effect_manager_v1_listener listener{capability};
            ext_background_effect_manager_v1_add_listener(self.manager, &listener, &self);
        } else if (std::strcmp(interface, "kos_surface_shape_manager_v1") == 0 && !self.shapeManager) {
            self.shapeManagerName = name;
            self.shapeManager = static_cast<kos_surface_shape_manager_v1*>(
                wl_registry_bind(registry, name, &kos_surface_shape_manager_v1_interface, std::min(version, 4u)));
        }
    }
    static void removed(void* data, wl_registry*, uint32_t name) {
        auto& self = *static_cast<Impl*>(data);
        // Removing a global does not invalidate its bound objects (Wayland registry
        // semantics). Send their protocol destructors before binding a replacement,
        // otherwise the surviving per-surface extension would trigger already_exists.
        if (name == self.managerName) {
            if (self.effect) ext_background_effect_surface_v1_destroy(self.effect);
            self.effect = nullptr;
            if (self.manager) ext_background_effect_manager_v1_destroy(self.manager);
            self.manager = nullptr;
            self.managerName = 0;
            self.capabilities = 0;
        }
        if (name == self.shapeManagerName) {
            if (self.shape) kos_surface_shape_v1_destroy(self.shape);
            self.shape = nullptr;
            if (self.shapeManager) kos_surface_shape_manager_v1_destroy(self.shapeManager);
            self.shapeManager = nullptr;
            self.shapeManagerName = 0;
        }
        if (name == self.compositorName) {
            if (self.compositor) wl_compositor_destroy(self.compositor);
            self.compositor = nullptr;
            self.compositorName = 0;
        }
        self.resetGeometry();
    }
#endif
};
WaylandMaterial::WaylandMaterial(void* display, void* surface, float radius, bool compositorFrame)
    : m_impl(std::make_unique<Impl>()) {
#ifdef MAYONNAISE_WAYLAND_MATERIAL
    if (!display || !surface) return;
    auto& d = *m_impl;
    d.compositorFrame = compositorFrame;
    d.display = static_cast<wl_display*>(display);
    d.surface = static_cast<wl_surface*>(surface);
    d.queue = wl_display_create_queue(d.display);
    // Wrappers route only our newly created objects to our queue. SDL retains ownership of its display/surface.
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(d.display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), d.queue);
    d.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    static const wl_registry_listener registryListener{Impl::global, Impl::removed};
    wl_registry_add_listener(d.registry, &registryListener, &d);
    if (wl_display_roundtrip_queue(d.display, d.queue) < 0 ||
        wl_display_roundtrip_queue(d.display, d.queue) < 0) return;
    d.ensureObjects();
    (void)radius;
#else
    (void)display; (void)surface; (void)radius; (void)compositorFrame;
#endif
}
WaylandMaterial::~WaylandMaterial() {
#ifdef MAYONNAISE_WAYLAND_MATERIAL
    auto& d = *m_impl;
    if (d.shape) kos_surface_shape_v1_destroy(d.shape);
    if (d.effect) ext_background_effect_surface_v1_destroy(d.effect);
    if (d.shapeManager) kos_surface_shape_manager_v1_destroy(d.shapeManager);
    if (d.manager) ext_background_effect_manager_v1_destroy(d.manager);
    if (d.compositor) wl_compositor_destroy(d.compositor);
    if (d.registry) wl_registry_destroy(d.registry);
    if (d.queue) wl_event_queue_destroy(d.queue);
    if (d.display) wl_display_flush(d.display);
#endif
}
bool WaylandMaterial::available() const {
#ifdef MAYONNAISE_WAYLAND_MATERIAL
    return !m_impl->failed && m_impl->effect && m_impl->manager && m_impl->compositor &&
        (m_impl->capabilities & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
#else
    return false;
#endif
}
void WaylandMaterial::update(int width, int height, float radius) {
#ifdef MAYONNAISE_WAYLAND_MATERIAL
    auto& d = *m_impl;
    if (!d.display || !d.queue) return;
    if (wl_display_dispatch_queue_pending(d.display, d.queue) < 0) d.failed = true;
    d.ensureObjects();
    if (!available() || width <= 0 || height <= 0) return;
    if (width == d.width && height == d.height && radius == d.radius) return;
    d.width = width; d.height = height; d.radius = radius;
    // A scanline region follows the same circular rounded rectangle as the renderer.
    auto* region = wl_compositor_create_region(d.compositor);
    // A compositor-owned frame clips its own blur to the final window contour.
    // Publishing another client circle here would leave sharp/unpainted corner crescents.
    const double r = d.compositorFrame ? 0.0 :
        std::clamp(static_cast<double>(radius), 0.0, std::min(width, height) * 0.5);
    for (int y = 0; y < height; ++y) {
        const double dy = std::max({r - (y + 0.5), y + 0.5 - (height - r), 0.0});
        const int inset = static_cast<int>(std::ceil(r - std::sqrt(std::max(0.0, r * r - dy * dy))));
        if (width > inset * 2) wl_region_add(region, inset, y, width - inset * 2, 1);
    }
    ext_background_effect_surface_v1_set_blur_region(d.effect, region);
    wl_region_destroy(region);
    if (d.shape) {
        kos_surface_shape_v1_set_geometry(d.shape, 0, 0, width, height);
        kos_surface_shape_v1_set_corner(d.shape, wl_fixed_from_double(r), wl_fixed_from_int(2));
    }
    // State is double-buffered and becomes effective with SDL's next surface commit.
    wl_display_flush(d.display);
#else
    (void)width; (void)height; (void)radius;
#endif
}
}
