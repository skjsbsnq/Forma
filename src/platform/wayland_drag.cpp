#include "wayland_drag.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#endif

namespace mayo::platform::wayland {

#if !defined(__linux__)

bool startFileDrag(void*, void*, const std::vector<std::string>&, std::function<void()>, std::function<void(void*, float, float, const std::vector<std::string>&)>) { return false; }
void dispatch() {}
bool dragActive() { return false; }
void prepare(void*) {}
void shutdown() {}
int scrollSource() { return -1; }
bool scrollGestureActive() { return false; }
bool takeScrollStop() { return false; }

#else

namespace {

// Only opaque pointers are needed; the protocol is driven through wl_proxy_marshal_flags with the opcodes and
// event orders of wayland.xml (core protocol, stable since 2012).
struct wl_display;
struct wl_proxy;
struct wl_event_queue;
struct wl_interface;

using MarshalFlags = wl_proxy* (*)(wl_proxy*, std::uint32_t, const wl_interface*, std::uint32_t, std::uint32_t, ...);

struct Lib {
    bool ok = false;
    wl_event_queue* (*createQueue)(wl_display*) = nullptr;
    int (*dispatchQueuePending)(wl_display*, wl_event_queue*) = nullptr;
    int (*roundtripQueue)(wl_display*, wl_event_queue*) = nullptr;
    int (*flush)(wl_display*) = nullptr;
    MarshalFlags marshal = nullptr;
    int (*addListener)(wl_proxy*, void (**)(void), void*) = nullptr;
    void (*setQueue)(wl_proxy*, wl_event_queue*) = nullptr;
    std::uint32_t (*getVersion)(wl_proxy*) = nullptr;
    void* (*createWrapper)(void*) = nullptr;
    void (*wrapperDestroy)(void*) = nullptr;
    void (*proxyDestroy)(wl_proxy*) = nullptr;
    void (*queueDestroy)(wl_event_queue*) = nullptr;
    const wl_interface* registryIface = nullptr;
    const wl_interface* seatIface = nullptr;
    const wl_interface* pointerIface = nullptr;
    const wl_interface* managerIface = nullptr;
    const wl_interface* deviceIface = nullptr;
    const wl_interface* sourceIface = nullptr;
    const wl_interface* offerIface = nullptr;
};

Lib& lib() {
    static Lib l = [] {
        Lib r;
        void* h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!h) return r;
        auto sym = [&](auto& out, const char* name) {
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(dlsym(h, name));
            return out != nullptr;
        };
        r.ok = sym(r.createQueue, "wl_display_create_queue") && sym(r.dispatchQueuePending, "wl_display_dispatch_queue_pending") &&
               sym(r.roundtripQueue, "wl_display_roundtrip_queue") && sym(r.flush, "wl_display_flush") &&
               sym(r.marshal, "wl_proxy_marshal_flags") && sym(r.addListener, "wl_proxy_add_listener") &&
               sym(r.setQueue, "wl_proxy_set_queue") && sym(r.getVersion, "wl_proxy_get_version") &&
               sym(r.createWrapper, "wl_proxy_create_wrapper") && sym(r.wrapperDestroy, "wl_proxy_wrapper_destroy") &&
               sym(r.proxyDestroy, "wl_proxy_destroy") && sym(r.queueDestroy, "wl_event_queue_destroy") &&
               sym(r.registryIface, "wl_registry_interface") && sym(r.seatIface, "wl_seat_interface") &&
               sym(r.pointerIface, "wl_pointer_interface") && sym(r.managerIface, "wl_data_device_manager_interface") &&
               sym(r.deviceIface, "wl_data_device_interface") && sym(r.sourceIface, "wl_data_source_interface") &&
               sym(r.offerIface, "wl_data_offer_interface");
        return r;
    }();
    return l;
}

// Opcodes (requests) from wayland.xml.
constexpr std::uint32_t kDisplayGetRegistry = 1;
constexpr std::uint32_t kRegistryBind = 0;
constexpr std::uint32_t kSeatGetPointer = 0;
constexpr std::uint32_t kManagerCreateSource = 0;
constexpr std::uint32_t kManagerGetDevice = 1;
constexpr std::uint32_t kSourceOffer = 0;
constexpr std::uint32_t kSourceDestroy = 1;
constexpr std::uint32_t kSourceSetActions = 2;
constexpr std::uint32_t kDeviceStartDrag = 0;
constexpr std::uint32_t kDeviceRelease = 2;  // since wl_data_device v2
constexpr std::uint32_t kOfferDestroy = 2;
constexpr std::uint32_t kMarshalDestroy = 1;  // WL_MARSHAL_FLAG_DESTROY
constexpr std::uint32_t kActionCopy = 1;
constexpr std::uint32_t kActionMove = 2;

struct State {
    wl_display* display = nullptr;
    wl_event_queue* queue = nullptr;
    wl_proxy* registry = nullptr;
    wl_proxy* seat = nullptr;
    wl_proxy* pointer = nullptr;
    wl_proxy* manager = nullptr;
    wl_proxy* device = nullptr;
    std::uint32_t pressSerial = 0;  // last button press on our surfaces
    int axisSource = -1;            // of the latest scroll frame: 0 wheel, 1 finger, 2 continuous, 3 tilt
    bool axisStopInFrame = false;
    bool axisInFrame = false;
    bool gestureOpen = false;       // touchpad scrolling seen, fingers not lifted yet
    bool scrollStopped = false;     // fingers lifted; not yet taken by the event loop
    bool failed = false;

    wl_proxy* source = nullptr;
    wl_proxy* dropOffer = nullptr;
    void* dropSurface = nullptr;
    float dropX = 0, dropY = 0;
    std::vector<std::string> paths;
    std::function<void(void*, float, float, const std::vector<std::string>&)> localDrop;
    std::string uriList;
    std::string plain;
    std::function<void()> ended;
};

State g;

/// A request whose only argument is the new object's id.
wl_proxy* create(wl_proxy* p, std::uint32_t opcode, const wl_interface* iface) {
    return lib().marshal(p, opcode, iface, lib().getVersion(p), 0, nullptr);
}

std::string percentEncode(const std::string& path) {
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-_.~", c)) {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

void releaseDevice() {
    // A v1 device can't be released; keep it then (only very old compositors).
    if (!g.device || lib().getVersion(g.device) < 2) return;
    lib().marshal(g.device, kDeviceRelease, nullptr, lib().getVersion(g.device), kMarshalDestroy);
    g.device = nullptr;
}

void clearDropOffer();

void finishDrag(bool destroySource) {
    if (g.ended) {
        auto fn = std::move(g.ended);
        g.ended = nullptr;
        fn();
    }
    if (destroySource && g.source) {
        clearDropOffer();
        lib().marshal(g.source, kSourceDestroy, nullptr, lib().getVersion(g.source), kMarshalDestroy);
        g.source = nullptr;
        releaseDevice();
        lib().flush(g.display);
    }
}

// Listeners ------------------------------------------------------------------------------------------------

void noop() {}

extern void (*kSeatListener[])(void);

void registryGlobal(void* /*data*/, wl_proxy* registry, std::uint32_t name, const char* iface, std::uint32_t version) {
    auto bind = [&](const wl_interface* interface, const char* ifaceName, std::uint32_t want) {
        const std::uint32_t v = version < want ? version : want;
        return lib().marshal(registry, kRegistryBind, interface, v, 0, name, ifaceName, v, nullptr);
    };
    // v5 pointers report the scroll source and when the fingers lift (axis_source / axis_stop).
    if (!g.seat && std::strcmp(iface, "wl_seat") == 0) {
        g.seat = bind(lib().seatIface, "wl_seat", 5);
        // Bind replies may arrive within this roundtrip; install before dispatching them.
        lib().addListener(g.seat, kSeatListener, nullptr);
    }
    if (!g.manager && std::strcmp(iface, "wl_data_device_manager") == 0)
        g.manager = bind(lib().managerIface, "wl_data_device_manager", 3);
}
void registryRemove(void*, wl_proxy*, std::uint32_t) {}
void (*kRegistryListener[])(void) = {reinterpret_cast<void (*)()>(registryGlobal), reinterpret_cast<void (*)()>(registryRemove)};

void pointerEnter(void*, wl_proxy*, std::uint32_t, wl_proxy*, std::int32_t, std::int32_t) {}
void pointerLeave(void*, wl_proxy*, std::uint32_t, wl_proxy*) {}
void pointerMotion(void*, wl_proxy*, std::uint32_t, std::int32_t, std::int32_t) {}
void pointerButton(void*, wl_proxy*, std::uint32_t serial, std::uint32_t, std::uint32_t, std::uint32_t state) {
    if (state == 1) g.pressSerial = serial;  // WL_POINTER_BUTTON_STATE_PRESSED
}
void pointerAxis(void*, wl_proxy*, std::uint32_t, std::uint32_t, std::int32_t) { g.axisInFrame = true; }
void pointerFrame(void*, wl_proxy*) {
    if (g.axisInFrame && (g.axisSource == 1 || g.axisSource == 2)) g.gestureOpen = true;
    if (g.axisStopInFrame) {
        g.scrollStopped = g.gestureOpen;
        g.gestureOpen = false;
    }
    g.axisInFrame = false;
    g.axisStopInFrame = false;
}
void pointerAxisSource(void*, wl_proxy*, std::uint32_t source) { g.axisSource = static_cast<int>(source); }
void pointerAxisStop(void*, wl_proxy*, std::uint32_t, std::uint32_t) { g.axisStopInFrame = true; }
void pointerAxisDiscrete(void*, wl_proxy*, std::uint32_t, std::int32_t) {}

void (*kPointerListener[])(void) = {
    reinterpret_cast<void (*)()>(pointerEnter),      reinterpret_cast<void (*)()>(pointerLeave),
    reinterpret_cast<void (*)()>(pointerMotion),     reinterpret_cast<void (*)()>(pointerButton),
    reinterpret_cast<void (*)()>(pointerAxis),       reinterpret_cast<void (*)()>(pointerFrame),
    reinterpret_cast<void (*)()>(pointerAxisSource), reinterpret_cast<void (*)()>(pointerAxisStop),
    reinterpret_cast<void (*)()>(pointerAxisDiscrete),
    // v8+ events (value120, relative direction) are never sent to a v5 pointer; slots for safety.
    reinterpret_cast<void (*)()>(noop),              reinterpret_cast<void (*)()>(noop),
};

void seatCapabilities(void*, wl_proxy* seat, std::uint32_t caps) {
    if ((caps & 1) && !g.pointer) {  // WL_SEAT_CAPABILITY_POINTER
        g.pointer = create(seat, kSeatGetPointer, lib().pointerIface);
        lib().addListener(g.pointer, kPointerListener, nullptr);
    }
}
void seatName(void*, wl_proxy*, const char*) {}
void (*kSeatListener[])(void) = {reinterpret_cast<void (*)()>(seatCapabilities), reinterpret_cast<void (*)()>(seatName)};

void offerMime(void*, wl_proxy*, const char*) {}
void offerActions(void*, wl_proxy*, std::uint32_t) {}
void (*kOfferListener[])(void) = {reinterpret_cast<void (*)()>(offerMime),
    reinterpret_cast<void (*)()>(offerActions), reinterpret_cast<void (*)()>(offerActions)};
void clearDropOffer() {
    if (g.dropOffer) lib().marshal(g.dropOffer, kOfferDestroy, nullptr, lib().getVersion(g.dropOffer), kMarshalDestroy);
    g.dropOffer = nullptr;
    g.dropSurface = nullptr;
}
void deviceDataOffer(void*, wl_proxy*, wl_proxy* offer) {
    // This device exists only during our outgoing drag. Keep its self-drop offer alive.
    clearDropOffer();
    g.dropOffer = offer;
    if (offer) lib().addListener(offer, kOfferListener, nullptr);
}
void deviceEnter(void*, wl_proxy*, std::uint32_t serial, wl_proxy* surface,
                 std::int32_t x, std::int32_t y, wl_proxy* offer) {
    if (!offer || offer != g.dropOffer || !g.source) return;
    g.dropSurface = surface;
    g.dropX = x / 256.0f; g.dropY = y / 256.0f;
    lib().marshal(offer, 0, nullptr, lib().getVersion(offer), 0, serial, "text/uri-list");
    if (lib().getVersion(offer) >= 3)
        lib().marshal(offer, 4, nullptr, lib().getVersion(offer), 0, kActionCopy, kActionCopy);
}
void deviceLeave(void*, wl_proxy*) { clearDropOffer(); }
void deviceMotion(void*, wl_proxy*, std::uint32_t, std::int32_t x, std::int32_t y) {
    g.dropX = x / 256.0f; g.dropY = y / 256.0f;
}
void deviceDrop(void*, wl_proxy*) {
    if (!g.dropSurface || !g.dropOffer) return;
    if (g.localDrop) g.localDrop(g.dropSurface, g.dropX, g.dropY, g.paths);
    if (lib().getVersion(g.dropOffer) >= 3)
        lib().marshal(g.dropOffer, 3, nullptr, lib().getVersion(g.dropOffer), 0);
    clearDropOffer();
}
void deviceSelection(void*, wl_proxy*, wl_proxy*) {}
void (*kDeviceListener[])(void) = {
    reinterpret_cast<void (*)()>(deviceDataOffer), reinterpret_cast<void (*)()>(deviceEnter),
    reinterpret_cast<void (*)()>(deviceLeave),     reinterpret_cast<void (*)()>(deviceMotion),
    reinterpret_cast<void (*)()>(deviceDrop),      reinterpret_cast<void (*)()>(deviceSelection),
};

void sourceTarget(void*, wl_proxy*, const char*) {}
void sourceSend(void*, wl_proxy*, const char* mime, std::int32_t fd) {
    const std::string& data = std::strcmp(mime, "text/uri-list") == 0 ? g.uriList : g.plain;
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    ::close(fd);
}
void sourceCancelled(void*, wl_proxy*) { finishDrag(true); }
void sourceDropPerformed(void*, wl_proxy*) {}  // data may still be requested
void sourceFinished(void*, wl_proxy*) { finishDrag(true); }
void sourceAction(void*, wl_proxy*, std::uint32_t) {}
void (*kSourceListener[])(void) = {
    reinterpret_cast<void (*)()>(sourceTarget),        reinterpret_cast<void (*)()>(sourceSend),
    reinterpret_cast<void (*)()>(sourceCancelled),     reinterpret_cast<void (*)()>(sourceDropPerformed),
    reinterpret_cast<void (*)()>(sourceFinished),      reinterpret_cast<void (*)()>(sourceAction),
};


bool init(void* display) {
    if (g.failed) return false;
    if (g.queue) return g.manager != nullptr;
    if (!display || !lib().ok) {
        g.failed = true;
        return false;
    }
    g.display = static_cast<wl_display*>(display);
    g.queue = lib().createQueue(g.display);
    // The registry must be created on our queue from the start, or its events could reach SDL's queue first.
    auto* wrapper = static_cast<wl_proxy*>(lib().createWrapper(display));
    lib().setQueue(wrapper, g.queue);
    g.registry = lib().marshal(wrapper, kDisplayGetRegistry, lib().registryIface, lib().getVersion(wrapper), 0, nullptr);
    lib().wrapperDestroy(wrapper);
    lib().addListener(g.registry, kRegistryListener, nullptr);
    lib().roundtripQueue(g.display, g.queue);  // globals -> seat, manager
    if (!g.seat || !g.manager) {
        g.failed = true;
        return false;
    }
    lib().roundtripQueue(g.display, g.queue);  // seat capabilities -> pointer
    // No wl_data_device yet: Mutter sends the clipboard selection and drops to only one data device per
    // client, so a second one here would starve SDL's (paste from other apps and drops stopped working).
    // startFileDrag() creates it for the length of a drag.
    lib().flush(g.display);
    if (std::getenv("MAYO_VERBOSE"))
        std::fprintf(stderr, "mayonnaise: Wayland file drags ready (pointer %s)\n", g.pointer ? "yes" : "no");
    return true;
}

}  // namespace

void prepare(void* display) { init(display); }

void shutdown() {
    // The connection is about to close. Destroy local proxies without invoking user callbacks or
    // waiting for protocol replies; none of these pointers may survive into the next display.
    if (lib().ok) {
        for (auto* proxy : {g.source, g.device, g.pointer, g.seat, g.manager, g.registry})
            if (proxy) lib().proxyDestroy(proxy);
        if (g.queue) lib().queueDestroy(g.queue);
    }
    g = State{};
}

bool startFileDrag(void* display, void* surface, const std::vector<std::string>& paths, std::function<void()> ended,
                   std::function<void(void*, float, float, const std::vector<std::string>&)> localDrop) {
    if (paths.empty() || !surface || !init(display) || !g.pointer) return false;
    dispatch();  // pick up the serial of the press that started this drag
    if (g.pressSerial == 0) return false;
    if (g.source) finishDrag(true);  // a previous drag never finished

    g.uriList.clear();
    g.plain.clear();
    for (const auto& p : paths) {
        g.uriList += "file://" + percentEncode(p) + "\r\n";
        g.plain += (g.plain.empty() ? "" : "\n") + p;
    }
    g.paths = paths;
    g.localDrop = std::move(localDrop);
    g.ended = std::move(ended);
    if (!g.device) {
        g.device = lib().marshal(g.manager, kManagerGetDevice, lib().deviceIface, lib().getVersion(g.manager), 0, nullptr, g.seat);
        lib().addListener(g.device, kDeviceListener, nullptr);
    }
    g.source = create(g.manager, kManagerCreateSource, lib().sourceIface);
    lib().addListener(g.source, kSourceListener, nullptr);
    // SDL can prefer a text offer over URI data, then try to parse plain paths as URIs.
    // File drags advertise the interoperable file format only.
    for (const char* mime : {"text/uri-list"})
        lib().marshal(g.source, kSourceOffer, nullptr, lib().getVersion(g.source), 0, mime);
    if (lib().getVersion(g.source) >= 3)
        lib().marshal(g.source, kSourceSetActions, nullptr, lib().getVersion(g.source), 0, kActionCopy);
    lib().marshal(g.device, kDeviceStartDrag, nullptr, lib().getVersion(g.device), 0, g.source, surface, nullptr, g.pressSerial);
    lib().flush(g.display);
    return true;
}

void dispatch() {
    if (!g.queue) return;
    lib().dispatchQueuePending(g.display, g.queue);
    lib().flush(g.display);
}

bool dragActive() { return g.source != nullptr; }

int scrollSource() { return g.axisSource; }

bool scrollGestureActive() { return g.gestureOpen; }

bool takeScrollStop() {
    const bool stopped = g.scrollStopped;
    g.scrollStopped = false;
    return stopped;
}

#endif

}  // namespace mayo::platform::wayland
