#include "vulkan/vk_dispatch.h"

#include <mutex>

namespace framefov {

namespace {

std::mutex g_mapLock;
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;

}  // namespace

PFN_vkGetDeviceProcAddr g_lastNextGdpa = nullptr;

InstanceData* findInstance(VkInstance instance) { return findInstanceByKey(dispatchKey(instance)); }

InstanceData* findInstanceByKey(void* key) {
  if (!key) return nullptr;
  std::lock_guard<std::mutex> lock(g_mapLock);
  auto it = g_instances.find(key);
  return it == g_instances.end() ? nullptr : it->second.get();
}

void addInstance(std::unique_ptr<InstanceData> data) {
  void* key = dispatchKey(data->instance);
  if (!key) return;
  std::lock_guard<std::mutex> lock(g_mapLock);
  g_instances[key] = std::move(data);
}

std::unique_ptr<InstanceData> removeInstance(VkInstance instance) {
  void* key = dispatchKey(instance);
  if (!key) return nullptr;
  std::lock_guard<std::mutex> lock(g_mapLock);
  auto it = g_instances.find(key);
  if (it == g_instances.end()) return nullptr;
  auto out = std::move(it->second);
  g_instances.erase(it);
  return out;
}

DeviceData* findDevice(VkDevice device) { return findDeviceByKey(dispatchKey(device)); }

DeviceData* findDeviceByKey(void* key) {
  if (!key) return nullptr;
  std::lock_guard<std::mutex> lock(g_mapLock);
  auto it = g_devices.find(key);
  return it == g_devices.end() ? nullptr : it->second.get();
}

void addDevice(std::unique_ptr<DeviceData> data) {
  void* key = dispatchKey(data->device);
  if (!key) return;
  std::lock_guard<std::mutex> lock(g_mapLock);
  g_devices[key] = std::move(data);
}

std::unique_ptr<DeviceData> removeDevice(VkDevice device) {
  void* key = dispatchKey(device);
  if (!key) return nullptr;
  std::lock_guard<std::mutex> lock(g_mapLock);
  auto it = g_devices.find(key);
  if (it == g_devices.end()) return nullptr;
  auto out = std::move(it->second);
  g_devices.erase(it);
  return out;
}

}  // namespace framefov
