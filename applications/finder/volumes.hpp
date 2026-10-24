#pragma once

#include <mayonnaise/app.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace finder {

struct DeviceVolume {
    std::string id, drive, siblingId, device, name, mountPoint, filesystem;
    std::uint64_t size = 0;
    bool external = false, system = false, ignored = false, mountable = false;
    bool canPowerOff = false, ejectable = false, readOnly = false;
    [[nodiscard]] bool visible() const { return mountable && !system && !ignored && (external || !mountPoint.empty()); }
    [[nodiscard]] bool canRemove() const { return visible() && external && !drive.empty() && drive != "/"; }
    bool operator==(const DeviceVolume&) const = default;
};

bool sameDevice(const DeviceVolume& a, const DeviceVolume& b);

struct VolumeCall {
    std::string object, interface, method;
    bool operator==(const VolumeCall&) const = default;
};
// Includes hidden partitions in the plan; refuses to remove a drive containing a system volume.
std::vector<VolumeCall> removalPlan(const std::vector<DeviceVolume>& volumes, const std::string& id,
                                    std::string& error);
std::string volumeError(const std::string& dbusName, const std::string& detail);

class VolumeManager {
public:
    enum class Action { Mount, Remove };
    struct Result {
        std::string error, mountPoint;
        bool poweredOff = false;
    };
    using Changed = std::function<void(std::vector<DeviceVolume>, std::string serviceError)>;
    VolumeManager(mayo::App& app, Changed changed);
    ~VolumeManager();
    VolumeManager(const VolumeManager&) = delete;
    VolumeManager& operator=(const VolumeManager&) = delete;
    // One operation at a time; all callbacks run on the UI thread, D-Bus runs on the worker.
    void perform(std::string id, Action action, std::function<void(Result)> done);
    [[nodiscard]] bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace finder
