// Dispatch tables and per-object data for the Vulkan half.
//
// The loader stores its own dispatch pointer as the first word of every
// dispatchable handle, so `dispatchKey()` maps a handle to the map key used for
// our data. Physical devices share the instance's key; queues and command
// buffers share the device's key.
#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "common/shared_state.h"
#include "common/spirv_scan.h"
#include "vulkan/vk_resources.h"

namespace framefov {

struct Resources;

inline void* dispatchKey(const void* handle) {
  return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}

struct InstanceDispatch {
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;  // core or KHR
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;      // core or KHR
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
  PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR GetPhysicalDeviceFragmentShadingRatesKHR = nullptr;
};

struct DeviceDispatch {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkDestroyDevice DestroyDevice = nullptr;

  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;

  PFN_vkCreateRenderPass CreateRenderPass = nullptr;
  PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
  PFN_vkCreateRenderPass2KHR CreateRenderPass2KHR = nullptr;
  PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
  PFN_vkCreateShaderModule CreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;

  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
  PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2 = nullptr;
  PFN_vkCmdBeginRenderPass2KHR CmdBeginRenderPass2KHR = nullptr;
  PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
  PFN_vkCmdBeginRenderingKHR CmdBeginRenderingKHR = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
};

struct InstanceData {
  VkInstance instance = VK_NULL_HANDLE;
  uint32_t apiVersion = VK_API_VERSION_1_0;
  PFN_vkGetInstanceProcAddr nextGipa = nullptr;
  InstanceDispatch d;
};

struct ImageViewInfo {
  VkImage image = VK_NULL_HANDLE;
  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
  uint32_t baseArrayLayer = 0;
  uint32_t layerCount = 1;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct CmdState {
  bool neutralInitialized = false;
  void* neutralInitImage = nullptr;               // neutral image that was initialized
  VkImageView lastAttachedView = VK_NULL_HANDLE;  // for suspend/resume
};

// Per render pass bookkeeping (05 §1.2).
struct RenderPassInfo {
  bool modified = false;   // we appended the FSR attachment
  bool skipped = false;    // app-supplied VRS/density-map struct: untouched
  uint32_t origAttachmentCount = 0;
  uint32_t fsrIndex = 0;
  uint32_t viewMaskUnion = 0;
  bool hasColor = false;
  bool multiview = false;
};

// Per framebuffer bookkeeping (05 §2).
struct FramebufferInfo {
  RenderPassInfo rp;
  uint32_t width = 0, height = 0, layers = 1;
  bool imageless = false;
  bool neutral = true;                // points at the shared all-zero image
  std::vector<VkImageView> appViews;  // non-imageless only
  RateImage* image = nullptr;         // dedicated rate image, or the shared neutral one
};

// Device capabilities gathered at activation (04 §5).
struct Caps {
  VkExtent2D minTexel{8, 8};
  VkExtent2D maxTexel{8, 8};
  uint32_t maxTexelAspect = 1;
  bool layeredAttachments = false;
  bool nonTrivialCombiners = false;
  // Safety for VRS combined with depth/stencil writes, sample masks, sample
  // shading (05 §6).
  bool withDepthStencilWrites = false;
  bool withShaderSampleMask = false;
  bool withSampleMask = false;
  std::vector<VkExtent2D> supportedRates;
  uint32_t texel = 8;
  bool dynamicRenderingAvailable = false;
  uint32_t memoryTypeDeviceLocal = 0;
  uint32_t memoryTypeHostVisibleCoherent = 0;
  VkPhysicalDeviceMemoryProperties memProps{};
  // Config rates clamped to what the device supports.
  Rate innerRate{1, 1}, middleRate{2, 2}, outerRate{2, 2};
};

struct DeviceData {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  InstanceData* instance = nullptr;
  std::atomic<bool> active{false};
  PFN_vkGetInstanceProcAddr nextGipa = nullptr;
  PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
  DeviceDispatch d;
  Caps caps;
  Resources res;
  // Standalone mode (fixed foveation without an OpenXR session): the config the
  // Vulkan half loaded itself, for hot reload.
  Config localConfig;
  bool standalone = false;
  std::string configPath;
  int64_t configMtime = 0;
  int64_t lastConfigCheckNs = 0;
  int64_t lastGazeCheckNs = 0;  // standalone gaze poll (see vk_device.cpp)

  // Guards the maps below. vkCmd* hooks take a shared lock for reads; create and
  // destroy paths take the unique lock. Never hold it across a next-layer call.
  std::shared_mutex lock;
  std::unordered_map<VkImageView, ImageViewInfo> imageViews;
  std::unordered_map<VkCommandBuffer, CmdState> cmdStates;
  std::unordered_map<VkShaderModule, SpirvScan> shaderModules;
  std::unordered_map<VkRenderPass, RenderPassInfo> renderPasses;
  std::unordered_map<VkFramebuffer, FramebufferInfo> framebuffers;
};

// Last next-layer GetDeviceProcAddr we resolved, for forwarding calls that arrive
// with a device handle we do not own.
extern PFN_vkGetDeviceProcAddr g_lastNextGdpa;

InstanceData* findInstance(VkInstance instance);
InstanceData* findInstanceByKey(void* key);
void addInstance(std::unique_ptr<InstanceData> data);
std::unique_ptr<InstanceData> removeInstance(VkInstance instance);

DeviceData* findDevice(VkDevice device);
DeviceData* findDeviceByKey(void* key);
inline DeviceData* findDeviceForQueue(VkQueue queue) { return findDeviceByKey(dispatchKey(queue)); }
inline DeviceData* findDeviceForCommandBuffer(VkCommandBuffer cmd) {
  return findDeviceByKey(dispatchKey(cmd));
}
void addDevice(std::unique_ptr<DeviceData> data);
std::unique_ptr<DeviceData> removeDevice(VkDevice device);

}  // namespace framefov
