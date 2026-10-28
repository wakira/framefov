// Vulkan layer negotiation, dispatch routing and instance lifecycle.
#include "common/export.h"
#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"

#include <vulkan/vk_layer.h>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

using namespace framefov;

namespace {

// End-of-process summary: proves from one run (at INFO) how many passes were
// actually foveated, without needing FRAMEFOV_LOG_LEVEL=debug.
void logStatsAtExit() {
  const Stats& s = SharedState::get().stats;
  LOG_INFO("stats: passes=%llu foveated=%llu neutral=%llu",
           (unsigned long long)s.passes.load(std::memory_order_relaxed),
           (unsigned long long)s.foveatedPasses.load(std::memory_order_relaxed),
           (unsigned long long)s.neutralPasses.load(std::memory_order_relaxed));
}

std::once_flag g_statsAtExitOnce;

}  // namespace

extern "C" {

FRAMEFOV_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
  if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
    pVersionStruct->loaderLayerInterfaceVersion = 2;
  }
  pVersionStruct->pfnGetInstanceProcAddr = framefov::framefov_vkGetInstanceProcAddr;
  pVersionStruct->pfnGetDeviceProcAddr = framefov::framefov_vkGetDeviceProcAddr;
  // Never advertise a physical-device proc addr: interposing physical device
  // functions breaks other layers (VK_LAYER_MESA_device_select).
  pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

  log::init();
  std::call_once(g_statsAtExitOnce, [] { std::atexit(logStatsAtExit); });
  LOG_INFO("Vulkan layer negotiated (interface version %u); SharedState @ %p",
           pVersionStruct->loaderLayerInterfaceVersion,
           static_cast<const void*>(&SharedState::get()));
  return VK_SUCCESS;
}

}  // extern "C"

namespace framefov {

// ---------------------------------------------------------------------------
// Hook tables
// ---------------------------------------------------------------------------
namespace {

// Last next-layer entry points we resolved, used to forward calls that arrive
// with a handle we do not own (a different loader's instance, or a caller
// outside the chain). Without this such calls look like "function missing".
PFN_vkGetInstanceProcAddr g_lastNextGipa = nullptr;

const NamedHook kInstanceHooks[] = {
    {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateInstance)},
    {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyInstance)},
    {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateDevice)},
};

const NamedHook kDeviceHooks[] = {
    {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyDevice)},
    {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateImageView)},
    {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyImageView)},
    {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateRenderPass)},
    {"vkCreateRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateRenderPass2)},
    {"vkCreateRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateRenderPass2KHR)},
    {"vkDestroyRenderPass", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyRenderPass)},
    {"vkCreateFramebuffer", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateFramebuffer)},
    {"vkDestroyFramebuffer", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyFramebuffer)},
    {"vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateShaderModule)},
    {"vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkDestroyShaderModule)},
    {"vkCreateGraphicsPipelines",
     reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateGraphicsPipelines)},
    {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkBeginCommandBuffer)},
    {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkFreeCommandBuffers)},
    {"vkCmdBeginRendering", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCmdBeginRendering)},
    {"vkCmdBeginRenderingKHR", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCmdBeginRenderingKHR)},
    {"vkCmdBeginRenderPass", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCmdBeginRenderPass)},
    {"vkCmdBeginRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCmdBeginRenderPass2)},
    {"vkCmdBeginRenderPass2KHR",
     reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCmdBeginRenderPass2KHR)},
};

PFN_vkVoidFunction lookup(const NamedHook* table, size_t count, const char* name) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(table[i].name, name) == 0) return table[i].fn;
  }
  return nullptr;
}

constexpr size_t kInstanceHookCount = sizeof(kInstanceHooks) / sizeof(kInstanceHooks[0]);
constexpr size_t kDeviceHookCount = sizeof(kDeviceHooks) / sizeof(kDeviceHooks[0]);

const VkLayerInstanceCreateInfo* findInstanceLinkInfo(const void* pNext) {
  auto info = reinterpret_cast<const VkLayerInstanceCreateInfo*>(pNext);
  while (info) {
    if (info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
        info->function == VK_LAYER_LINK_INFO) {
      return info;
    }
    info = reinterpret_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
  }
  return nullptr;
}

template <class T>
T resolveFn(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
  return gipa ? reinterpret_cast<T>(gipa(instance, name)) : nullptr;
}

// Core name first, then its KHR alias (for instances below 1.1).
template <class T>
T resolveFn2(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* core,
             const char* alias) {
  T fn = resolveFn<T>(gipa, instance, core);
  if (!fn && alias) fn = resolveFn<T>(gipa, instance, alias);
  return fn;
}

// Pick the spelling that is legal for the instance's API version: with a 1.0
// instance the *2 core names must not be called (validation flags it), even
// though the loader resolves them.
template <class T>
T resolveVersioned(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, uint32_t apiVersion,
                   const char* core, const char* alias) {
  T fn = nullptr;
  if (apiVersion >= VK_API_VERSION_1_1) {
    fn = resolveFn<T>(gipa, instance, core);
    if (!fn) fn = resolveFn<T>(gipa, instance, alias);
  } else {
    fn = resolveFn<T>(gipa, instance, alias);
    if (!fn) fn = resolveFn<T>(gipa, instance, core);
  }
  return fn;
}

bool hasExtension(const VkInstanceCreateInfo* ci, const char* want) {
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
    if (ci->ppEnabledExtensionNames && ci->ppEnabledExtensionNames[i] &&
        strcmp(ci->ppEnabledExtensionNames[i], want) == 0) {
      return true;
    }
  }
  return false;
}

bool instanceExtensionAvailable(PFN_vkGetInstanceProcAddr gipa, const char* want) {
  auto enumerate = resolveFn<PFN_vkEnumerateInstanceExtensionProperties>(
      gipa, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  if (!enumerate) return false;
  uint32_t count = 0;
  if (enumerate(nullptr, &count, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> props(count);
  if (count && enumerate(nullptr, &count, props.data()) != VK_SUCCESS) return false;
  for (const auto& p : props) {
    if (strcmp(p.extensionName, want) == 0) return true;
  }
  return false;
}

void resolveInstanceDispatch(InstanceData& data) {
  const VkInstance inst = data.instance;
  PFN_vkGetInstanceProcAddr gipa = data.nextGipa;
  InstanceDispatch& t = data.d;
  t.GetInstanceProcAddr = gipa;
  t.DestroyInstance = resolveFn<PFN_vkDestroyInstance>(gipa, inst, "vkDestroyInstance");
  t.EnumerateDeviceExtensionProperties =
      resolveFn<PFN_vkEnumerateDeviceExtensionProperties>(gipa, inst,
                                                          "vkEnumerateDeviceExtensionProperties");
  t.GetPhysicalDeviceProperties =
      resolveFn<PFN_vkGetPhysicalDeviceProperties>(gipa, inst, "vkGetPhysicalDeviceProperties");
  t.GetPhysicalDeviceProperties2 = resolveVersioned<PFN_vkGetPhysicalDeviceProperties2>(
      gipa, inst, data.apiVersion, "vkGetPhysicalDeviceProperties2",
      "vkGetPhysicalDeviceProperties2KHR");
  t.GetPhysicalDeviceFeatures2 = resolveVersioned<PFN_vkGetPhysicalDeviceFeatures2>(
      gipa, inst, data.apiVersion, "vkGetPhysicalDeviceFeatures2",
      "vkGetPhysicalDeviceFeatures2KHR");
  t.GetPhysicalDeviceMemoryProperties = resolveFn<PFN_vkGetPhysicalDeviceMemoryProperties>(
      gipa, inst, "vkGetPhysicalDeviceMemoryProperties");
  t.GetPhysicalDeviceFormatProperties = resolveFn<PFN_vkGetPhysicalDeviceFormatProperties>(
      gipa, inst, "vkGetPhysicalDeviceFormatProperties");
  t.GetPhysicalDeviceFragmentShadingRatesKHR =
      resolveFn<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
          gipa, inst, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
}

}  // namespace

PFN_vkVoidFunction hookedInstanceFn(const char* name) {
  PFN_vkVoidFunction fn = lookup(kInstanceHooks, kInstanceHookCount, name);
  if (!fn) fn = lookup(kDeviceHooks, kDeviceHookCount, name);
  return fn;
}

PFN_vkVoidFunction hookedDeviceFn(const char* name) {
  return lookup(kDeviceHooks, kDeviceHookCount, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framefov_vkGetInstanceProcAddr(VkInstance instance,
                                                                    const char* name) {
  if (!name) return nullptr;
  if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(framefov_vkGetInstanceProcAddr);
  }
  // Every layer in the chain must provide vkCreateInstance, and it must answer
  // for *any* instance argument: loaders and winevulkan ask for it with an
  // instance handle of their own, before we have seen an instance at all.
  // Returning NULL here makes the loader print
  //   loader_create_instance_chain: Failed to find 'vkCreateInstance'
  // unload the layer and fail instance creation for the whole process (this is
  // what made Proton games crash when the layer was merely present).
  if (strcmp(name, "vkCreateInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(framefov_vkCreateInstance);
  }
  if (instance == VK_NULL_HANDLE) return nullptr;

  InstanceData* data = findInstance(instance);
  if (!data) {
    // A handle we do not own (another loader's instance, or a caller outside the
    // chain): forward through the remembered next GIPA instead of pretending the
    // function does not exist.
    return g_lastNextGipa ? g_lastNextGipa(instance, name) : nullptr;
  }
  PFN_vkVoidFunction next =
      data->d.GetInstanceProcAddr ? data->d.GetInstanceProcAddr(instance, name) : nullptr;
  PFN_vkVoidFunction hook = hookedInstanceFn(name);
  if (!hook) return next;
  // Only report a hook when the next layer can provide the function (i.e. the
  // app enabled the required extension / the needed core version).
  return next ? hook : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framefov_vkGetDeviceProcAddr(VkDevice device,
                                                                  const char* name) {
  if (!name) return nullptr;
  if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(framefov_vkGetDeviceProcAddr);
  }
  DeviceData* data = findDevice(device);
  if (!data) {
    // Unknown device handle: forward rather than report the function as missing.
    return g_lastNextGdpa ? g_lastNextGdpa(device, name) : nullptr;
  }
  PFN_vkVoidFunction next =
      data->d.GetDeviceProcAddr ? data->d.GetDeviceProcAddr(device, name) : nullptr;
  if (!next) return nullptr;
  if (!data->active) return next;
  PFN_vkVoidFunction hook = hookedDeviceFn(name);
  return hook ? hook : next;
}

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkInstance* pInstance) {
  FRAMEFOV_GUARD_BEGIN
  const VkLayerInstanceCreateInfo* link = findInstanceLinkInfo(pCreateInfo->pNext);
  if (!link || !link->u.pLayerInfo) {
    LOG_ERROR("vkCreateInstance: no loader link info in the pNext chain");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerInstanceCreateInfo* mutableLink = const_cast<VkLayerInstanceCreateInfo*>(link);
  PFN_vkGetInstanceProcAddr nextGipa = mutableLink->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  mutableLink->u.pLayerInfo = mutableLink->u.pLayerInfo->pNext;
  if (!nextGipa) return VK_ERROR_INITIALIZATION_FAILED;

  uint32_t apiVersion = VK_API_VERSION_1_0;
  if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion != 0) {
    apiVersion = pCreateInfo->pApplicationInfo->apiVersion;
  }

  // Below 1.1 we need VK_KHR_get_physical_device_properties2 for capability
  // queries, so ask for it if the app did not. Storage must outlive the call.
  VkInstanceCreateInfo createInfo = *pCreateInfo;
  std::vector<const char*> extensions;
  if (apiVersion < VK_API_VERSION_1_1 &&
      !hasExtension(pCreateInfo, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) &&
      instanceExtensionAvailable(nextGipa, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
    extensions.assign(pCreateInfo->ppEnabledExtensionNames,
                      pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  }

  auto nextCreate =
      resolveFn<PFN_vkCreateInstance>(nextGipa, VK_NULL_HANDLE, "vkCreateInstance");
  if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = nextCreate(&createInfo, pAllocator, pInstance);
  if (result != VK_SUCCESS) return result;

  auto data = std::make_unique<InstanceData>();
  data->instance = *pInstance;
  data->apiVersion = apiVersion;
  data->nextGipa = nextGipa;
  resolveInstanceDispatch(*data);
  g_lastNextGipa = nextGipa;

  addInstance(std::move(data));
  LOG_INFO("vkCreateInstance: apiVersion %u.%u, instance %p",
           VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion),
           reinterpret_cast<void*>(*pInstance));
  // What else the launch asked to chain: tells you whether Valve's FDM layer is
  // in this process (wrapper STEAM_LAUNCH_WRAPPER_FDM) or explicitly disabled.
  {
    const char* inst = getenv("VK_INSTANCE_LAYERS");
    const char* dis = getenv("VK_LOADER_LAYERS_DISABLE");
    LOG_INFO("chain env: VK_INSTANCE_LAYERS='%s' VK_LOADER_LAYERS_DISABLE='%s'",
             inst ? inst : "", dis ? dis : "");
  }
  return VK_SUCCESS;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyInstance(VkInstance instance,
                                                   const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  std::unique_ptr<InstanceData> data = removeInstance(instance);
  if (data && data->d.DestroyInstance) {
    data->d.DestroyInstance(instance, pAllocator);
  } else {
    LOG_WARN("vkDestroyInstance: no instance data for %p", reinterpret_cast<void*>(instance));
  }
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace framefov
