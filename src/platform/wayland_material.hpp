#pragma once
#include <memory>

namespace mayo::platform {
/// Optional per-surface Wayland material. Never dispatches SDL's event queue.
class WaylandMaterial {
public:
    WaylandMaterial(void* display, void* surface, float radius, bool compositorFrame = false);
    ~WaylandMaterial();
    bool available() const;
    void update(int width, int height, float radius);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
