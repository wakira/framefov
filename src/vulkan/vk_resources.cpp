#include "vulkan/vk_resources.h"

#include <cstring>

#include "common/log.h"
#include "vulkan/vk_dispatch.h"

namespace framefov {

namespace {

bool pickMemoryType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits,
                    VkMemoryPropertyFlags want, uint32_t* out) {
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (!(bits & (1u << i))) continue;
    if ((mp.memoryTypes[i].propertyFlags & want) == want) {
      *out = i;
      return true;
    }
  }
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (bits & (1u << i)) {
      *out = i;
      return true;
    }
  }
  return false;
}

bool allocAndBindImage(DeviceData& data, VkImage image, VkMemoryPropertyFlags want,
                       VkDeviceMemory* memory) {
  DeviceDispatch& d = data.d;
  VkMemoryRequirements req{};
  d.GetImageMemoryRequirements(data.device, image, &req);
  uint32_t type = 0;
  if (!pickMemoryType(data.caps.memProps, req.memoryTypeBits, want, &type)) {
    LOG_ERROR("no memory type for rate image");
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (d.AllocateMemory(data.device, &ai, nullptr, memory) != VK_SUCCESS) {
    LOG_ERROR("vkAllocateMemory failed for rate image");
    return false;
  }
  if (d.BindImageMemory(data.device, image, *memory, 0) != VK_SUCCESS) {
    LOG_ERROR("vkBindImageMemory failed for rate image");
    d.FreeMemory(data.device, *memory, nullptr);
    *memory = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool createRateImage(DeviceData& data, uint32_t w, uint32_t h, uint32_t layers, RateImage& out) {
  DeviceDispatch& d = data.d;
  if (!d.CreateImage || !d.CreateImageView || !d.AllocateMemory || !d.BindImageMemory ||
      !d.GetImageMemoryRequirements) {
    return false;
  }
  out = RateImage{};
  out.width = w;
  out.height = h;
  out.layers = layers;

  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8_UINT;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (d.CreateImage(data.device, &ci, nullptr, &out.image) != VK_SUCCESS) {
    LOG_ERROR("vkCreateImage failed for %ux%u x%u rate image", w, h, layers);
    out.image = VK_NULL_HANDLE;
    return false;
  }
  if (!allocAndBindImage(data, out.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &out.memory)) {
    d.DestroyImage(data.device, out.image, nullptr);
    out.image = VK_NULL_HANDLE;
    return false;
  }

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out.image;
  vi.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R8_UINT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  if (d.CreateImageView(data.device, &vi, nullptr, &out.view) != VK_SUCCESS) {
    LOG_ERROR("vkCreateImageView failed for rate image");
    d.FreeMemory(data.device, out.memory, nullptr);
    d.DestroyImage(data.device, out.image, nullptr);
    out = RateImage{};
    return false;
  }
  LOG_DEBUG("created rate image %ux%u x%u", w, h, layers);
  return true;
}

uint64_t rateImageKey(uint32_t w, uint32_t h, uint32_t layers) {
  return ((uint64_t)w << 40) | ((uint64_t)h << 20) | layers;
}

}  // namespace

void Resources::destroyAll(DeviceDispatch& d, VkDevice device) {
  std::lock_guard<std::mutex> guard(lock);
  for (auto& img : images) {
    if (img->view) d.DestroyImageView(device, img->view, nullptr);
    if (img->image) d.DestroyImage(device, img->image, nullptr);
    if (img->memory) d.FreeMemory(device, img->memory, nullptr);
  }
  images.clear();
  for (auto& p : patterns) {
    if (p->buffer) d.DestroyBuffer(device, p->buffer, nullptr);
    if (p->memory) d.FreeMemory(device, p->memory, nullptr);
  }
  patterns.clear();
  dynamicRateImages.clear();
  patternBuffers.clear();
  neutral = nullptr;
}

RateImage* acquireDynamicRateImage(DeviceData& data, uint32_t w, uint32_t h, uint32_t layers) {
  Resources& res = data.res;
  std::lock_guard<std::mutex> guard(res.lock);
  const uint64_t key = rateImageKey(w, h, layers);
  auto it = res.dynamicRateImages.find(key);
  if (it != res.dynamicRateImages.end()) return it->second;

  auto img = std::make_unique<RateImage>();
  if (!createRateImage(data, w, h, layers, *img)) return nullptr;
  RateImage* raw = img.get();
  res.images.push_back(std::move(img));
  res.dynamicRateImages[key] = raw;
  return raw;
}

RateImage* acquireNeutralImage(DeviceData& data, uint32_t w, uint32_t h) {
  Resources& res = data.res;
  std::lock_guard<std::mutex> guard(res.lock);
  if (res.neutral && res.neutral->width >= w && res.neutral->height >= h) return res.neutral;

  const uint32_t nw = std::max(w, res.neutral ? res.neutral->width : 0u);
  const uint32_t nh = std::max(h, res.neutral ? res.neutral->height : 0u);
  auto img = std::make_unique<RateImage>();
  if (!createRateImage(data, nw, nh, 1, *img)) return res.neutral;
  RateImage* raw = img.get();
  res.images.push_back(std::move(img));
  res.neutral = raw;
  LOG_DEBUG("neutral rate image is now %ux%u", nw, nh);
  return raw;
}

const PatternBuffer* acquirePattern(DeviceData& data, uint32_t ew, uint32_t eh, bool eyeTracked,
                                    const Config& config) {
  Resources& res = data.res;
  PatternKey key;
  key.ew = ew;
  key.eh = eh;
  key.generation = config.generation;
  key.debugMode = config.debugMode;
  key.eyeTracked = eyeTracked;
  key.drawRadii = config.drawRadii;
  key.ringWidth = config.ringWidth;

  std::lock_guard<std::mutex> guard(res.lock);
  auto it = res.patternBuffers.find(key);
  if (it != res.patternBuffers.end()) return it->second;

  const Caps& caps = data.caps;
  std::vector<Rate> supported;
  supported.reserve(caps.supportedRates.size());
  for (const VkExtent2D& s : caps.supportedRates) {
    supported.push_back(Rate{(uint8_t)s.width, (uint8_t)s.height});
  }
  const PatternParams params = makePatternParams(config, caps.innerRate, caps.middleRate,
                                                 caps.outerRate, eyeTracked, &supported);
  const std::vector<uint8_t> bytes = buildPattern(ew, eh, params);
  if (bytes.empty()) return nullptr;

  if (config.drawRadii) {
    // Say which rates the radii circles ended up using: on hardware whose
    // supported set has no rate that differs from both zones (for example only
    // 1x1 and 2x2), the ring can only be drawn as the edge of a zone.
    Rate ri, ro;
    decodeRate(params.ringInnerRate, ri);
    decodeRate(params.ringOuterRate, ro);
    const std::string key = "draw-radii-" + std::to_string(config.generation);
    LOG_ONCE(INFO, key.c_str(),
             "draw_radii=%u: circle at r=%.2f rate %s, circle at r=%.2f rate %s, %u texel(s) "
             "wide (eye %ux%u texels, zones %s/%s/%s)",
             config.drawRadii, params.innerRadius, rateToString(ri).c_str(), params.outerRadius,
             rateToString(ro).c_str(), config.ringWidth, ew, eh,
             rateToString(params.innerRate).c_str(), rateToString(params.middleRate).c_str(),
             rateToString(params.outerRate).c_str());
  }

  auto pb = std::make_unique<PatternBuffer>();
  pb->ew = ew;
  pb->eh = eh;
  pb->key = key;

  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes.size();
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  DeviceDispatch& d = data.d;
  if (!d.CreateBuffer || !d.AllocateMemory || !d.BindBufferMemory || !d.MapMemory ||
      !d.GetBufferMemoryRequirements) {
    return nullptr;
  }
  if (d.CreateBuffer(data.device, &bci, nullptr, &pb->buffer) != VK_SUCCESS) {
    LOG_ERROR("vkCreateBuffer failed for pattern buffer");
    return nullptr;
  }
  VkMemoryRequirements req{};
  d.GetBufferMemoryRequirements(data.device, pb->buffer, &req);
  uint32_t type = 0;
  const VkMemoryPropertyFlags want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (!pickMemoryType(data.caps.memProps, req.memoryTypeBits, want, &type)) {
    d.DestroyBuffer(data.device, pb->buffer, nullptr);
    return nullptr;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (d.AllocateMemory(data.device, &ai, nullptr, &pb->memory) != VK_SUCCESS ||
      d.BindBufferMemory(data.device, pb->buffer, pb->memory, 0) != VK_SUCCESS) {
    LOG_ERROR("pattern buffer memory allocation failed");
    if (pb->memory) d.FreeMemory(data.device, pb->memory, nullptr);
    d.DestroyBuffer(data.device, pb->buffer, nullptr);
    return nullptr;
  }
  void* mapped = nullptr;
  if (d.MapMemory(data.device, pb->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    LOG_ERROR("vkMapMemory failed for pattern buffer");
    d.FreeMemory(data.device, pb->memory, nullptr);
    d.DestroyBuffer(data.device, pb->buffer, nullptr);
    return nullptr;
  }
  memcpy(mapped, bytes.data(), bytes.size());
  pb->mapped = static_cast<const uint8_t*>(mapped);

  PatternBuffer* raw = pb.get();
  res.patterns.push_back(std::move(pb));
  res.patternBuffers[key] = raw;
  LOG_DEBUG("created pattern buffer %ux%u (gen %llu, debug %d, tracked %d): center=%u corner=%u",
            ew, eh, (unsigned long long)key.generation, key.debugMode, (int)key.eyeTracked,
            bytes[(size_t)eh * (2 * ew) + ew], bytes[0]);
  return raw;
}

}  // namespace framefov
