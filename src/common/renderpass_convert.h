// vkCreateRenderPass (v1) -> VkRenderPassCreateInfo2 conversion with owned
// storage. Pure (no Vulkan calls), unit tested (05 §1.1).
#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace framefov {

struct ConvertedRenderPass {
  std::vector<VkAttachmentDescription2> attachments;
  std::vector<VkSubpassDescription2> subpasses;
  std::vector<std::vector<VkAttachmentReference2>> inputRefs;
  std::vector<std::vector<VkAttachmentReference2>> colorRefs;
  std::vector<std::vector<VkAttachmentReference2>> resolveRefs;
  std::vector<std::vector<VkAttachmentReference2>> depthRefs;
  std::vector<std::vector<uint32_t>> preserveIndices;
  std::vector<VkSubpassDependency2> dependencies;
  std::vector<uint32_t> correlatedViewMasks;
  VkRenderPassCreateInfo2 info{};
};

// Returns false (with `error` set) when the v1 create info uses a pNext struct
// we cannot convert; the caller then passes the v1 info through unchanged.
bool convertRenderPass1to2(const VkRenderPassCreateInfo& in, ConvertedRenderPass& out,
                           std::string* error);

VkImageAspectFlags aspectsForFormat(VkFormat format);

}  // namespace framefov
