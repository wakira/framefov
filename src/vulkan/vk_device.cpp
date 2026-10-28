// Device creation / destruction, activation decision and capability detection.
#include "common/guard.h"
#include "common/gaze_shm.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"

#include <vulkan/vk_layer.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
using namespace framefov;

namespace framefov {
namespace {

const VkLayerDeviceCreateInfo* findLinkInfo(const void* pNext) {
  auto info = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pNext);
  while (info) {
    if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        info->function == VK_LAYER_LINK_INFO) {
      return info;
    }
    info = reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
  }
  return nullptr;
}

void resolveDeviceDispatch(DeviceData& data) {
  VkDevice dev = data.device;
  PFN_vkGetDeviceProcAddr gdpa = data.nextGdpa;
  DeviceDispatch& t = data.d;
  t.GetDeviceProcAddr = gdpa;

#define FRAMEFOV_RESOLVE(field, name) \
  t.field = gdpa ? reinterpret_cast<decltype(t.field)>(gdpa(dev, name)) : nullptr
  FRAMEFOV_RESOLVE(DestroyDevice, "vkDestroyDevice");
  FRAMEFOV_RESOLVE(CreateImage, "vkCreateImage");
  FRAMEFOV_RESOLVE(DestroyImage, "vkDestroyImage");
  FRAMEFOV_RESOLVE(CreateImageView, "vkCreateImageView");
  FRAMEFOV_RESOLVE(DestroyImageView, "vkDestroyImageView");
  FRAMEFOV_RESOLVE(CreateBuffer, "vkCreateBuffer");
  FRAMEFOV_RESOLVE(DestroyBuffer, "vkDestroyBuffer");
  FRAMEFOV_RESOLVE(AllocateMemory, "vkAllocateMemory");
  FRAMEFOV_RESOLVE(FreeMemory, "vkFreeMemory");
  FRAMEFOV_RESOLVE(BindImageMemory, "vkBindImageMemory");
  FRAMEFOV_RESOLVE(BindBufferMemory, "vkBindBufferMemory");
  FRAMEFOV_RESOLVE(MapMemory, "vkMapMemory");
  FRAMEFOV_RESOLVE(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
  FRAMEFOV_RESOLVE(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
  FRAMEFOV_RESOLVE(CreateRenderPass, "vkCreateRenderPass");
  FRAMEFOV_RESOLVE(CreateRenderPass2, "vkCreateRenderPass2");
  FRAMEFOV_RESOLVE(CreateRenderPass2KHR, "vkCreateRenderPass2KHR");
  FRAMEFOV_RESOLVE(DestroyRenderPass, "vkDestroyRenderPass");
  FRAMEFOV_RESOLVE(CreateFramebuffer, "vkCreateFramebuffer");
  FRAMEFOV_RESOLVE(DestroyFramebuffer, "vkDestroyFramebuffer");
  FRAMEFOV_RESOLVE(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
  FRAMEFOV_RESOLVE(CreateShaderModule, "vkCreateShaderModule");
  FRAMEFOV_RESOLVE(DestroyShaderModule, "vkDestroyShaderModule");
  FRAMEFOV_RESOLVE(BeginCommandBuffer, "vkBeginCommandBuffer");
  FRAMEFOV_RESOLVE(FreeCommandBuffers, "vkFreeCommandBuffers");
  FRAMEFOV_RESOLVE(CmdBeginRenderPass, "vkCmdBeginRenderPass");
  FRAMEFOV_RESOLVE(CmdBeginRenderPass2, "vkCmdBeginRenderPass2");
  FRAMEFOV_RESOLVE(CmdBeginRenderPass2KHR, "vkCmdBeginRenderPass2KHR");
  FRAMEFOV_RESOLVE(CmdBeginRendering, "vkCmdBeginRendering");
  FRAMEFOV_RESOLVE(CmdBeginRenderingKHR, "vkCmdBeginRenderingKHR");
  FRAMEFOV_RESOLVE(CmdPipelineBarrier, "vkCmdPipelineBarrier");
  FRAMEFOV_RESOLVE(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
  FRAMEFOV_RESOLVE(CmdClearColorImage, "vkCmdClearColorImage");
#undef FRAMEFOV_RESOLVE
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

template <class T>
T* findInChain(const void* head, VkStructureType sType) {
  auto node = reinterpret_cast<const VkBaseInStructure*>(head);
  while (node) {
    if (node->sType == sType) return const_cast<T*>(reinterpret_cast<const T*>(node));
    node = node->pNext;
  }
  return nullptr;
}

bool nameInList(uint32_t count, const char* const* names, const char* want) {
  if (!names) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (names[i] && strcmp(names[i], want) == 0) return true;
  }
  return false;
}

bool deviceExtensionEnabled(const VkDeviceCreateInfo* ci, const char* name) {
  return nameInList(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, name);
}

bool deviceExtensionAvailable(InstanceData* inst, VkPhysicalDevice gpu, const char* name) {
  if (!inst || !inst->d.EnumerateDeviceExtensionProperties) return false;
  uint32_t count = 0;
  if (inst->d.EnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> props(count);
  if (count &&
      inst->d.EnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data()) != VK_SUCCESS) {
    return false;
  }
  for (const auto& p : props) {
    if (strcmp(p.extensionName, name) == 0) return true;
  }
  return false;
}

uint32_t pow2Floor(uint32_t v) {
  if (v == 0) return 1;
  while (v & (v - 1)) v &= v - 1;
  return v;
}

Rate clampRate(const Rate& want, const std::vector<VkExtent2D>& supported) {
  Rate best{1, 1};
  bool first = true;
  for (const VkExtent2D& s : supported) {
    if (s.width == 0 || s.height == 0) continue;
    if (s.width > want.w || s.height > want.h) continue;
    if (first || (uint32_t)s.width * s.height > (uint32_t)best.w * best.h) {
      best.w = (uint8_t)s.width;
      best.h = (uint8_t)s.height;
      first = false;
    }
  }
  return best;
}

struct Activation {
  bool active = false;
  std::string reason;
};

// 04 §5 step 2: everything that can rule the device out before we touch the
// create info.
Activation evaluateActivation(DeviceData& dd, const VkDeviceCreateInfo* ci, const Config& cfg) {
  Activation a;

  if (!cfg.enabled) {
    a.reason = "disabled in config";
    return a;
  }
  // Diagnostic switch: the layer stays in the chain (loaded and negotiated) but
  // never activates, so "merely present" can be told apart from "active".
  if (envFlag("FRAMEFOV_DISABLE_VK")) {
    a.reason = "FRAMEFOV_DISABLE_VK";
    return a;
  }
  // No OpenXR instance is ever needed on Steam Frame: the layer is a plain
  // standalone foveation injector, activated as soon as the device is capable.
  InstanceData* inst = dd.instance;
  if (!inst || !inst->d.GetPhysicalDeviceFeatures2 || !inst->d.GetPhysicalDeviceProperties2) {
    a.reason = "instance dispatch incomplete";
    return a;
  }

  if (!deviceExtensionAvailable(inst, dd.physicalDevice, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
    a.reason = "VK_KHR_fragment_shading_rate not available";
    return a;
  }
  const bool renderPass2 = inst->apiVersion >= VK_API_VERSION_1_2 ||
                           deviceExtensionAvailable(inst, dd.physicalDevice,
                                                    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  if (!renderPass2) {
    a.reason = "no VK_KHR_create_renderpass2 and API < 1.2";
    return a;
  }
  if (!inst->d.GetPhysicalDeviceFragmentShadingRatesKHR) {
    a.reason = "vkGetPhysicalDeviceFragmentShadingRatesKHR not resolvable";
    return a;
  }

  VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &fsr;
  inst->d.GetPhysicalDeviceFeatures2(dd.physicalDevice, &features2);
  if (!fsr.pipelineFragmentShadingRate || !fsr.attachmentFragmentShadingRate) {
    a.reason = "pipeline/attachment fragment shading rate not supported";
    return a;
  }

  // Apps that already use NV shading rate image or EXT fragment density map must
  // be left alone: those are mutually exclusive with FSR.
  if (const auto* nv = findInChain<VkPhysicalDeviceShadingRateImageFeaturesNV>(
          ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV)) {
    if (nv->shadingRateImage == VK_TRUE) {
      a.reason = "app uses VK_NV_shading_rate_image";
      return a;
    }
  }
  if (const auto* fdm = findInChain<VkPhysicalDeviceFragmentDensityMapFeaturesEXT>(
          ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT)) {
    if (fdm->fragmentDensityMap == VK_TRUE) {
      a.reason = "app uses VK_EXT_fragment_density_map";
      return a;
    }
  }

  VkFormatProperties fmt{};
  if (inst->d.GetPhysicalDeviceFormatProperties) {
    inst->d.GetPhysicalDeviceFormatProperties(dd.physicalDevice, VK_FORMAT_R8_UINT, &fmt);
  }
  if (!(fmt.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
    a.reason = "R8_UINT is not a fragment shading rate attachment format";
    return a;
  }

  a.active = true;
  return a;
}

void fillCaps(DeviceData& dd, const VkDeviceCreateInfo* ci, const Config& cfg) {
  Caps& c = dd.caps;
  InstanceData* inst = dd.instance;

  VkPhysicalDeviceFragmentShadingRatePropertiesKHR props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &props;
  inst->d.GetPhysicalDeviceProperties2(dd.physicalDevice, &props2);

  c.minTexel = props.minFragmentShadingRateAttachmentTexelSize;
  c.maxTexel = props.maxFragmentShadingRateAttachmentTexelSize;
  c.maxTexelAspect = std::max(1u, props.maxFragmentShadingRateAttachmentTexelSizeAspectRatio);
  c.layeredAttachments = props.layeredShadingRateAttachments == VK_TRUE;
  c.nonTrivialCombiners = props.fragmentShadingRateNonTrivialCombinerOps == VK_TRUE;
  c.withDepthStencilWrites = props.fragmentShadingRateWithShaderDepthStencilWrites == VK_TRUE;
  c.withShaderSampleMask = props.fragmentShadingRateWithShaderSampleMask == VK_TRUE;
  c.withSampleMask = props.fragmentShadingRateWithSampleMask == VK_TRUE;
  if (c.minTexel.width == 0 || c.minTexel.height == 0) c.minTexel = VkExtent2D{1, 1};
  if (c.maxTexel.width == 0 || c.maxTexel.height == 0) c.maxTexel = VkExtent2D{1, 1};

  uint32_t count = 0;
  inst->d.GetPhysicalDeviceFragmentShadingRatesKHR(dd.physicalDevice, &count, nullptr);
  std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count);
  for (auto& r : rates) {
    r.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
  }
  if (count) {
    inst->d.GetPhysicalDeviceFragmentShadingRatesKHR(dd.physicalDevice, &count, rates.data());
  }
  for (const auto& r : rates) {
    if (!(r.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) continue;
    c.supportedRates.push_back(r.fragmentSize);
  }
  if (c.supportedRates.empty()) c.supportedRates.push_back(VkExtent2D{1, 1});

  // Square texels, power of two, inside the device range; 16 preferred.
  uint32_t lo = std::max(c.minTexel.width, c.minTexel.height);
  uint32_t hi = std::min(c.maxTexel.width, c.maxTexel.height);
  if (hi < lo) hi = lo;
  uint32_t want = cfg.texelSize ? cfg.texelSize : 16;
  want = std::min(std::max(want, lo), hi);
  c.texel = std::max(1u, pow2Floor(want));

  c.innerRate = clampRate(cfg.innerRate, c.supportedRates);
  c.middleRate = clampRate(cfg.middleRate, c.supportedRates);
  c.outerRate = clampRate(cfg.outerRate, c.supportedRates);

  VkPhysicalDeviceMemoryProperties mem{};
  if (inst->d.GetPhysicalDeviceMemoryProperties) {
    inst->d.GetPhysicalDeviceMemoryProperties(dd.physicalDevice, &mem);
  }
  c.memProps = mem;
  const VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  const VkMemoryPropertyFlags hostCoherent =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
    if ((f & deviceLocal) == deviceLocal) {
      c.memoryTypeDeviceLocal = i;
      break;
    }
  }
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
    if ((f & hostCoherent) == hostCoherent) {
      c.memoryTypeHostVisibleCoherent = i;
      break;
    }
  }

  c.dynamicRenderingAvailable =
      dd.instance->apiVersion >= VK_API_VERSION_1_3 ||
      deviceExtensionAvailable(inst, dd.physicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
      deviceExtensionEnabled(ci, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
}

// Enables pipeline+attachment fragment shading rate on the create info: patch the
// app's feature struct in place when it exists (never duplicate an sType), else
// prepend our own. Restores/undoes on destruction.
class FsrFeaturePatch {
 public:
  void apply(VkDeviceCreateInfo& ci) {
    auto* existing = findInChain<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(
        ci.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
    if (existing) {
      patched_ = existing;
      savedPipeline_ = existing->pipelineFragmentShadingRate;
      savedAttachment_ = existing->attachmentFragmentShadingRate;
      existing->pipelineFragmentShadingRate = VK_TRUE;
      existing->attachmentFragmentShadingRate = VK_TRUE;
      mode_ = Mode::Patch;
    } else {
      ours_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
      ours_.pNext = const_cast<void*>(ci.pNext);
      ours_.pipelineFragmentShadingRate = VK_TRUE;
      ours_.attachmentFragmentShadingRate = VK_TRUE;
      ours_.primitiveFragmentShadingRate = VK_FALSE;
      ci.pNext = &ours_;
      mode_ = Mode::Prepend;
    }
  }

  void restore(VkDeviceCreateInfo& ci) {
    if (mode_ == Mode::Patch && patched_) {
      patched_->pipelineFragmentShadingRate = savedPipeline_;
      patched_->attachmentFragmentShadingRate = savedAttachment_;
    } else if (mode_ == Mode::Prepend) {
      ci.pNext = ours_.pNext;
    }    mode_ = Mode::None;
  }

  ~FsrFeaturePatch() {
    if (mode_ == Mode::Patch && patched_) {
      patched_->pipelineFragmentShadingRate = savedPipeline_;
      patched_->attachmentFragmentShadingRate = savedAttachment_;
    }
  }

 private:
  enum class Mode { None, Patch, Prepend };
  Mode mode_ = Mode::None;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR* patched_ = nullptr;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR ours_{};
  VkBool32 savedPipeline_ = VK_FALSE;
  VkBool32 savedAttachment_ = VK_FALSE;
};

// Standalone / debug: synthesize the snapshot the OpenXR half would publish.
//   standalone = 1        fixed foveation for a Vulkan app without OpenXR
//   FRAMEFOV_FAKE_EYE=WxH   (debug) override the recommended eye size
//   FRAMEFOV_FAKE_GAZE=u,v  (debug) override the gaze point
void installSyntheticSnapshot(DeviceData& data, const Config& cfg) {
  (void)data;  // the snapshot describes the whole process, not one device
  const char* spec = getenv("FRAMEFOV_FAKE_EYE");
  const bool haveFakeEye = spec && *spec;
  if (!haveFakeEye && !cfg.standalone) return;

  uint32_t w = cfg.eyeWidth, h = cfg.eyeHeight;
  if (haveFakeEye && (sscanf(spec, "%ux%u", &w, &h) != 2 || w == 0 || h == 0)) {
    LOG_WARN("FRAMEFOV_FAKE_EYE='%s' is not WxH; ignoring", spec);
    w = cfg.eyeWidth;
    h = cfg.eyeHeight;
  }

  float u = 0.5f, v = 0.5f;
  bool haveGaze = false;
  if (const char* gaze = getenv("FRAMEFOV_FAKE_GAZE")) {
    if (sscanf(gaze, "%f,%f", &u, &v) == 2) {
      haveGaze = true;
    } else {
      LOG_WARN("FRAMEFOV_FAKE_GAZE='%s' is not u,v; using the center", gaze);
    }
  }
  if (!haveGaze) {
    // Fixed-mode gaze: the centre plus the configured offsets. The OpenXR half
    // maps them through the session FOV; here the 90 degree convention is used
    // (fixed_offset_x = 0.5 -> u = 0.75).
    u = std::min(1.f, std::max(0.f, 0.5f + 0.5f * cfg.fixedOffsetX));
    v = std::min(1.f, std::max(0.f, 0.5f - 0.5f * cfg.fixedOffsetY));
  }

  auto config = std::make_shared<const Config>(cfg);
  SharedState::get().update([&](Snapshot& snap) {
    snap.active = true;
    snap.eyeW = w;
    snap.eyeH = h;
    snap.eyes[0] = EyeView{true, u, v, false};
    snap.eyes[1] = EyeView{true, u, v, false};
    snap.config = config;
    snap.publishTimeNs = nowNs();
    snap.synthetic = true;
    snap.anyDevice = true;
  });

  LOG_INFO("synthetic snapshot (%s): recommended %ux%u, gaze (%.3f, %.3f), debugMode %d",
           haveFakeEye ? "FRAMEFOV_FAKE_EYE" : "standalone mode", w, h, u, v, cfg.debugMode);
  LOG_INFO("config (file + environment): %s", configSummary(cfg).c_str());
  if (w == 0 || h == 0) {
    LOG_WARN("no usable eye size: every colour target that passes skip_depth_only will be "
             "foveated at the centre (set eye_size = WxH, or FRAMEFOV_FAKE_EYE=WxH)");
  }
}

// Standalone mode has no OpenXR half to hot reload the config, so the Vulkan half
// does it while recording (throttled to once per second by the caller).
void reloadStandaloneConfig(DeviceData& data) {
  const int64_t now = nowNs();
  if (now - data.lastConfigCheckNs < 1000LL * 1000 * 1000) return;
  data.lastConfigCheckNs = now;
  const int64_t mtime = fileMtimeNs(data.configPath);
  if (mtime == 0 || mtime == data.configMtime) return;
  data.configMtime = mtime;

  const uint64_t generation = data.localConfig.generation + 1;
  Config reloaded = loadConfigForProcess(std::string(), generation, data.configPath);
  data.localConfig = reloaded;
  LOG_INFO("standalone config reloaded (generation %llu): %s", (unsigned long long)generation,
           configSummary(reloaded).c_str());
  if (!reloaded.standalone) {
    LOG_INFO("standalone mode turned off; foveation stops");
    SharedState::get().update([&](Snapshot& snap) {
      snap.active = false;
      snap.synthetic = false;
      snap.anyDevice = false;
      snap.publishTimeNs = nowNs();
    });
    data.standalone = false;
    return;
  }
  installSyntheticSnapshot(data, reloaded);
}

// Standalone gaze comes from the out-of-process publisher (framefov-gaze), which
// connects to SteamVR and writes /dev/shm/framefov-gaze. Nothing in this process
// talks to SteamVR: VR_InitInternal() here blocked the render thread (black
// headset, audio still playing) and competed with Proton's own VR client.
//
// Polled at ~60 Hz with optional EMA (`smoothing`). Stale or absent data keeps
// the last centre; no data at all leaves the fixed centre in place.
void updateStandaloneGaze(DeviceData& data) {
  if (!envFlag("FRAMEFOV_OPENVR_GAZE")) return;
  const int64_t now = nowNs();

  // Tell the publisher a game is rendering. It stays disconnected while this is
  // stale, which keeps it out of the boot ordering race entirely.
  {
    static int64_t lastWantNs = 0;
    if (now - lastWantNs > 1000LL * 1000 * 1000) {
      lastWantNs = now;
      int want = open(kGazeWantPath, O_WRONLY | O_CREAT, 0600);
      if (want >= 0) {
        const char c = '1';
        (void)!write(want, &c, 1);
        close(want);
      }
    }
  }
  // The publisher samples at a fixed 60 Hz; the layer only has to not miss a
  // published sample. A 4 ms floor plus the seqlock seq check below means one
  // snapshot per sample, however often a frame records command buffers.
  constexpr int64_t kPollFloorNs = 4LL * 1000 * 1000;
  if (now - data.lastGazeCheckNs < kPollFloorNs) return;
  data.lastGazeCheckNs = now;

  static int fd = -1;
  static const GazeShm* map = nullptr;
  static int64_t lastOpenAttempt = 0;
  static bool have = false;
  static float lu = 0.5f, lv = 0.5f, ru = 0.5f, rv = 0.5f;
  static float lastLoggedU = -1.f, lastLoggedV = -1.f;
  static int64_t lastLogNs = 0;

  if (!map) {
    if (now - lastOpenAttempt < 1000LL * 1000 * 1000) return;
    lastOpenAttempt = now;
    fd = shm_open(kGazeShmName, O_RDONLY, 0);
    if (fd >= 0) {
      void* p = mmap(nullptr, sizeof(GazeShm), PROT_READ, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED) {
        map = static_cast<const GazeShm*>(p);
        LOG_INFO("standalone gaze: publisher found (%s)", kGazeShmName);
      } else {
        close(fd);
        fd = -1;
      }
    }
    if (!map) {
      LOG_ONCE(INFO, "standalone-gaze-no-publisher",
               "standalone gaze: no publisher at %s (start framefov-gaze); fixed foveation",
               kGazeShmName);
      return;
    }
  }

  static uint64_t lastSeq = 0;
  GazeShm rec{};
  if (!gazeShmRead(map, rec) || !rec.valid || now - rec.monoNs > kGazeStaleNs) {
    LOG_ONCE(DEBUG, "standalone-gaze-no-data",
             "standalone gaze: no fresh sample; keeping the last centre");
    return;
  }
  // One update per published sample, not per command buffer: keeps the smoothing
  // frame-rate independent and avoids allocating a snapshot for a stale sample.
  if (rec.seq == lastSeq) return;
  lastSeq = rec.seq;

  // Some stacks compose the eye image flipped vertically relative to the gaze
  // convention; gaze_yflip mirrors v to match the image the rate map is applied to.
  float gu0 = rec.u0, gv0 = rec.v0, gu1 = rec.u1, gv1 = rec.v1;
  if (data.localConfig.gazeYFlip) {
    gv0 = 1.f - gv0;
    gv1 = 1.f - gv1;
  }

  float s = data.localConfig.smoothing;
  if (s < 0.f) s = 0.f;
  if (s > 0.99f) s = 0.99f;
  if (!have) {
    lu = gu0, lv = gv0, ru = gu1, rv = gv1;
    have = true;
  } else {
    lu = s * lu + (1.f - s) * gu0;
    lv = s * lv + (1.f - s) * gv0;
    ru = s * ru + (1.f - s) * gu1;
    rv = s * rv + (1.f - s) * gv1;
  }

  SharedState::get().update([&](Snapshot& snap) {
    snap.eyes[0] = EyeView{true, lu, lv, true};
    snap.eyes[1] = EyeView{true, ru, rv, true};
    snap.publishTimeNs = nowNs();
  });
  Stats& stats = SharedState::get().stats;
  stats.eyeTracked.store(1);
  stats.gazeU.store(lu);
  stats.gazeV.store(lv);

  LOG_ONCE(INFO, "standalone-gaze-active",
           "standalone gaze: eye tracking active (publisher), centre left (%.3f, %.3f) "
           "right (%.3f, %.3f)",
           (double)lu, (double)lv, (double)ru, (double)rv);

  const float du = (lu > lastLoggedU ? lu - lastLoggedU : lastLoggedU - lu);
  const float dv = (lv > lastLoggedV ? lv - lastLoggedV : lastLoggedV - lv);
  if (now - lastLogNs >= 1000LL * 1000 * 1000 && (du > 0.03f || dv > 0.03f)) {
    lastLogNs = now;
    lastLoggedU = lu;
    lastLoggedV = lv;
    LOG_INFO("standalone gaze: left (%.3f, %.3f) right (%.3f, %.3f)", (double)lu, (double)lv,
             (double)ru, (double)rv);
  }
}

}  // namespace

void framefovMaybeReloadStandaloneConfig(DeviceData& data) { reloadStandaloneConfig(data); }

// Late activation is not needed without an OpenXR half: a device either has the
// capability at creation time or the layer stays inactive for its lifetime.
void maybeActivate(DeviceData& data) { (void)data; }

}  // namespace framefov

namespace framefov {

void framefovMaybeActivate(DeviceData& data) { maybeActivate(data); }

void framefovMaybeUpdateStandaloneGaze(DeviceData& data) { updateStandaloneGaze(data); }

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDevice* pDevice) {
  FRAMEFOV_GUARD_BEGIN
  const VkLayerDeviceCreateInfo* link = findLinkInfo(pCreateInfo->pNext);
  if (!link || !link->u.pLayerInfo) {
    LOG_ERROR("vkCreateDevice: no loader link info in the pNext chain");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerDeviceCreateInfo* mutableLink = const_cast<VkLayerDeviceCreateInfo*>(link);
  PFN_vkGetInstanceProcAddr nextGipa = mutableLink->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr nextGdpa = mutableLink->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  mutableLink->u.pLayerInfo = mutableLink->u.pLayerInfo->pNext;
  if (!nextGipa || !nextGdpa) return VK_ERROR_INITIALIZATION_FAILED;

  InstanceData* instance = findInstanceByKey(dispatchKey(physicalDevice));
  VkInstance instanceHandle = instance ? instance->instance : VK_NULL_HANDLE;
  auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instanceHandle, "vkCreateDevice"));
  if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;

  g_lastNextGdpa = nextGdpa;

  auto data = std::make_unique<DeviceData>();
  data->physicalDevice = physicalDevice;
  data->instance = instance;
  data->nextGipa = nextGipa;
  data->nextGdpa = nextGdpa;

  // No OpenXR half on Steam Frame: the Vulkan half always loads the file and
  // environment itself. [app:...] sections cannot match here (no applicationName).
  const std::string configPath = defaultConfigPath();
  Config localConfig = loadConfigForProcess(std::string(), 0, configPath);
  const Config& cfg = localConfig;

  data->localConfig = localConfig;
  data->standalone = localConfig.standalone;
  data->configPath = configPath;
  data->configMtime = fileMtimeNs(configPath);

  Activation activation = evaluateActivation(*data, pCreateInfo, cfg);
  VkDeviceCreateInfo createInfo = *pCreateInfo;
  std::vector<const char*> extensions;
  FsrFeaturePatch patch;
  // Capable: we can enable our extensions and features on this device.
  bool capable = activation.active;
  auto giveUp = [&](const char* reason) {
    activation.active = false;
    activation.reason = reason;
    capable = false;
  };
  if (capable) {
    fillCaps(*data, pCreateInfo, cfg);

    // Add the extensions we need to the app's list.
    extensions.assign(pCreateInfo->ppEnabledExtensionNames,
                      pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    auto addExtension = [&](const char* name) {
      if (nameInList((uint32_t)extensions.size(), extensions.data(), name)) return true;
      if (!deviceExtensionAvailable(instance, physicalDevice, name)) return false;
      extensions.push_back(name);
      return true;
    };
    if (!addExtension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
      giveUp("cannot enable VK_KHR_fragment_shading_rate");
    } else if (instance->apiVersion < VK_API_VERSION_1_2) {
      // VK_KHR_create_renderpass2 and its dependencies.
      if (!addExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) ||
          !addExtension(VK_KHR_MULTIVIEW_EXTENSION_NAME) ||
          !addExtension(VK_KHR_MAINTENANCE_2_EXTENSION_NAME)) {
        giveUp("cannot enable VK_KHR_create_renderpass2");
      }
    }
  }

  if (capable) {
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    patch.apply(createInfo);
    VkResult result = nextCreate(physicalDevice, &createInfo, pAllocator, pDevice);
    patch.restore(createInfo);
    if (result != VK_SUCCESS) {
      // Retry once without our modifications; the app must keep working.
      LOG_WARN("vkCreateDevice with VRS extensions failed (%d); retrying without", (int)result);
      giveUp("device creation with VRS extensions failed");
      result = nextCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
      if (result != VK_SUCCESS) return result;
    }
  } else {
    VkResult result = nextCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;
  }

  data->device = *pDevice;
  resolveDeviceDispatch(*data);
  data->active = activation.active;
  if (data->active) installSyntheticSnapshot(*data, cfg);
  if (data->active) {
    const Caps& c = data->caps;
    LOG_INFO("device %p ACTIVE: texel %u, rates inner %s middle %s outer %s (supported %zu), "
             "layered=%d nonTrivialCombiners=%d dynamicRendering=%d",
             reinterpret_cast<void*>(*pDevice), c.texel, rateToString(c.innerRate).c_str(),
             rateToString(c.middleRate).c_str(), rateToString(c.outerRate).c_str(),
             c.supportedRates.size(), (int)c.layeredAttachments, (int)c.nonTrivialCombiners,
             (int)c.dynamicRenderingAvailable);
    if (instance && instance->d.GetPhysicalDeviceProperties) {
      VkPhysicalDeviceProperties props{};
      instance->d.GetPhysicalDeviceProperties(physicalDevice, &props);
      LOG_INFO("device name: %s (apiVersion %u.%u.%u)", props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion));
    }
  } else {
    LOG_INFO("device %p created; layer inactive (%s)", reinterpret_cast<void*>(*pDevice),
             activation.reason.c_str());
  }
  addDevice(std::move(data));
  return VK_SUCCESS;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyDevice(VkDevice device,
                                                 const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  std::unique_ptr<DeviceData> data = removeDevice(device);
  if (data) {
    // Free our rate images and pattern buffers before the device goes away.
    if (data->d.DestroyDevice) {
      data->res.destroyAll(data->d, device);
      data->d.DestroyDevice(device, pAllocator);
    } else {
      data->res.destroyAll(data->d, device);
    }
  } else {
    LOG_WARN("vkDestroyDevice: no device data for %p", reinterpret_cast<void*>(device));
  }
  FRAMEFOV_GUARD_END_VOID
}

}  // namespace framefov
