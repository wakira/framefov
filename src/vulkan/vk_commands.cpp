// Command buffer hooks: per-command-buffer state, dynamic rendering injection
// (05 §4) and the transfer-only rate image updates (05 §5).
#include <algorithm>
#include <vector>

#include "common/classify.h"
#include "common/guard.h"
#include "common/log.h"
#include "common/pattern.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"
#include "vulkan/vk_resources.h"

using namespace framefov;

namespace framefov {
namespace {

const void* chainFind(const void* head, VkStructureType sType) {
  for (auto* node = reinterpret_cast<const VkBaseInStructure*>(head); node; node = node->pNext) {
    if (node->sType == sType) return node;
  }
  return nullptr;
}

uint32_t highestSetBit(uint32_t v) {
  uint32_t bit = 0;
  while (v >>= 1) ++bit;
  return bit;
}

void imageBarrier(VkCommandBuffer cmd, DeviceData& data, VkImage image, uint32_t layers,
                  VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                  VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  data.d.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void clearRateImage(VkCommandBuffer cmd, DeviceData& data, RateImage& img) {
  if (!data.d.CmdClearColorImage) return;
  VkClearColorValue color{};
  color.uint32[0] = 0;
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers};
  data.d.CmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

// Does one region cover the whole rate image (all texels of its layer)?
bool regionCoversAll(const Region& r, uint32_t texel, const RateImage& img) {
  CopyRegion cr;
  if (!gazeCopyRegion(r.x, r.y, r.w, r.h, r.layer, texel, img.width, img.height, r.u, r.v, cr)) {
    return false;
  }
  return cr.imageX == 0 && cr.imageY == 0 && cr.imageW == img.width && cr.imageH == img.height;
}

// 05 §5.1: transition, clear when needed, copy the static pattern windows, and
// hand the image over in FSR_OPTIMAL. Transfer commands only.
void recordRateUpdate(VkCommandBuffer cmd, DeviceData& data, RateImage& img,
                      const FoveationPlan& plan, const Config& config, bool eyeTracked) {
  if (!data.d.CmdPipelineBarrier) return;
  const uint32_t t = data.caps.texel;

  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

  bool fullyCovered = true;
  for (uint32_t layer = 0; layer < img.layers && fullyCovered; ++layer) {
    bool covered = false;
    for (const Region& r : plan.regions) {
      if (r.layer == layer && regionCoversAll(r, t, img)) {
        covered = true;
        break;
      }
    }
    if (!covered) fullyCovered = false;
  }

  if (!fullyCovered) {
    clearRateImage(cmd, data, img);
    if (!plan.regions.empty()) {
      imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT);
    }
  }

  if (data.d.CmdCopyBufferToImage && !plan.regions.empty()) {
    std::vector<VkBufferImageCopy> copies;
    const PatternBuffer* current = nullptr;
    auto flush = [&]() {
      if (!copies.empty() && current) {
        data.d.CmdCopyBufferToImage(cmd, current->buffer, img.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(),
                                    copies.data());
      }
      copies.clear();
    };
    for (const Region& r : plan.regions) {
      CopyRegion cr;
      if (!gazeCopyRegion(r.x, r.y, r.w, r.h, r.layer, t, img.width, img.height, r.u, r.v, cr)) {
        continue;
      }
      const PatternBuffer* pattern = acquirePattern(data, cr.imageW, cr.imageH, eyeTracked, config);
      if (!pattern) continue;
      if (pattern != current) {
        flush();
        current = pattern;
      }
      copies.push_back(toVkBufferImageCopy(cr));
    }
    flush();
  }

  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
               VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
}

// 05 §5.3: the shared neutral image must contain zeros before it is used as an
// attachment; do it once per command buffer (and again if the image was grown).
void ensureNeutralInitialized(VkCommandBuffer cmd, DeviceData& data, CmdState& state,
                              RateImage& img) {
  if (state.neutralInitialized && state.neutralInitImage == img.image) return;
  if (!data.d.CmdPipelineBarrier) return;
  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  clearRateImage(cmd, data, img);
  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
               VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
  state.neutralInitialized = true;
  state.neutralInitImage = img.image;
}

void fillTargetFromRenderingInfo(DeviceData& data, const VkRenderingInfo* info, TargetDesc& desc) {
  desc.width = (uint32_t)info->renderArea.offset.x + info->renderArea.extent.width;
  desc.height = (uint32_t)info->renderArea.offset.y + info->renderArea.extent.height;
  desc.source = "render area";
  desc.viewMask = info->viewMask;
  desc.layers = info->layerCount;
  desc.vkDevice = (uint64_t)(uintptr_t)data.device;
  desc.hasColor = false;
  desc.colorAttachments.clear();
  if (!info->pColorAttachments) return;
  std::shared_lock<std::shared_mutex> lock(data.lock);
  for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
    const VkRenderingAttachmentInfo& att = info->pColorAttachments[i];
    if (!att.imageView) continue;
    desc.hasColor = true;
    auto it = data.imageViews.find(att.imageView);
    if (it == data.imageViews.end()) {
      desc.colorAttachments.push_back(TargetAtt{0, 0, 1});
    } else {
      desc.colorAttachments.push_back(TargetAtt{(uint64_t)(uintptr_t)it->second.image,
                                                it->second.baseArrayLayer, it->second.layerCount});
    }
  }
}

uint32_t rateImageLayersFor(const DeviceData& data, const VkRenderingInfo* info) {
  if (info->viewMask != 0 && data.caps.layeredAttachments) {
    return highestSetBit(info->viewMask) + 1;
  }
  return 1;
}

// Shared body for vkCmdBeginRendering and its KHR alias.
void beginRenderingImpl(VkCommandBuffer cmd, const VkRenderingInfo* info,
                        PFN_vkCmdBeginRendering next) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || !next || !info) {
    LOG_ONCE(DEBUG, "skip-beginRendering-nodevice", "dynamic rendering pass passed through: %s",
             !next ? "no next entry point" : (!data ? "unknown command buffer/device"
                                                   : "no rendering info"));
    if (next) next(cmd, info);
    return;
  }
  FRAMEFOV_GUARD_BEGIN
  if (!data->active) {
    LOG_ONCE(DEBUG, "skip-beginRendering-inactive",
             "dynamic rendering pass passed through: device not active");
    next(cmd, info);
    return;
  }
  if (chainFind(info->pNext, VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) ||
      chainFind(info->pNext, VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)) {
    next(cmd, info);  // the app manages VRS / density map itself
    return;
  }

  const bool resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0;
  const bool suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;

  if (resuming) {
    VkImageView lastView = VK_NULL_HANDLE;
    {
      std::shared_lock<std::shared_mutex> lock(data->lock);
      auto it = data->cmdStates.find(cmd);
      if (it != data->cmdStates.end()) lastView = it->second.lastAttachedView;
    }
    if (lastView == VK_NULL_HANDLE) {
      LOG_ONCE(DEBUG, "skip-beginRendering-resume",
               "dynamic rendering pass passed through: resumed without a recorded rate image");
      next(cmd, info);  // no attachment to resume with
      return;
    }
    VkRenderingFragmentShadingRateAttachmentInfoKHR fsr{};
    fsr.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
    fsr.imageView = lastView;
    fsr.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    fsr.shadingRateAttachmentTexelSize = {data->caps.texel, data->caps.texel};
    VkRenderingInfo copy = *info;
    fsr.pNext = const_cast<void*>(info->pNext);
    copy.pNext = &fsr;
    next(cmd, &copy);
    return;
  }

  auto snapshot = SharedState::get().snapshot();
  const Config& config = effectiveConfig(snapshot);
  static const Snapshot kEmptySnapshot;
  const Snapshot& snap = snapshot ? *snapshot : kEmptySnapshot;

  TargetDesc desc;
  fillTargetFromRenderingInfo(*data, info, desc);
  const uint32_t rateLayers = rateImageLayersFor(*data, info);

  SharedState::get().stats.passes.fetch_add(1, std::memory_order_relaxed);
  const FoveationPlan plan = planForTarget(desc, snap, rateLayers);
  for (const Region& r : plan.regions) {
    LOG_DEBUG("  region layer %u rect (%d,%d) %ux%u gaze (%.3f,%.3f)", r.layer, r.x, r.y, r.w, r.h,
              r.u, r.v);
  }
  if (plan.regions.empty()) {
    SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
    next(cmd, info);
    return;
  }

  const uint32_t t = data->caps.texel;
  const uint32_t sw = (desc.width + t - 1) / t;
  const uint32_t sh = (desc.height + t - 1) / t;
  RateImage* img = acquireDynamicRateImage(*data, sw, sh, rateLayers);
  if (!img) {
    next(cmd, info);
    return;
  }

  const bool eyeTracked = snap.eyes[0].eyeTracked && snap.eyes[1].eyeTracked;
  recordRateUpdate(cmd, *data, *img, plan, config, eyeTracked);

  VkRenderingFragmentShadingRateAttachmentInfoKHR fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
  fsr.imageView = img->view;
  fsr.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
  fsr.shadingRateAttachmentTexelSize = {t, t};
  VkRenderingInfo copy = *info;
  fsr.pNext = const_cast<void*>(info->pNext);
  copy.pNext = &fsr;

  if (suspending) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->cmdStates[cmd].lastAttachedView = img->view;
  }

  SharedState::get().stats.foveatedPasses.fetch_add(1, std::memory_order_relaxed);
  LOG_ONCE(INFO, ("attach:" + std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                  "x" + std::to_string(img->layers)).c_str(),
           "attaching rate image %ux%u texels x%u layer(s) to %ux%u viewMask=0x%x "
           "(%zu region(s), inner %s middle %s outer %s)",
           img->width, img->height, img->layers, desc.width, desc.height, desc.viewMask,
           plan.regions.size(), rateToString(data->caps.innerRate).c_str(),
           rateToString(data->caps.middleRate).c_str(), rateToString(data->caps.outerRate).c_str());
  LOG_DEBUG("dynamic rendering %ux%u viewMask=0x%x: %zu region(s), attaching rate image "
            "%ux%u texels x%u layer(s) (pattern rates: inner %s, middle %s, outer %s; "
            "debugMode %d)",
            desc.width, desc.height, desc.viewMask, plan.regions.size(), img->width, img->height,
            img->layers, rateToString(data->caps.innerRate).c_str(),
            rateToString(data->caps.middleRate).c_str(), rateToString(data->caps.outerRate).c_str(),
            config.debugMode);
  next(cmd, &copy);
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  if (!data || !data->d.BeginCommandBuffer) {
    LOG_ERROR("vkBeginCommandBuffer: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = data->d.BeginCommandBuffer(commandBuffer, pBeginInfo);
  if (result == VK_SUCCESS) {
    // A device created before the XrInstance activates here, long before the app
    // renders (throttled inside).
    if (!data->active) framefovMaybeActivate(*data);
    if (data->active) {
      std::unique_lock<std::shared_mutex> lock(data->lock);
      if (data->standalone) framefovMaybeReloadStandaloneConfig(*data);
      if (data->standalone) framefovMaybeUpdateStandaloneGaze(*data);
      data->cmdStates[commandBuffer] = CmdState{};
    }
  }
  return result;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                      uint32_t commandBufferCount,
                                                      const VkCommandBuffer* pCommandBuffers) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.FreeCommandBuffers) {
    LOG_ERROR("vkFreeCommandBuffers: no device data");
    return;
  }
  if (data->active) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    for (uint32_t i = 0; i < commandBufferCount; ++i) data->cmdStates.erase(pCommandBuffers[i]);
  }
  data->d.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
  FRAMEFOV_GUARD_END_VOID
}

VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                     const VkRenderingInfo* pRenderingInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderingImpl(commandBuffer, pRenderingInfo, data ? data->d.CmdBeginRendering : nullptr);
}

VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer,
                                                        const VkRenderingInfo* pRenderingInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderingImpl(commandBuffer, pRenderingInfo, data ? data->d.CmdBeginRenderingKHR : nullptr);
}

namespace {

// Shared body for vkCmdBeginRenderPass and its v2/KHR variants (05 §3).
void beginRenderPassImpl(VkCommandBuffer cmd, const VkRenderPassBeginInfo* beginInfo,
                         const VkSubpassBeginInfo* subpassInfo, VkSubpassContents contents,
                         PFN_vkCmdBeginRenderPass next1, PFN_vkCmdBeginRenderPass2 next2) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || !beginInfo || (!next1 && !next2)) {
    LOG_ONCE(DEBUG, "skip-beginRenderPass-nodevice", "render pass passed through: %s",
             (!next1 && !next2) ? "no next entry point"
                                : (!data ? "unknown command buffer/device" : "no begin info"));
    if (next2) next2(cmd, beginInfo, subpassInfo);
    else if (next1) next1(cmd, beginInfo, contents);
    return;
  }
  auto callNext = [&](const VkRenderPassBeginInfo* info) {
    if (next2) next2(cmd, info, subpassInfo);
    else next1(cmd, info, contents);
  };

  FRAMEFOV_GUARD_BEGIN
  if (!data->active) {
    callNext(beginInfo);
    return;
  }

  FramebufferInfo fb;
  {
    std::shared_lock<std::shared_mutex> lock(data->lock);
    auto it = data->framebuffers.find(beginInfo->framebuffer);
    if (it == data->framebuffers.end() || !it->second.rp.modified || !it->second.image) {
      LOG_ONCE(DEBUG, "skip-beginRenderPass-fb",
               "render pass passed through: framebuffer %s",
               it == data->framebuffers.end()
                   ? "not created through us"
                   : (!it->second.rp.modified ? "its render pass was not modified"
                                              : "no rate image"));
      callNext(beginInfo);
      return;
    }
    fb = it->second;
  }

  const VkRenderPassAttachmentBeginInfo* attachmentBegin = nullptr;
  std::vector<VkImageView> views;
  if (fb.imageless) {
    attachmentBegin = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(
        chainFind(beginInfo->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO));
    if (!attachmentBegin || !attachmentBegin->pAttachments ||
        attachmentBegin->attachmentCount < fb.rp.origAttachmentCount) {
      callNext(beginInfo);
      return;
    }
    views.assign(attachmentBegin->pAttachments,
                 attachmentBegin->pAttachments + attachmentBegin->attachmentCount);
  } else {
    views = fb.appViews;
  }

  auto snapshot = SharedState::get().snapshot();
  static const Snapshot kEmptySnapshot;
  const Snapshot& snap = snapshot ? *snapshot : kEmptySnapshot;
  const Config& config = effectiveConfig(snapshot);
  SharedState::get().stats.passes.fetch_add(1, std::memory_order_relaxed);

  if (fb.neutral) {
    // Nothing to foveate: make sure the shared neutral image contains zeros.
    std::unique_lock<std::shared_mutex> lock(data->lock);
    CmdState& state = data->cmdStates[cmd];
    ensureNeutralInitialized(cmd, *data, state, *fb.image);
    SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
  } else {
    TargetDesc desc;
    desc.width = fb.width;
    desc.height = fb.height;
    desc.viewMask = fb.rp.viewMaskUnion;
    desc.layers = fb.layers;
    desc.hasColor = fb.rp.hasColor;
    desc.vkDevice = (uint64_t)(uintptr_t)data->device;
    {
      std::shared_lock<std::shared_mutex> lock(data->lock);
      for (VkImageView view : views) {
        auto it = data->imageViews.find(view);
        if (it == data->imageViews.end()) {
          desc.colorAttachments.push_back(TargetAtt{0, 0, 1});
        } else {
          desc.colorAttachments.push_back(TargetAtt{(uint64_t)(uintptr_t)it->second.image,
                                                    it->second.baseArrayLayer,
                                                    it->second.layerCount});
        }
      }
    }
    const FoveationPlan plan = planForTarget(desc, snap, fb.image->layers);
    const bool eyeTracked = snap.eyes[0].eyeTracked && snap.eyes[1].eyeTracked;
    recordRateUpdate(cmd, *data, *fb.image, plan, config, eyeTracked);
    if (plan.regions.empty()) {
      SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
    } else {
      SharedState::get().stats.foveatedPasses.fetch_add(1, std::memory_order_relaxed);
      LOG_DEBUG("render pass %ux%u viewMask=0x%x: %zu region(s), attaching rate image "
                "%ux%u texels x%u layer(s) (pattern rates: inner %s, middle %s, outer %s; "
                "debugMode %d)",
                desc.width, desc.height, desc.viewMask, plan.regions.size(), fb.image->width,
                fb.image->height, fb.image->layers, rateToString(data->caps.innerRate).c_str(),
                rateToString(data->caps.middleRate).c_str(),
                rateToString(data->caps.outerRate).c_str(), config.debugMode);
    }
  }

  if (fb.imageless) {
    // Patch-call-restore: append our view to the attachment list.
    auto* mutableBegin = const_cast<VkRenderPassAttachmentBeginInfo*>(attachmentBegin);
    const uint32_t savedCount = mutableBegin->attachmentCount;
    const VkImageView* savedViews = mutableBegin->pAttachments;
    views.push_back(fb.image->view);
    mutableBegin->attachmentCount = (uint32_t)views.size();
    mutableBegin->pAttachments = views.data();
    callNext(beginInfo);
    mutableBegin->attachmentCount = savedCount;
    mutableBegin->pAttachments = savedViews;
  } else {
    callNext(beginInfo);
  }
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                      const VkRenderPassBeginInfo* pRenderPassBegin,
                                                      VkSubpassContents contents) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, nullptr, contents,
                      data ? data->d.CmdBeginRenderPass : nullptr, nullptr);
}

VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                       const VkRenderPassBeginInfo* pRenderPassBegin,
                                                       const VkSubpassBeginInfo* pSubpassBeginInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, pSubpassBeginInfo, VK_SUBPASS_CONTENTS_INLINE,
                      nullptr, data ? data->d.CmdBeginRenderPass2 : nullptr);
}

VKAPI_ATTR void VKAPI_CALL framefov_vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, pSubpassBeginInfo, VK_SUBPASS_CONTENTS_INLINE,
                      nullptr, data ? data->d.CmdBeginRenderPass2KHR : nullptr);
}

}  // namespace framefov
