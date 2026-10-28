#include "common/renderpass_convert.h"

#include <cstring>

namespace framefov {

VkImageAspectFlags aspectsForFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

namespace {

VkAttachmentReference2 makeRef(const VkAttachmentReference& r, bool input, VkImageAspectFlags aspects) {
  VkAttachmentReference2 out{};
  out.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
  out.pNext = nullptr;
  out.attachment = r.attachment;
  out.layout = r.layout;
  // Per the plan: aspectMask stays 0 except for input attachments (where the
  // spec uses it to select the depth/stencil aspect being read).
  out.aspectMask = input ? aspects : (VkImageAspectFlags)0;
  return out;
}

}  // namespace

bool convertRenderPass1to2(const VkRenderPassCreateInfo& in, ConvertedRenderPass& out,
                           std::string* error) {
  auto fail = [error](const char* msg) {
    if (error) *error = msg;
    return false;
  };

  const VkRenderPassMultiviewCreateInfo* multiview = nullptr;
  const VkRenderPassInputAttachmentAspectCreateInfo* inputAspect = nullptr;
  for (const auto* base = reinterpret_cast<const VkBaseInStructure*>(in.pNext); base;
       base = base->pNext) {
    switch (base->sType) {
      case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
        multiview = reinterpret_cast<const VkRenderPassMultiviewCreateInfo*>(base);
        break;
      case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
        inputAspect = reinterpret_cast<const VkRenderPassInputAttachmentAspectCreateInfo*>(base);
        break;
      default:
        return fail("unsupported pNext structure on vkCreateRenderPass");
    }
  }

  // Attachments.
  out.attachments.resize(in.attachmentCount);
  for (uint32_t i = 0; i < in.attachmentCount; ++i) {
    const VkAttachmentDescription& a = in.pAttachments[i];
    VkAttachmentDescription2& d = out.attachments[i];
    d = VkAttachmentDescription2{};
    d.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    d.pNext = nullptr;
    d.flags = a.flags;
    d.format = a.format;
    d.samples = a.samples;
    d.loadOp = a.loadOp;
    d.storeOp = a.storeOp;
    d.stencilLoadOp = a.stencilLoadOp;
    d.stencilStoreOp = a.stencilStoreOp;
    d.initialLayout = a.initialLayout;
    d.finalLayout = a.finalLayout;
  }

  // Subpasses.
  out.subpasses.resize(in.subpassCount);
  out.inputRefs.resize(in.subpassCount);
  out.colorRefs.resize(in.subpassCount);
  out.resolveRefs.resize(in.subpassCount);
  out.depthRefs.resize(in.subpassCount);
  out.preserveIndices.resize(in.subpassCount);

  for (uint32_t i = 0; i < in.subpassCount; ++i) {
    const VkSubpassDescription& s = in.pSubpasses[i];
    VkSubpassDescription2& d = out.subpasses[i];
    d = VkSubpassDescription2{};
    d.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    d.pNext = nullptr;
    d.flags = s.flags;
    d.pipelineBindPoint = s.pipelineBindPoint;
    d.viewMask = (multiview && multiview->pViewMasks && i < multiview->subpassCount)
                     ? multiview->pViewMasks[i]
                     : 0;

    auto build = [&](const VkAttachmentReference* src, uint32_t count, bool input,
                     std::vector<VkAttachmentReference2>& dst) {
      dst.resize(count);
      for (uint32_t r = 0; r < count; ++r) {
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        if (input && src[r].attachment != VK_ATTACHMENT_UNUSED &&
            src[r].attachment < in.attachmentCount) {
          aspects = aspectsForFormat(in.pAttachments[src[r].attachment].format);
          if (inputAspect) {
            for (uint32_t k = 0; k < inputAspect->aspectReferenceCount; ++k) {
              const VkInputAttachmentAspectReference& ref = inputAspect->pAspectReferences[k];
              if (ref.subpass == i && ref.inputAttachmentIndex == r) {
                aspects = ref.aspectMask;
                break;
              }
            }
          }
        }
        dst[r] = makeRef(src[r], input, aspects);
      }
    };

    build(s.pInputAttachments, s.inputAttachmentCount, true, out.inputRefs[i]);
    build(s.pColorAttachments, s.colorAttachmentCount, false, out.colorRefs[i]);
    build(s.pResolveAttachments, s.pResolveAttachments ? s.colorAttachmentCount : 0u, false,
          out.resolveRefs[i]);
    build(s.pDepthStencilAttachment, s.pDepthStencilAttachment ? 1u : 0u, false, out.depthRefs[i]);

    out.preserveIndices[i].assign(s.pPreserveAttachments,
                                  s.pPreserveAttachments + s.preserveAttachmentCount);

    d.inputAttachmentCount = (uint32_t)out.inputRefs[i].size();
    d.pInputAttachments = d.inputAttachmentCount ? out.inputRefs[i].data() : nullptr;
    d.colorAttachmentCount = (uint32_t)out.colorRefs[i].size();
    d.pColorAttachments = d.colorAttachmentCount ? out.colorRefs[i].data() : nullptr;
    d.pResolveAttachments = out.resolveRefs[i].empty() ? nullptr : out.resolveRefs[i].data();
    d.pDepthStencilAttachment = out.depthRefs[i].empty() ? nullptr : out.depthRefs[i].data();
    d.preserveAttachmentCount = (uint32_t)out.preserveIndices[i].size();
    d.pPreserveAttachments = d.preserveAttachmentCount ? out.preserveIndices[i].data() : nullptr;
  }

  // Dependencies.
  out.dependencies.resize(in.dependencyCount);
  for (uint32_t i = 0; i < in.dependencyCount; ++i) {
    const VkSubpassDependency& s = in.pDependencies[i];
    VkSubpassDependency2& d = out.dependencies[i];
    d = VkSubpassDependency2{};
    d.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    d.pNext = nullptr;
    d.srcSubpass = s.srcSubpass;
    d.dstSubpass = s.dstSubpass;
    d.srcStageMask = s.srcStageMask;
    d.dstStageMask = s.dstStageMask;
    d.srcAccessMask = s.srcAccessMask;
    d.dstAccessMask = s.dstAccessMask;
    d.dependencyFlags = s.dependencyFlags;
    d.viewOffset = (multiview && multiview->pViewOffsets && i < multiview->dependencyCount)
                       ? multiview->pViewOffsets[i]
                       : 0;
  }

  if (multiview && multiview->pCorrelationMasks && multiview->correlationMaskCount) {
    out.correlatedViewMasks.assign(multiview->pCorrelationMasks,
                                   multiview->pCorrelationMasks + multiview->correlationMaskCount);
  }

  out.info = VkRenderPassCreateInfo2{};
  out.info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
  out.info.pNext = nullptr;
  out.info.flags = in.flags;
  out.info.attachmentCount = (uint32_t)out.attachments.size();
  out.info.pAttachments = out.attachments.empty() ? nullptr : out.attachments.data();
  out.info.subpassCount = (uint32_t)out.subpasses.size();
  out.info.pSubpasses = out.subpasses.empty() ? nullptr : out.subpasses.data();
  out.info.dependencyCount = (uint32_t)out.dependencies.size();
  out.info.pDependencies = out.dependencies.empty() ? nullptr : out.dependencies.data();
  out.info.correlatedViewMaskCount = (uint32_t)out.correlatedViewMasks.size();
  out.info.pCorrelatedViewMasks = out.correlatedViewMasks.empty() ? nullptr : out.correlatedViewMasks.data();
  return true;
}

}  // namespace framefov
