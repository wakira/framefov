// All Vulkan hooks implemented by the layer, plus the name -> function tables
// used by framefov_vkGetInstanceProcAddr / framefov_vkGetDeviceProcAddr.
#pragma once

#include <vulkan/vulkan.h>

#include "vulkan/vk_dispatch.h"

namespace framefov {

struct NamedHook {
  const char* name;
  PFN_vkVoidFunction fn;
};

// GetProcAddr entry points handed to the loader.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framefov_vkGetInstanceProcAddr(VkInstance instance,
                                                                    const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL framefov_vkGetDeviceProcAddr(VkDevice device,
                                                                  const char* name);

// Returns our hook for a name, or nullptr when the name is not hooked.
PFN_vkVoidFunction hookedInstanceFn(const char* name);  // instance + device hooks, for GIPA
PFN_vkVoidFunction hookedDeviceFn(const char* name);

// ---- instance level (vk_layer.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkInstance* pInstance);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyInstance(VkInstance instance,
                                                   const VkAllocationCallbacks* pAllocator);
// Hot reload for standalone mode (fixed foveation without OpenXR), called from
// vkBeginCommandBuffer. Guards itself with a one second throttle.
void framefovMaybeReloadStandaloneConfig(DeviceData& data);
// Late activation for a device created before the XrInstance (same call site and
// throttle). No-op unless the device is waiting for an instance.
void framefovMaybeActivate(DeviceData& data);
// Standalone mode only: poll SteamVR's own eye tracking (FRAMEFOV_OPENVR_GAZE)
// once a second and publish it as the shared snapshot's gaze. No-op otherwise.
void framefovMaybeUpdateStandaloneGaze(DeviceData& data);

// ---- device level (vk_device.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDevice* pDevice);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyDevice(VkDevice device,
                                                 const VkAllocationCallbacks* pAllocator);

// ---- image views (vk_framebuffer.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateImageView(VkDevice device,
                                                       const VkImageViewCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImageView* pView);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyImageView(VkDevice device, VkImageView imageView,
                                                    const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                      const VkAllocationCallbacks* pAllocator);

// ---- render passes (vk_renderpass.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass(VkDevice device,
                                                        const VkRenderPassCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkRenderPass* pRenderPass);
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass2KHR(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                     const VkAllocationCallbacks* pAllocator);

// ---- pipelines (vk_pipeline.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule);
VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyShaderModule(VkDevice device,
                                                       VkShaderModule shaderModule,
                                                       const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);

// ---- command buffers (vk_commands.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL framefov_vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                          const VkCommandBufferBeginInfo* pBeginInfo);
VKAPI_ATTR void VKAPI_CALL framefov_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                      uint32_t commandBufferCount,
                                                      const VkCommandBuffer* pCommandBuffers);
VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                     const VkRenderingInfo* pRenderingInfo);
VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer,
                                                        const VkRenderingInfo* pRenderingInfo);
VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                      const VkRenderPassBeginInfo* pRenderPassBegin,
                                                      VkSubpassContents contents);
VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass2(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo);
VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo);

}  // namespace framefov
