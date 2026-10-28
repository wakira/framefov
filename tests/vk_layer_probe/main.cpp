// Loader-protocol probe for the Vulkan layer.
//
// Regression test for the bug that made Proton games crash with the layer merely
// present: when the loader (or winevulkan) asks the layer for "vkCreateInstance"
// with an instance handle the layer has never seen — which it does, before any
// instance exists — the layer must still return a usable vkCreateInstance.
// Returning NULL makes the loader print
//   loader_create_instance_chain: Failed to find 'vkCreateInstance'
// unload the layer and fail instance creation for the whole process.
//
// Usage: vk_layer_probe <path-to-libVkLayer_framefov.so>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "libVkLayer_framefov.so";
  printf("layer: %s\n", path);

  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    printf("FAIL: dlopen: %s\n", dlerror());
    return 1;
  }
  auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
      dlsym(handle, "vkNegotiateLoaderLayerInterfaceVersion"));
  if (!negotiate) {
    printf("FAIL: vkNegotiateLoaderLayerInterfaceVersion not exported\n");
    return 1;
  }

  VkNegotiateLayerInterface info{};
  info.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
  info.loaderLayerInterfaceVersion = 2;
  check(negotiate(&info) == VK_SUCCESS, "negotiation succeeds");
  check(info.pfnGetInstanceProcAddr != nullptr, "layer provides pfnGetInstanceProcAddr");
  check(info.pfnGetDeviceProcAddr != nullptr, "layer provides pfnGetDeviceProcAddr");
  check(info.pfnGetPhysicalDeviceProcAddr == nullptr,
        "layer does not advertise a physical-device proc addr (breaks other layers)");

  PFN_vkGetInstanceProcAddr gipa = info.pfnGetInstanceProcAddr;
  check(gipa(VK_NULL_HANDLE, "vkGetInstanceProcAddr") != nullptr,
        "GIPA returns itself for xrGetInstanceProcAddr-style lookups");

  // The loader requires every layer to provide vkCreateInstance for the chain.
  check(gipa(VK_NULL_HANDLE, "vkCreateInstance") != nullptr,
        "GIPA returns vkCreateInstance for a NULL instance");
  // ... and loaders/winevulkan ask for it with an instance handle of their own.
  // Model that with a mapped object whose dispatch pointer we do not know (a
  // real foreign handle is always mapped; only the dispatch value is unknown).
  void* foreignStorage = std::calloc(1, 64);
  auto foreignInstance = reinterpret_cast<VkInstance>(foreignStorage);
  check(gipa(foreignInstance, "vkCreateInstance") != nullptr,
        "GIPA returns vkCreateInstance for an unknown instance handle");
  // Unknown handles must not crash for any name.
  (void)gipa(foreignInstance, "vkDestroyInstance");
  (void)gipa(foreignInstance, "vkEnumeratePhysicalDevices");
  (void)gipa(foreignInstance, "vkSomethingUnknown");
  check(true, "unknown instance handles do not crash the layer");

  PFN_vkGetDeviceProcAddr gdpa = info.pfnGetDeviceProcAddr;
  void* foreignDeviceStorage = std::calloc(1, 64);
  auto foreignDevice = reinterpret_cast<VkDevice>(foreignDeviceStorage);
  (void)gdpa(foreignDevice, "vkDestroyDevice");
  (void)gdpa(foreignDevice, "vkCmdBeginRendering");
  check(true, "unknown device handles do not crash the layer");
  std::free(foreignStorage);
  std::free(foreignDeviceStorage);

  printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
