#pragma once
#include <gio/gio.h>
#include "volumes.hpp"

namespace finder {
// ObjectManager's a{oa{sa{sv}}} payload. Kept separate for fixture-based regression tests.
std::vector<DeviceVolume> parseVolumeObjects(GVariant* objects);
std::vector<DeviceVolume> readVolumeObjects(GDBusConnection* bus, GCancellable* cancel, std::string& error);
VolumeManager::Result performVolumeAction(GDBusConnection* bus, GCancellable* cancel, const std::string& id,
                                          VolumeManager::Action action);
}  // namespace finder
