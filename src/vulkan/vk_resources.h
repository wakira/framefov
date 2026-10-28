// VRS resources owned by a device: rate images (S), the neutral image and the
// static pattern buffers (P).
//
// Everything is created once and kept until vkDestroyDevice: in-flight command
// buffers may still reference old pattern buffers/images.
#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/config.h"
#include "common/pattern.h"

namespace framefov {

struct DeviceData;      // vk_dispatch.h
struct DeviceDispatch;  // vk_dispatch.h

struct RateImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0, layers = 1;
};

struct PatternBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  const uint8_t* mapped = nullptr;  // host visible + coherent, written once
  uint32_t ew = 0, eh = 0;
  PatternKey key;
};

struct Resources {
  std::mutex lock;  // guards everything below

  std::vector<std::unique_ptr<RateImage>> images;
  std::vector<std::unique_ptr<PatternBuffer>> patterns;
  std::unordered_map<uint64_t, RateImage*> dynamicRateImages;  // (w,h,layers)
  std::unordered_map<PatternKey, PatternBuffer*, PatternKeyHash> patternBuffers;
  RateImage* neutral = nullptr;

  void destroyAll(DeviceDispatch& d, VkDevice device);
};

// Cached per (w, h, layers), created on demand.
RateImage* acquireDynamicRateImage(DeviceData& data, uint32_t w, uint32_t h, uint32_t layers);
// The shared all-zero image, grown on demand; never smaller than (w, h).
RateImage* acquireNeutralImage(DeviceData& data, uint32_t w, uint32_t h);
// Static foveation pattern for a region of ew x eh texels.
const PatternBuffer* acquirePattern(DeviceData& data, uint32_t ew, uint32_t eh, bool eyeTracked,
                                    const Config& config);

}  // namespace framefov
