#include "test_framework.h"

#include "common/renderpass_convert.h"

#include <cstring>

using namespace framefov;

namespace {

VkAttachmentDescription makeColorAttachment() {
  VkAttachmentDescription a{};
  a.format = VK_FORMAT_B8G8R8A8_UNORM;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  return a;
}

VkAttachmentDescription makeDepthAttachment() {
  VkAttachmentDescription a{};
  a.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  return a;
}

}  // namespace

FRAMEFOV_TEST(renderpass_convert_basic) {
  VkAttachmentDescription atts[2] = {makeColorAttachment(), makeDepthAttachment()};

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;

  uint32_t viewMasks[1] = {0b11u};
  uint32_t correlation[1] = {0b11u};
  VkRenderPassMultiviewCreateInfo mv{};
  mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
  mv.subpassCount = 1;
  mv.pViewMasks = viewMasks;
  mv.correlationMaskCount = 1;
  mv.pCorrelationMasks = correlation;

  VkRenderPassCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.pNext = &mv;
  ci.attachmentCount = 2;
  ci.pAttachments = atts;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;

  ConvertedRenderPass out;
  std::string err;
  CHECK(convertRenderPass1to2(ci, out, &err));
  CHECK_EQ(out.info.sType, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);
  CHECK_EQ(out.info.attachmentCount, 2u);
  CHECK_EQ(out.attachments[0].sType, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2);
  CHECK_EQ((int)out.attachments[0].initialLayout, (int)VK_IMAGE_LAYOUT_UNDEFINED);
  CHECK_EQ(out.subpasses[0].viewMask, 0b11u);
  CHECK_EQ(out.subpasses[0].colorAttachmentCount, 1u);
  CHECK(out.subpasses[0].pColorAttachments == out.colorRefs[0].data());
  CHECK_EQ(out.colorRefs[0][0].attachment, 0u);
  CHECK_EQ((int)out.colorRefs[0][0].layout, (int)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  CHECK_EQ((int)out.colorRefs[0][0].aspectMask, 0);  // non-input -> 0
  CHECK_EQ(out.depthRefs[0][0].attachment, 1u);
  CHECK_EQ(out.correlatedViewMasks.size(), 1u);
  CHECK_EQ(out.correlatedViewMasks[0], 0b11u);
  CHECK(out.info.pSubpasses == out.subpasses.data());
  CHECK_EQ(out.info.subpassCount, 1u);
}

FRAMEFOV_TEST(renderpass_convert_input_aspect) {
  VkAttachmentDescription atts[1] = {makeDepthAttachment()};
  VkAttachmentReference inputRef{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.inputAttachmentCount = 1;
  sub.pInputAttachments = &inputRef;

  VkRenderPassCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.attachmentCount = 1;
  ci.pAttachments = atts;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;

  ConvertedRenderPass out;
  CHECK(convertRenderPass1to2(ci, out, nullptr));
  // Derived from the combined depth/stencil format.
  const int expected = (int)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  CHECK_EQ((int)out.inputRefs[0][0].aspectMask, expected);

  // An explicit VkRenderPassInputAttachmentAspectCreateInfo wins.
  VkInputAttachmentAspectReference aspectRef{};
  aspectRef.subpass = 0;
  aspectRef.inputAttachmentIndex = 0;
  aspectRef.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  VkRenderPassInputAttachmentAspectCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO;
  ia.aspectReferenceCount = 1;
  ia.pAspectReferences = &aspectRef;
  ci.pNext = &ia;
  ConvertedRenderPass out2;
  CHECK(convertRenderPass1to2(ci, out2, nullptr));
  CHECK_EQ((int)out2.inputRefs[0][0].aspectMask, (int)VK_IMAGE_ASPECT_STENCIL_BIT);
}

FRAMEFOV_TEST(renderpass_convert_aspects_for_format) {
  CHECK_EQ((int)aspectsForFormat(VK_FORMAT_R8G8B8A8_UNORM), (int)VK_IMAGE_ASPECT_COLOR_BIT);
  CHECK_EQ((int)aspectsForFormat(VK_FORMAT_D32_SFLOAT), (int)VK_IMAGE_ASPECT_DEPTH_BIT);
  CHECK_EQ((int)aspectsForFormat(VK_FORMAT_S8_UINT), (int)VK_IMAGE_ASPECT_STENCIL_BIT);
  CHECK_EQ((int)aspectsForFormat(VK_FORMAT_D24_UNORM_S8_UINT),
           (int)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
}

FRAMEFOV_TEST(renderpass_convert_rejects_unknown_pnext) {
  VkAttachmentDescription atts[1] = {makeColorAttachment()};
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;

  VkRenderPassFragmentDensityMapCreateInfoEXT fdm{};
  fdm.sType = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT;

  VkRenderPassCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.pNext = &fdm;
  ci.attachmentCount = 1;
  ci.pAttachments = atts;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;

  ConvertedRenderPass out;
  std::string err;
  CHECK(!convertRenderPass1to2(ci, out, &err));
  CHECK(!err.empty());
}

FRAMEFOV_TEST(renderpass_convert_preserve_and_dependencies) {
  VkAttachmentDescription atts[1] = {makeColorAttachment()};
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  uint32_t preserve[1] = {0};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.preserveAttachmentCount = 1;
  sub.pPreserveAttachments = preserve;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  int32_t viewOffsets[1] = {1};
  VkRenderPassMultiviewCreateInfo mv{};
  mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
  mv.subpassCount = 1;
  mv.dependencyCount = 1;
  mv.pViewOffsets = viewOffsets;
  // pViewMasks left null: viewMask must be 0.

  VkRenderPassCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.pNext = &mv;
  ci.attachmentCount = 1;
  ci.pAttachments = atts;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 1;
  ci.pDependencies = &dep;

  ConvertedRenderPass out;
  CHECK(convertRenderPass1to2(ci, out, nullptr));
  CHECK_EQ(out.preserveIndices[0].size(), 1u);
  CHECK_EQ(out.subpasses[0].pPreserveAttachments, out.preserveIndices[0].data());
  CHECK_EQ(out.dependencies.size(), 1u);
  CHECK_EQ(out.dependencies[0].sType, VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2);
  CHECK_EQ(out.dependencies[0].viewOffset, 1);
  CHECK_EQ(out.dependencies[0].srcSubpass, VK_SUBPASS_EXTERNAL);
  CHECK_EQ(out.subpasses[0].viewMask, 0u);
}
