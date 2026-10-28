// Render pass interception: v1 -> v2 conversion, FSR attachment injection and
// destroy bookkeeping (05 §1).
#include <string>
#include <vector>

#include "common/guard.h"
#include "common/log.h"
#include "common/renderpass_convert.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"

using namespace framefov;

namespace framefov {
namespace {

const VkBaseInStructure* chainBase(const void* pNext) {
  return reinterpret_cast<const VkBaseInStructure*>(pNext);
}

const void* chainFind(const void* head, VkStructureType sType, int* count = nullptr) {
  const void* found = nullptr;
  int n = 0;
  for (const auto* node = chainBase(head); node; node = node->pNext) {
    if (node->sType == sType) {
      if (!found) found = node;
      ++n;
    }
  }
  if (count) *count = n;
  return found;
}

// 05 §1.2: leave the render pass alone when the app brings its own VRS or a
// fragment density map.
bool mustSkip(const VkRenderPassCreateInfo2& info, std::string* why) {
  if (chainFind(info.pNext, VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT)) {
    *why = "app uses VkRenderPassFragmentDensityMapCreateInfoEXT";
    return true;
  }
  for (uint32_t i = 0; i < info.subpassCount; ++i) {
    const VkSubpassDescription2& sub = info.pSubpasses[i];
    if (chainFind(sub.pNext, VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR)) {
      *why = "subpass already has a fragment shading rate attachment";
      return true;
    }
    if (chainFind(sub.pNext, VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT)) {
      *why = "subpass uses multisampled render to single sampled";
      return true;
    }
  }
  return false;
}

// Owns every structure the injected render pass create info points at.
struct InjectedRenderPass {
  std::vector<VkAttachmentDescription2> attachments;
  std::vector<VkSubpassDescription2> subpasses;
  std::vector<VkFragmentShadingRateAttachmentInfoKHR> fsrInfos;
  std::vector<VkAttachmentReference2> fsrRefs;
  std::vector<VkSubpassDependency2> dependencies;
  std::vector<uint32_t> correlatedViewMasks;
  VkRenderPassCreateInfo2 info{};
};

// Appends the R8_UINT FSR attachment and chains a fragment shading rate
// attachment into every subpass.
bool injectFsr(const VkRenderPassCreateInfo2& in, uint32_t texel, InjectedRenderPass& out,
               RenderPassInfo& record) {
  record = RenderPassInfo{};
  record.origAttachmentCount = in.attachmentCount;
  record.fsrIndex = in.attachmentCount;
  record.modified = true;

  out.attachments.assign(in.pAttachments, in.pAttachments + in.attachmentCount);
  VkAttachmentDescription2 fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
  fsr.format = VK_FORMAT_R8_UINT;
  fsr.samples = VK_SAMPLE_COUNT_1_BIT;
  fsr.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  fsr.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  fsr.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  fsr.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  fsr.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
  fsr.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
  out.attachments.push_back(fsr);

  out.subpasses.assign(in.pSubpasses, in.pSubpasses + in.subpassCount);
  out.fsrInfos.resize(in.subpassCount);
  out.fsrRefs.resize(in.subpassCount);
  for (uint32_t i = 0; i < in.subpassCount; ++i) {
    out.fsrRefs[i] = VkAttachmentReference2{};
    out.fsrRefs[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    out.fsrRefs[i].attachment = record.fsrIndex;
    out.fsrRefs[i].layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    out.fsrRefs[i].aspectMask = 0;

    out.fsrInfos[i] = VkFragmentShadingRateAttachmentInfoKHR{};
    out.fsrInfos[i].sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
    out.fsrInfos[i].pNext = const_cast<void*>(in.pSubpasses[i].pNext);
    out.fsrInfos[i].pFragmentShadingRateAttachment = &out.fsrRefs[i];
    out.fsrInfos[i].shadingRateAttachmentTexelSize = {texel, texel};
    out.subpasses[i].pNext = &out.fsrInfos[i];

    record.viewMaskUnion |= out.subpasses[i].viewMask;
    if (out.subpasses[i].colorAttachmentCount > 0) record.hasColor = true;
  }
  record.multiview = record.viewMaskUnion != 0;

  out.dependencies.assign(in.pDependencies, in.pDependencies + in.dependencyCount);
  out.correlatedViewMasks.assign(in.pCorrelatedViewMasks,
                                 in.pCorrelatedViewMasks + in.correlatedViewMaskCount);

  out.info = in;
  out.info.attachmentCount = (uint32_t)out.attachments.size();
  out.info.pAttachments = out.attachments.data();
  out.info.subpassCount = (uint32_t)out.subpasses.size();
  out.info.pSubpasses = out.subpasses.data();
  out.info.dependencyCount = (uint32_t)out.dependencies.size();
  out.info.pDependencies = out.dependencies.empty() ? nullptr : out.dependencies.data();
  out.info.correlatedViewMaskCount = (uint32_t)out.correlatedViewMasks.size();
  out.info.pCorrelatedViewMasks = out.correlatedViewMasks.empty() ? nullptr
                                                                 : out.correlatedViewMasks.data();
  return true;
}

void recordRenderPass(DeviceData& data, VkRenderPass pass, const RenderPassInfo& info) {
  if (!pass) return;
  std::unique_lock<std::shared_mutex> lock(data.lock);
  data.renderPasses[pass] = info;
}

// Creates through the v2 entry point (05 §1.1/§1.2).
VkResult createPass2(DeviceData& data, VkDevice device, const VkRenderPassCreateInfo2* info,
                     const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  if (data.d.CreateRenderPass2KHR) return data.d.CreateRenderPass2KHR(device, info, alloc, out);
  if (data.d.CreateRenderPass2) return data.d.CreateRenderPass2(device, info, alloc, out);
  return VK_ERROR_EXTENSION_NOT_PRESENT;
}

}  // namespace
}  // namespace framefov

namespace framefov {

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass(VkDevice device,
                                                        const VkRenderPassCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkRenderPass* pRenderPass) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateRenderPass) {
    LOG_ERROR("vkCreateRenderPass: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!data->active) {
    return data->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
  }

  ConvertedRenderPass converted;
  std::string error;
  if (!convertRenderPass1to2(*pCreateInfo, converted, &error)) {
    LOG_ONCE(WARN, "rp-convert-skip", "vkCreateRenderPass passed through unchanged (%s)",
             error.c_str());
    VkResult result = data->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) recordRenderPass(*data, *pRenderPass, RenderPassInfo{});
    return result;
  }

  std::string why;
  if (mustSkip(converted.info, &why)) {
    LOG_WARN("vkCreateRenderPass left unmodified: %s (VRS disabled for it)", why.c_str());
    VkResult result = data->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
      RenderPassInfo rec;
      rec.skipped = true;
      recordRenderPass(*data, *pRenderPass, rec);
    }
    return result;
  }

  InjectedRenderPass injected;
  RenderPassInfo record;
  injectFsr(converted.info, data->caps.texel, injected, record);
  VkResult result = createPass2(*data, device, &injected.info, pAllocator, pRenderPass);
  if (result != VK_SUCCESS) {
    LOG_WARN("vkCreateRenderPass2 failed for a converted v1 render pass (%d); retrying v1",
             (int)result);
    result = data->d.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) recordRenderPass(*data, *pRenderPass, RenderPassInfo{});
    return result;
  }
  recordRenderPass(*data, *pRenderPass, record);
  LOG_DEBUG("vkCreateRenderPass %p converted+injected (attachments %u -> %u)", (void*)*pRenderPass,
            record.origAttachmentCount, record.origAttachmentCount + 1);
  return VK_SUCCESS;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass2(VkDevice device,
                                                         const VkRenderPassCreateInfo2* pCreateInfo,
                                                         const VkAllocationCallbacks* pAllocator,
                                                         VkRenderPass* pRenderPass) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateRenderPass2) {
    LOG_ERROR("vkCreateRenderPass2: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!data->active) {
    return data->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
  }
  std::string why;
  if (mustSkip(*pCreateInfo, &why)) {
    LOG_WARN("vkCreateRenderPass2 left unmodified: %s", why.c_str());
    VkResult result = data->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
      RenderPassInfo rec;
      rec.skipped = true;
      recordRenderPass(*data, *pRenderPass, rec);
    }
    return result;
  }
  InjectedRenderPass injected;
  RenderPassInfo record;
  injectFsr(*pCreateInfo, data->caps.texel, injected, record);
  VkResult result = data->d.CreateRenderPass2(device, &injected.info, pAllocator, pRenderPass);
  if (result != VK_SUCCESS) {
    result = data->d.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) recordRenderPass(*data, *pRenderPass, RenderPassInfo{});
    return result;
  }
  recordRenderPass(*data, *pRenderPass, record);
  return VK_SUCCESS;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateRenderPass2KHR(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateRenderPass2KHR) {
    LOG_ERROR("vkCreateRenderPass2KHR: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!data->active) {
    return data->d.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
  }
  std::string why;
  if (mustSkip(*pCreateInfo, &why)) {
    LOG_WARN("vkCreateRenderPass2KHR left unmodified: %s", why.c_str());
    VkResult result = data->d.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) {
      RenderPassInfo rec;
      rec.skipped = true;
      recordRenderPass(*data, *pRenderPass, rec);
    }
    return result;
  }
  InjectedRenderPass injected;
  RenderPassInfo record;
  injectFsr(*pCreateInfo, data->caps.texel, injected, record);
  VkResult result = data->d.CreateRenderPass2KHR(device, &injected.info, pAllocator, pRenderPass);
  if (result != VK_SUCCESS) {
    result = data->d.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
    if (result == VK_SUCCESS) recordRenderPass(*data, *pRenderPass, RenderPassInfo{});
    return result;
  }
  recordRenderPass(*data, *pRenderPass, record);
  return VK_SUCCESS;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                     const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.DestroyRenderPass) {
    LOG_ERROR("vkDestroyRenderPass: no device data");
    return;
  }
  if (data->active) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->renderPasses.erase(renderPass);
  }
  data->d.DestroyRenderPass(device, renderPass, pAllocator);
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace framefov
