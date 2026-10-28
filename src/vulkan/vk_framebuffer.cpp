// Image view, framebuffer and render pass attachment bookkeeping.
//
// Framebuffers of a modified render pass get our rate image as an extra
// attachment (imageless framebuffers get an extra attachment image info), so
// pipelines and framebuffers stay compatible with the modified render passes.
#include <vector>

#include "common/classify.h"
#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"
#include "vulkan/vk_resources.h"

using namespace framefov;

namespace framefov {
namespace {

template <class T>
T* chainFind(void* head, VkStructureType sType) {
  for (auto* node = reinterpret_cast<VkBaseOutStructure*>(head); node; node = node->pNext) {
    if (node->sType == sType) return reinterpret_cast<T*>(node);
  }
  return nullptr;
}

uint32_t highestSetBit(uint32_t v) {
  uint32_t bit = 0;
  while (v >>= 1) ++bit;
  return bit;
}

uint32_t rateImageLayersFor(const DeviceData& data, const RenderPassInfo& rp) {
  if (rp.multiview && data.caps.layeredAttachments) return highestSetBit(rp.viewMaskUnion) + 1;
  return 1;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateImageView(VkDevice device,
                                                       const VkImageViewCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImageView* pView) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateImageView) {
    LOG_ERROR("vkCreateImageView: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = data->d.CreateImageView(device, pCreateInfo, pAllocator, pView);
  if (result == VK_SUCCESS && data->active && pView) {
    ImageViewInfo info;
    info.image = pCreateInfo->image;
    info.viewType = pCreateInfo->viewType;
    info.baseArrayLayer = pCreateInfo->subresourceRange.baseArrayLayer;
    info.layerCount = pCreateInfo->subresourceRange.layerCount;
    info.format = pCreateInfo->format;
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->imageViews[*pView] = info;
  }
  return result;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyImageView(VkDevice device, VkImageView imageView,
                                                    const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.DestroyImageView) {
    LOG_ERROR("vkDestroyImageView: no device data");
    return;
  }
  if (data->active) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->imageViews.erase(imageView);
  }
  data->d.DestroyImageView(device, imageView, pAllocator);
  FRAMEFOV_GUARD_END_VOID
}

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateFramebuffer) {
    LOG_ERROR("vkCreateFramebuffer: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!data->active) {
    return data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
  }

  RenderPassInfo rp;
  {
    std::shared_lock<std::shared_mutex> lock(data->lock);
    auto it = data->renderPasses.find(pCreateInfo->renderPass);
    if (it == data->renderPasses.end() || !it->second.modified) {
      return data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    }
    rp = it->second;
  }

  const uint32_t texel = data->caps.texel;
  const uint32_t sw = (pCreateInfo->width + texel - 1) / texel;
  const uint32_t sh = (pCreateInfo->height + texel - 1) / texel;
  const uint32_t rateLayers = rateImageLayersFor(*data, rp);

  auto snapshot = SharedState::get().snapshot();
  static const Snapshot kEmptySnapshot;
  const Snapshot& snap = snapshot ? *snapshot : kEmptySnapshot;
  const Config& config = effectiveConfig(snapshot);

  const bool candidate = isEyeCandidateSize(pCreateInfo->width, pCreateInfo->height, snap) &&
                         (rp.hasColor || !config.skipDepthOnly);
  RateImage* image = candidate ? acquireDynamicRateImage(*data, sw, sh, rateLayers)
                               : acquireNeutralImage(*data, sw, sh);
  if (!image) {
    LOG_WARN("could not create a rate image for framebuffer %ux%u; leaving it unmodified",
             pCreateInfo->width, pCreateInfo->height);
    return data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
  }

  FramebufferInfo info;
  info.rp = rp;
  info.width = pCreateInfo->width;
  info.height = pCreateInfo->height;
  info.layers = pCreateInfo->layers;
  info.image = image;
  info.neutral = !candidate;
  info.imageless = (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  if (info.imageless) {
    auto* attachments = chainFind<VkFramebufferAttachmentsCreateInfo>(
        const_cast<void*>(pCreateInfo->pNext), VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO);
    if (!attachments || !attachments->pAttachmentImageInfos) {
      LOG_WARN("imageless framebuffer without attachment image infos; leaving it unmodified");
      return data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    }
    std::vector<VkFramebufferAttachmentImageInfo> imageInfos(
        attachments->pAttachmentImageInfos,
        attachments->pAttachmentImageInfos + attachments->attachmentImageInfoCount);
    static const VkFormat kRateFormat = VK_FORMAT_R8_UINT;
    VkFramebufferAttachmentImageInfo ours{};
    ours.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;
    ours.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ours.width = sw;
    ours.height = sh;
    ours.layerCount = rateLayers;
    ours.viewFormatCount = 1;
    ours.pViewFormats = &kRateFormat;
    imageInfos.push_back(ours);

    // Patch-call-restore on the app's struct.
    const uint32_t savedCount = attachments->attachmentImageInfoCount;
    const VkFramebufferAttachmentImageInfo* savedInfos = attachments->pAttachmentImageInfos;
    attachments->attachmentImageInfoCount = (uint32_t)imageInfos.size();
    attachments->pAttachmentImageInfos = imageInfos.data();
    result = data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    attachments->attachmentImageInfoCount = savedCount;
    attachments->pAttachmentImageInfos = savedInfos;
  } else {
    if (!pCreateInfo->pAttachments) {
      return data->d.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
    }
    std::vector<VkImageView> views(pCreateInfo->pAttachments,
                                   pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
    info.appViews = views;
    views.push_back(image->view);
    VkFramebufferCreateInfo copy = *pCreateInfo;
    copy.attachmentCount = (uint32_t)views.size();
    copy.pAttachments = views.data();
    result = data->d.CreateFramebuffer(device, &copy, pAllocator, pFramebuffer);
  }

  if (result == VK_SUCCESS) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->framebuffers[*pFramebuffer] = info;
    LOG_DEBUG("framebuffer %p %ux%u x%u -> rate image %ux%u x%u (%s)", (void*)*pFramebuffer,
              info.width, info.height, info.layers, image->width, image->height, image->layers,
              candidate ? "candidate" : "neutral");
  }
  return result;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                      const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.DestroyFramebuffer) {
    LOG_ERROR("vkDestroyFramebuffer: no device data");
    return;
  }
  if (data->active) {
    // The rate images stay alive until vkDestroyDevice: in-flight command buffers
    // may still reference them, and they are cheap.
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->framebuffers.erase(framebuffer);
  }
  data->d.DestroyFramebuffer(device, framebuffer, pAllocator);
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace framefov
