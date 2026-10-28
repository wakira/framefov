// Headless GPU end-to-end test for the framefov Vulkan layer.
//
// The program renders a fullscreen triangle with a fragment shader that writes
// the fragment shading rate it observed, then reads the result back and checks
// that the foveation pattern really was applied.
//
// The layer is loaded implicitly through the environment, for example:
//   FRAMEFOV_ENABLE=1 FRAMEFOV_FORCE_ACTIVE=1 FRAMEFOV_FAKE_EYE=1024x1024 ./vk_vrs_test <mode>
//
// Modes: dynamic, dynamic-chain, rp1, rp2, multiview, imageless, nonEyeSize.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "shaders_spv.h"

namespace {

[[noreturn]] void fatal(const char* what, VkResult r) {
  fprintf(stderr, "FATAL: %s failed (VkResult %d)\n", what, (int)r);
  exit(2);
}

#define VK_CHECK(expr)                        \
  do {                                        \
    VkResult framefov_r = (expr);                \
    if (framefov_r != VK_SUCCESS) fatal(#expr, framefov_r); \
  } while (0)

struct Options {
  std::string mode = "dynamic";
  bool chainFeatures = false;
  bool verbose = false;
  bool validation = true;
  bool api12 = false;   // request Vulkan 1.2 + VK_KHR_dynamic_rendering
  bool coverage = false;  // use the gl_FragCoord coverage probe instead of gl_ShadingRateEXT
  uint32_t frames = 1;    // render this many frames (for hot-reload experiments)
};

struct Device {
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  bool dynamicRendering = false;
  bool multiview = false;
  bool imageless = false;
  bool fragmentShadingRate = false;
  VkExtent2D texel{8, 8};
};

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
  for (const auto& e : list) {
    if (strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice gpu) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> list(count);
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, list.data());
  return list;
}

struct Instance {
  VkInstance instance = VK_NULL_HANDLE;
};

Instance createInstance(const Options& opt) {
  Instance out;
  uint32_t layerCount = 0;
  vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
  std::vector<VkLayerProperties> layers(layerCount);
  vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
  std::vector<const char*> enabled;
  for (const auto& l : layers) {
    if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0 && opt.validation) {
      enabled.push_back("VK_LAYER_KHRONOS_validation");
    }
  }
  if (!enabled.empty()) printf("validation layer enabled\n");

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "framefov vk_vrs_test";
  app.applicationVersion = 1;
  app.pEngineName = "none";
  app.apiVersion = opt.api12 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = (uint32_t)enabled.size();
  ci.ppEnabledLayerNames = enabled.data();
  VK_CHECK(vkCreateInstance(&ci, nullptr, &out.instance));
  return out;
}

bool gpuSuitable(VkPhysicalDevice gpu, const std::string& mode, Device* info) {
  auto exts = deviceExtensions(gpu);
  if (!hasExtension(exts, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) return false;

  VkPhysicalDeviceFragmentShadingRatePropertiesKHR rateProps{};
  rateProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &rateProps;
  vkGetPhysicalDeviceProperties2(gpu, &props2);

  bool dynamic = true, multiview = true, imageless = true;
  {
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    multiview = f11.multiview == VK_TRUE;
    imageless = f12.imagelessFramebuffer == VK_TRUE;
  }
  VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  dyn.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &dyn;
  vkGetPhysicalDeviceFeatures2(gpu, &f2);
  dynamic = dynamic && (dyn.dynamicRendering == VK_TRUE || f13.dynamicRendering == VK_TRUE);

  if (info) {
    info->dynamicRendering = dynamic;
    info->multiview = multiview;
    info->imageless = imageless;
    info->fragmentShadingRate = true;
    info->texel = rateProps.minFragmentShadingRateAttachmentTexelSize.width
                      ? VkExtent2D{std::max(rateProps.minFragmentShadingRateAttachmentTexelSize.width,
                                            rateProps.minFragmentShadingRateAttachmentTexelSize.height),
                                   std::max(rateProps.minFragmentShadingRateAttachmentTexelSize.width,
                                            rateProps.minFragmentShadingRateAttachmentTexelSize.height)}
                      : VkExtent2D{1, 1};
  }
  if (mode == "multiview" && !multiview) return false;
  if (mode == "imageless" && !imageless) return false;
  if ((mode == "dynamic" || mode == "dynamic-chain") && !dynamic) return false;
  return true;
}

Device createDevice(VkInstance instance, const Options& opt) {
  Device out;
  uint32_t gpuCount = 0;
  vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
  if (!gpuCount) {
    fprintf(stderr, "FATAL: no Vulkan physical devices\n");
    exit(2);
  }
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());

  for (VkPhysicalDevice gpu : gpus) {
    Device info;
    if (!gpuSuitable(gpu, opt.mode, &info)) continue;
    out = info;
    out.gpu = gpu;
    break;
  }
  if (out.gpu == VK_NULL_HANDLE) {
    fprintf(stderr, "FATAL: no suitable Vulkan device (needs VK_KHR_fragment_shading_rate)\n");
    exit(2);
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(out.gpu, &props);
  printf("GPU: %s (API %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

  uint32_t qfCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(out.gpu, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(out.gpu, &qfCount, qfs.data());
  out.queueFamily = UINT32_MAX;
  for (uint32_t i = 0; i < qfCount; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      out.queueFamily = i;
      break;
    }
  }
  if (out.queueFamily == UINT32_MAX) {
    fprintf(stderr, "FATAL: no graphics queue\n");
    exit(2);
  }

  std::vector<const char*> extensions = {VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME};
  if ((opt.mode == "dynamic" || opt.mode == "dynamic-chain") && opt.api12) {
    extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  }

  // The fragment shader reads gl_ShadingRateEXT.
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
  fsr.pipelineFragmentShadingRate = VK_TRUE;
  if (!opt.chainFeatures) {
    // No app-supplied FSR feature struct: the layer must add its own.
    fsr.pNext = nullptr;
  }

  VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
  dyn.dynamicRendering = VK_TRUE;

  VkPhysicalDeviceVulkan11Features f11{};
  f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  f11.multiview = VK_TRUE;

  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.imagelessFramebuffer = VK_TRUE;

  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = VK_TRUE;

  const bool needsDynamic = (opt.mode == "dynamic" || opt.mode == "dynamic-chain");
  void* head = nullptr;
  if (needsDynamic && !opt.api12) {
    f13.pNext = head;
    head = &f13;
  }
  if (needsDynamic && opt.api12) {
    dyn.pNext = head;
    head = &dyn;
  }
  f11.pNext = head;
  head = &f11;
  f12.pNext = head;
  head = &f12;
  fsr.pNext = head;
  head = &fsr;

  VkDeviceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  ci.pNext = head;
  ci.enabledExtensionCount = (uint32_t)extensions.size();
  ci.ppEnabledExtensionNames = extensions.data();

  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = out.queueFamily;
  qci.queueCount = 1;
  float priority = 1.0f;
  qci.pQueuePriorities = &priority;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &qci;

  VK_CHECK(vkCreateDevice(out.gpu, &ci, nullptr, &out.device));
  return out;
}

uint32_t findMemoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  fprintf(stderr, "FATAL: no memory type (bits 0x%x, flags 0x%x)\n", bits, want);
  exit(2);
}

struct Target {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0, layers = 1;
};

Target createTarget(const Device& dev, uint32_t w, uint32_t h, uint32_t layers) {
  Target t;
  t.width = w;
  t.height = h;
  t.layers = layers;

  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8_UINT;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(dev.device, &ci, nullptr, &t.image));

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(dev.device, t.image, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = findMemoryType(dev.gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(dev.device, &ai, nullptr, &t.memory));
  VK_CHECK(vkBindImageMemory(dev.device, t.image, t.memory, 0));

  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = t.image;
  vi.viewType = layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R8_UINT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  VK_CHECK(vkCreateImageView(dev.device, &vi, nullptr, &t.view));
  return t;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
};

Buffer createReadbackBuffer(const Device& dev, VkDeviceSize size) {
  Buffer b;
  VkBufferCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  ci.size = size;
  ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(dev.device, &ci, nullptr, &b.buffer));
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev.device, b.buffer, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex =
      findMemoryType(dev.gpu, req.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_CHECK(vkAllocateMemory(dev.device, &ai, nullptr, &b.memory));
  VK_CHECK(vkBindBufferMemory(dev.device, b.buffer, b.memory, 0));
  VK_CHECK(vkMapMemory(dev.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
  return b;
}

struct Pipeline {
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
};

VkShaderModule makeModule(const Device& dev, const uint32_t* code, size_t size) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = size;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(dev.device, &ci, nullptr, &m));
  return m;
}

Pipeline createPipeline(const Device& dev, VkRenderPass renderPass, VkPipelineLayout layout,
                        uint32_t viewMask, uint32_t colorAttachmentCount, bool coverage) {
  Pipeline p;
  p.vert = makeModule(dev, kFullscreenVertSpv, kFullscreenVertSpvSize);
  p.frag = coverage ? makeModule(dev, kShadingRateCoverageFragSpv, kShadingRateCoverageFragSpvSize)
                    : makeModule(dev, kShadingRateFragSpv, kShadingRateFragSpvSize);
  p.layout = layout;

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = p.vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = p.frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkViewport viewport{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};  // dummy, dynamic
  VkRect2D scissor{{0, 0}, {0, 0}};
  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.pViewports = &viewport;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  std::vector<VkPipelineColorBlendAttachmentState> blends(colorAttachmentCount, blend);
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = colorAttachmentCount;
  cb.pAttachments = blends.data();

  const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynStates;

  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = colorAttachmentCount;
  VkFormat format = VK_FORMAT_R8_UINT;
  rendering.pColorAttachmentFormats = &format;
  rendering.viewMask = viewMask;

  VkGraphicsPipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dyn;
  ci.layout = p.layout;
  ci.renderPass = renderPass;
  if (renderPass == VK_NULL_HANDLE) ci.pNext = &rendering;

  VK_CHECK(vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline));
  return p;
}

VkCommandBuffer beginCommands(const Device& dev, VkCommandPool pool) {
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(dev.device, &ai, &cmd));
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  return cmd;
}

void submitAndWait(const Device& dev, VkQueue queue, VkCommandBuffer cmd) {
  (void)dev;
  VK_CHECK(vkEndCommandBuffer(cmd));
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));
}

// ---------------------------------------------------------------------------
// Result analysis
// ---------------------------------------------------------------------------

struct Report {
  bool ok = true;
  std::vector<uint8_t> map;  // per rate-image texel
  uint32_t texelsW = 0, texelsH = 0;
  std::set<int> distinct;
  int center = -1;
  int corner = -1;
};

Report analyze(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t texel, bool expectNeutral,
               bool expectFoveaAtCenter) {
  Report r;
  r.texelsW = (w + texel - 1) / texel;
  r.texelsH = (h + texel - 1) / texel;
  r.map.resize((size_t)r.texelsW * r.texelsH);
  for (uint32_t ty = 0; ty < r.texelsH; ++ty) {
    for (uint32_t tx = 0; tx < r.texelsW; ++tx) {
      const uint32_t px = std::min(tx * texel + texel / 2, w - 1);
      const uint32_t py = std::min(ty * texel + texel / 2, h - 1);
      const uint8_t v = pixels[(size_t)py * w + px];
      r.map[(size_t)ty * r.texelsW + tx] = v;
      r.distinct.insert(v);
    }
  }
  r.center = r.map[(size_t)(r.texelsH / 2) * r.texelsW + r.texelsW / 2];
  r.corner = r.map[0];

  if (expectNeutral) {
    for (uint8_t v : r.map) {
      if (v != 0) {
        r.ok = false;
        break;
      }
    }
    return r;
  }

  if (expectFoveaAtCenter && r.center != 0) {  // inner rate is 1x1 in the default config
    fprintf(stderr, "  center texel code is %d, expected 0 (1x1)\n", r.center);
    r.ok = false;
  }
  if (expectFoveaAtCenter && r.corner <= 0) {
    fprintf(stderr, "  corner texel code is %d, expected a coarse rate\n", r.corner);
    r.ok = false;
  }
  if (r.distinct.size() > 3) {
    fprintf(stderr, "  %zu distinct rate codes, expected at most 3 (inner/middle/outer)\n",
            r.distinct.size());
    r.ok = false;
  }
  for (int v : r.distinct) {
    if (v != 0 && v != r.corner && v > r.corner) {
      fprintf(stderr, "  code %d is coarser than the corner code %d\n", v, r.corner);
      r.ok = false;
    }
  }
  return r;
}

void printMap(const Report& r) {
  printf("  rate map (%ux%u texels, codes: ", r.texelsW, r.texelsH);
  for (int v : r.distinct) printf("%d ", v);
  printf(")\n");
  const uint32_t stepY = std::max(1u, r.texelsH / 12);
  const uint32_t stepX = std::max(1u, r.texelsW / 48);
  for (uint32_t y = 0; y < r.texelsH; y += stepY) {
    printf("  ");
    for (uint32_t x = 0; x < r.texelsW; x += stepX) {
      const uint8_t v = r.map[(size_t)y * r.texelsW + x];
      putchar(v == 0 ? '.' : (char)('0' + std::min<int>(v, 9)));
    }
    putchar('\n');
  }
}

uint32_t codeToWidth(int code) { return 1u << ((code >> 2) & 3); }
uint32_t codeToHeight(int code) { return 1u << (code & 3); }

// Fraction of 2x2 pixel blocks in [x0,x1) x [y0,y1) whose four bytes are equal.
// A coarse shading rate makes the fragment shader cover multiple pixels, so its
// gl_FragCoord-derived output is constant over the block.
double blockiness2x2(const uint8_t* p, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                     uint32_t x1, uint32_t y1) {
  (void)h;
  uint64_t same = 0, total = 0;
  for (uint32_t y = y0; y + 1 < y1; y += 2) {
    for (uint32_t x = x0; x + 1 < x1; x += 2) {
      const uint8_t v = p[(size_t)y * w + x];
      ++total;
      if (p[(size_t)y * w + x + 1] == v && p[(size_t)(y + 1) * w + x] == v &&
          p[(size_t)(y + 1) * w + x + 1] == v) {
        ++same;
      }
    }
  }
  return total ? (double)same / (double)total : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--chain-features") {
      opt.chainFeatures = true;
    } else if (arg == "--api-1.2") {
      opt.api12 = true;
    } else if (arg == "--coverage") {
      opt.coverage = true;
    } else if (arg.rfind("--frames=", 0) == 0) {
      opt.frames = (uint32_t)std::max(1, atoi(arg.c_str() + 9));
    } else if (arg == "--verbose" || arg == "-v") {
      opt.verbose = true;
    } else if (arg == "--no-validation") {
      opt.validation = false;
    } else if (arg == "--help" || arg == "-h") {
      printf("usage: %s [mode] [--chain-features] [--api-1.2] [--coverage] [--frames=N] "
             "[--no-validation] [-v]\n", argv[0]);
      printf("modes: dynamic dynamic-chain rp1 rp2 multiview imageless nonEyeSize\n");
      return 0;
    } else if (arg.rfind("--", 0) == 0) {
      fprintf(stderr, "unknown option %s\n", arg.c_str());
      return 2;
    } else {
      opt.mode = arg;
    }
  }
  if (opt.mode == "dynamic-chain") opt.chainFeatures = true;

  printf("mode: %s\n", opt.mode.c_str());
  Instance inst = createInstance(opt);
  Device dev = createDevice(inst.instance, opt);
  printf("device capabilities: dynamicRendering=%d multiview=%d imageless=%d fsr=%d\n",
         (int)dev.dynamicRendering, (int)dev.multiview, (int)dev.imageless,
         (int)dev.fragmentShadingRate);

  const bool neutralExpected = (opt.mode == "nonEyeSize");
  const bool multiview = (opt.mode == "multiview");
  const bool dynamicMode = (opt.mode == "dynamic" || opt.mode == "dynamic-chain");
  const bool imageless = (opt.mode == "imageless");

  uint32_t width = neutralExpected ? 512 : 1024;
  uint32_t height = neutralExpected ? 256 : 1024;
  const uint32_t layers = multiview ? 2 : 1;

  Target target = createTarget(dev, width, height, layers);
  Buffer readback = createReadbackBuffer(dev, (VkDeviceSize)width * height * layers);

  VkCommandPool pool = VK_NULL_HANDLE;
  {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = dev.queueFamily;
    VK_CHECK(vkCreateCommandPool(dev.device, &ci, nullptr, &pool));
  }
  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev.device, dev.queueFamily, 0, &queue);

  // The core 1.3 names are only valid on a 1.3 device; with a 1.2 device the
  // KHR aliases are the legal spelling.
  PFN_vkCmdBeginRendering pfnBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(dev.device, "vkCmdBeginRendering"));
  PFN_vkCmdEndRendering pfnEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(dev.device, "vkCmdEndRendering"));
  if (!pfnBeginRendering) {
    pfnBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
        vkGetDeviceProcAddr(dev.device, "vkCmdBeginRenderingKHR"));
  }
  if (!pfnEndRendering) {
    pfnEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
        vkGetDeviceProcAddr(dev.device, "vkCmdEndRenderingKHR"));
  }
  if (dynamicMode && (!pfnBeginRendering || !pfnEndRendering)) {
    fprintf(stderr, "FATAL: no dynamic rendering entry points available\n");
    return 2;
  }

  VkRenderPass renderPass = VK_NULL_HANDLE;
  if (!dynamicMode) {
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8_UINT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    uint32_t viewMasks[1] = {multiview ? 0b11u : 0u};
    VkRenderPassMultiviewCreateInfo mv{};
    mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    if (multiview) {
      mv.subpassCount = 1;
      mv.pViewMasks = viewMasks;
    }

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.pNext = multiview ? (const void*)&mv : nullptr;
    ci.attachmentCount = 1;
    ci.pAttachments = &att;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    if (opt.mode == "rp2" || multiview) {
      VkRenderPassCreateInfo2 ci2{};
      ci2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
      ci2.attachmentCount = 1;
      VkAttachmentDescription2 att2{};
      att2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      att2.format = att.format;
      att2.samples = att.samples;
      att2.loadOp = att.loadOp;
      att2.storeOp = att.storeOp;
      att2.stencilLoadOp = att.stencilLoadOp;
      att2.stencilStoreOp = att.stencilStoreOp;
      att2.initialLayout = att.initialLayout;
      att2.finalLayout = att.finalLayout;
      ci2.pAttachments = &att2;
      VkAttachmentReference2 ref2{};
      ref2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
      ref2.attachment = 0;
      ref2.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkSubpassDescription2 sub2{};
      sub2.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
      sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub2.colorAttachmentCount = 1;
      sub2.pColorAttachments = &ref2;
      sub2.viewMask = multiview ? 0b11u : 0u;
      ci2.subpassCount = 1;
      ci2.pSubpasses = &sub2;
      VkSubpassDependency2 dep2{};
      dep2.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
      dep2.srcSubpass = VK_SUBPASS_EXTERNAL;
      dep2.dstSubpass = 0;
      dep2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dep2.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dep2.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      ci2.dependencyCount = 1;
      ci2.pDependencies = &dep2;
      VK_CHECK(vkCreateRenderPass2(dev.device, &ci2, nullptr, &renderPass));
    } else {
      VK_CHECK(vkCreateRenderPass(dev.device, &ci, nullptr, &renderPass));
    }
  }

  // Framebuffer (render pass modes).
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  if (!dynamicMode) {
    if (imageless) {
      VkFramebufferAttachmentImageInfo attInfo{};
      attInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;
      attInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      attInfo.width = width;
      attInfo.height = height;
      attInfo.layerCount = layers;
      VkFormat format = VK_FORMAT_R8_UINT;
      attInfo.viewFormatCount = 1;
      attInfo.pViewFormats = &format;
      VkFramebufferAttachmentsCreateInfo atts{};
      atts.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO;
      atts.attachmentImageInfoCount = 1;
      atts.pAttachmentImageInfos = &attInfo;

      VkFramebufferCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      ci.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
      ci.pNext = &atts;
      ci.renderPass = renderPass;
      ci.attachmentCount = 1;
      ci.width = width;
      ci.height = height;
      ci.layers = layers;
      VK_CHECK(vkCreateFramebuffer(dev.device, &ci, nullptr, &framebuffer));
    } else {
      VkFramebufferCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      ci.renderPass = renderPass;
      ci.attachmentCount = 1;
      ci.pAttachments = &target.view;
      ci.width = width;
      ci.height = height;
      ci.layers = layers;
      VK_CHECK(vkCreateFramebuffer(dev.device, &ci, nullptr, &framebuffer));
    }
  }

  VkPipelineLayoutCreateInfo lci{};
  lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VK_CHECK(vkCreatePipelineLayout(dev.device, &lci, nullptr, &layout));

  Pipeline pipeline = createPipeline(dev, renderPass, layout, multiview ? 0b11u : 0u, 1, opt.coverage);

  VkCommandBuffer cmd = VK_NULL_HANDLE;
  for (uint32_t frame = 0; frame < opt.frames; ++frame) {
    if (frame == 0) {
      cmd = beginCommands(dev, pool);
    } else {
      VkCommandBufferBeginInfo rebegin{};
      rebegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      rebegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_CHECK(vkBeginCommandBuffer(cmd, &rebegin));
    }
    VkViewport viewport{0.f, 0.f, (float)width, (float)height, 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

    // Dynamic viewport/scissor are set inside the render pass instance.
    if (dynamicMode) {
      VkRenderingAttachmentInfo color{};
      color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color.imageView = target.view;
      color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color.clearValue = {{{0, 0, 0, 0}}};
      VkRenderingInfo ri{};
      ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      ri.renderArea = {{0, 0}, {width, height}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &color;
      pfnBeginRendering(cmd, &ri);
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      pfnEndRendering(cmd);
    } else {
      VkClearValue clear{};
      VkRenderPassBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      bi.renderPass = renderPass;
      bi.framebuffer = framebuffer;
      bi.renderArea = {{0, 0}, {width, height}};
      bi.clearValueCount = 1;
      bi.pClearValues = &clear;
      VkRenderPassAttachmentBeginInfo attBegin{};
      if (imageless) {
        attBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
        attBegin.attachmentCount = 1;
        attBegin.pAttachments = &target.view;
        bi.pNext = &attBegin;
      }
      vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                           1, &copy);
    submitAndWait(dev, queue, cmd);
  }

  const uint8_t* pixels = static_cast<const uint8_t*>(readback.mapped);
  const uint32_t texel = dev.texel.width;
  Report report;

  if (opt.coverage) {
    // Asymmetric probe: the center must stay at full rate, the corners must be
    // coarse. Works even on drivers that do not report the applied rate.
    const uint32_t margin = 64;
    const double center =
        blockiness2x2(pixels, width, height, width / 2 - margin / 2, height / 2 - margin / 2,
                      width / 2 + margin / 2, height / 2 + margin / 2);
    double corners = 0;
    corners += blockiness2x2(pixels, width, height, 0, 0, margin, margin);
    corners += blockiness2x2(pixels, width, height, width - margin, 0, width, margin);
    corners += blockiness2x2(pixels, width, height, 0, height - margin, margin, height);
    corners += blockiness2x2(pixels, width, height, width - margin, height - margin, width, height);
    corners /= 4.0;
    printf("  coherence: center %.3f, corners %.3f (1.0 = coarse rate applied)\n", center, corners);
    if (neutralExpected) {
      // Expect full rate everywhere.
      report.ok = (center < 0.1 && corners < 0.1);
    } else {
      report.ok = (center < 0.1 && corners > 0.9);
    }
    if (multiview) {
      const uint8_t* layer1 = pixels + (size_t)width * height;
      if (memcmp(pixels, layer1, (size_t)width * height) != 0) {
        printf("  layer 1 differs from layer 0\n");
        report.ok = false;
      }
    }
  } else {
    const bool gazeInjected = getenv("FRAMEFOV_FAKE_GAZE") != nullptr;
    report = analyze(pixels, width, height, texel, neutralExpected, !gazeInjected);
    printMap(report);

    if (multiview) {
      const uint8_t* layer1 = pixels + (size_t)width * height;
      bool same = memcmp(pixels, layer1, (size_t)width * height) == 0;
      printf("  layer 1 %s layer 0\n", same ? "matches" : "differs from");
      if (!same) report.ok = false;
    }

    if (!neutralExpected) {
      printf("  center code %d = %ux%u, corner code %d = %ux%u\n", report.center,
             codeToWidth(report.center), codeToHeight(report.center), report.corner,
             codeToWidth(report.corner), codeToHeight(report.corner));
    }

    // draw_radii: the circles must show up as a rate that is neither the fovea
    // rate nor the outer rate, and there must be enough texels for a closed ring.
    if (getenv("FRAMEFOV_EXPECT_RING") && !neutralExpected) {
      uint64_t ringTexels = 0;
      int ringCode = -1;
      for (uint8_t v : report.map) {
        if (v != (uint8_t)report.center && v != (uint8_t)report.corner) {
          ++ringTexels;
          ringCode = v;
        }
      }
      printf("  radii circles: %llu texels with code %d (%ux%u), center %d, corner %d\n",
             (unsigned long long)ringTexels, ringCode,
             ringCode >= 0 ? codeToWidth(ringCode) : 0, ringCode >= 0 ? codeToHeight(ringCode) : 0,
             report.center, report.corner);
      if (ringTexels < 16) {
        printf("  expected a closed radii circle in the rate map\n");
        report.ok = false;
      }
    }

    // When a gaze point was injected, the full-rate blob must sit on it.
    const char* gazeEnv = getenv("FRAMEFOV_FAKE_GAZE");
    if (gazeEnv && !neutralExpected) {
      float gazeU = 0.5f, gazeV = 0.5f;
      if (sscanf(gazeEnv, "%f,%f", &gazeU, &gazeV) == 2) {
        double sumX = 0, sumY = 0;
        uint64_t count = 0;
        for (uint32_t ty = 0; ty < report.texelsH; ++ty) {
          for (uint32_t tx = 0; tx < report.texelsW; ++tx) {
            if (report.map[(size_t)ty * report.texelsW + tx] == 0) {
              sumX += ((double)tx + 0.5) / report.texelsW;
              sumY += ((double)ty + 0.5) / report.texelsH;
              ++count;
            }
          }
        }
        if (count == 0) {
          printf("  no full-rate texels: no fovea at all\n");
          report.ok = false;
        } else {
          const double cu = sumX / count, cv = sumY / count;
          printf("  fovea centroid (%.3f, %.3f), injected gaze (%.3f, %.3f)\n", cu, cv, gazeU,
                 gazeV);
          if (std::fabs(cu - gazeU) > 0.05 || std::fabs(cv - gazeV) > 0.05) {
            printf("  fovea does not follow the gaze\n");
            report.ok = false;
          }
        }
      }
    }
  }
  printf("%s: mode %s\n", report.ok ? "PASS" : "FAIL", opt.mode.c_str());

  // Teardown (the process exit is enough, but be tidy for the validation layer).
  vkDeviceWaitIdle(dev.device);
  vkDestroyPipeline(dev.device, pipeline.pipeline, nullptr);
  vkDestroyShaderModule(dev.device, pipeline.vert, nullptr);
  vkDestroyShaderModule(dev.device, pipeline.frag, nullptr);
  vkDestroyPipelineLayout(dev.device, layout, nullptr);
  if (framebuffer) vkDestroyFramebuffer(dev.device, framebuffer, nullptr);
  if (renderPass) vkDestroyRenderPass(dev.device, renderPass, nullptr);
  vkDestroyBuffer(dev.device, readback.buffer, nullptr);
  vkFreeMemory(dev.device, readback.memory, nullptr);
  vkDestroyImageView(dev.device, target.view, nullptr);
  vkDestroyImage(dev.device, target.image, nullptr);
  vkFreeMemory(dev.device, target.memory, nullptr);
  vkDestroyCommandPool(dev.device, pool, nullptr);
  vkDestroyDevice(dev.device, nullptr);
  vkDestroyInstance(inst.instance, nullptr);
  return report.ok ? 0 : 1;
}
