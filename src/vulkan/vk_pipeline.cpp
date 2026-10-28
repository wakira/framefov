// Graphics pipeline injection (05 §6) and shader module scanning.
#include <algorithm>
#include <atomic>
#include <vector>

#include "common/guard.h"
#include "common/log.h"
#include "common/spirv_scan.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"

using namespace framefov;

namespace framefov {
namespace {

template <class T>
T* chainFind(void* head, VkStructureType sType) {
  auto* node = reinterpret_cast<VkBaseOutStructure*>(head);
  while (node) {
    if (node->sType == sType) return reinterpret_cast<T*>(node);
    node = node->pNext;
  }
  return nullptr;
}

template <class T>
const T* chainFind(const void* head, VkStructureType sType) {
  return chainFind<T>(const_cast<void*>(head), sType);
}

bool hasDynamicFragmentShadingRate(const VkPipelineDynamicStateCreateInfo* dynamicState) {
  if (!dynamicState) return false;
  for (uint32_t i = 0; i < dynamicState->dynamicStateCount; ++i) {
    if (dynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR) return true;
  }
  return false;
}

bool sampleMaskIsAllOnes(const VkSampleMask* mask, VkSampleCountFlagBits samples) {
  if (!mask) return true;
  uint32_t bits = 0;
  switch (samples) {
    case VK_SAMPLE_COUNT_1_BIT: bits = 1; break;
    case VK_SAMPLE_COUNT_2_BIT: bits = 2; break;
    case VK_SAMPLE_COUNT_4_BIT: bits = 4; break;
    case VK_SAMPLE_COUNT_8_BIT: bits = 8; break;
    case VK_SAMPLE_COUNT_16_BIT: bits = 16; break;
    case VK_SAMPLE_COUNT_32_BIT: bits = 32; break;
    default: bits = 64; break;
  }
  for (uint32_t word = 0; word < (bits + 31) / 32; ++word) {
    const uint32_t want = (word + 1) * 32 <= bits ? 0xffffffffu : ((1u << (bits % 32)) - 1u);
    if (mask[word] != want) return false;
  }
  return true;
}

SpirvScan lookupModuleScan(DeviceData& data, VkShaderModule module) {
  std::shared_lock<std::shared_mutex> lock(data.lock);
  auto it = data.shaderModules.find(module);
  if (it == data.shaderModules.end()) return SpirvScan{};  // unknown: assume safe
  return it->second;
}

// 05 §6.3: when the driver cannot combine VRS with these shader / state
// features, our attachment must not be applied to this pipeline.
bool pipelineIsSafe(DeviceData& data, const VkGraphicsPipelineCreateInfo& ci) {
  const VkPipelineShaderStageCreateInfo* frag = nullptr;
  for (uint32_t i = 0; i < ci.stageCount; ++i) {
    if (ci.pStages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) frag = &ci.pStages[i];
  }
  if (!frag) return true;

  SpirvScan scan{};
  if (frag->module != VK_NULL_HANDLE) {
    scan = lookupModuleScan(data, frag->module);
  } else if (const auto* mc =
                 chainFind<VkShaderModuleCreateInfo>(frag->pNext,
                                                     VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)) {
    scan = scanSpirv(mc->pCode, mc->codeSize / sizeof(uint32_t));
  } else {
    return true;  // unknown
  }

  const Caps& caps = data.caps;
  if (scan.writesDepthStencil && !caps.withDepthStencilWrites) return false;
  if (scan.writesSampleMask && !caps.withShaderSampleMask) return false;
  if (scan.sampleRate) return false;  // per-sample shading + VRS is pointless

  if (ci.pMultisampleState) {
    if (ci.pMultisampleState->sampleShadingEnable) return false;
    if (ci.pMultisampleState->pSampleMask &&
        !sampleMaskIsAllOnes(ci.pMultisampleState->pSampleMask,
                             ci.pMultisampleState->rasterizationSamples) &&
        !caps.withSampleMask) {
      return false;
    }
  }
  return true;
}

// Diagnostics: how many pipelines actually got our fragment shading rate state,
// and why the others did not. Without this, "the rate image is attached but the
// image looks unchanged" is indistinguishable from "the pipelines ignore it".
struct PipelineStats {
  std::atomic<uint64_t> injected{0}, unsafe{0}, skipDynamicState{0}, skipAppState{0},
      skipRenderPass{0}, total{0};
  std::atomic<int64_t> lastLogNs{0};
};
PipelineStats g_pipelineStats;

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateShaderModule) {
    LOG_ERROR("vkCreateShaderModule: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = data->d.CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
  if (result == VK_SUCCESS && data->active && pShaderModule && pCreateInfo->pCode) {
    const SpirvScan scan = scanSpirv(pCreateInfo->pCode, pCreateInfo->codeSize / sizeof(uint32_t));
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->shaderModules[*pShaderModule] = scan;
  }
  return result;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL framefov_vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                       const VkAllocationCallbacks* pAllocator) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.DestroyShaderModule) {
    LOG_ERROR("vkDestroyShaderModule: no device data");
    return;
  }
  if (data->active) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    data->shaderModules.erase(shaderModule);
  }
  data->d.DestroyShaderModule(device, shaderModule, pAllocator);
  FRAMEFOV_GUARD_END_VOID
}

VKAPI_ATTR VkResult VKAPI_CALL framefov_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
  FRAMEFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.CreateGraphicsPipelines) {
    LOG_ERROR("vkCreateGraphicsPipelines: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!data->active || createInfoCount == 0) {
    return data->d.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                           pAllocator, pPipelines);
  }

  // Storage must stay at a stable address for the duration of the next call.
  std::vector<VkGraphicsPipelineCreateInfo> infos(pCreateInfos, pCreateInfos + createInfoCount);
  std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> states(createInfoCount);
  bool modifiedAny = false;

  // Restores patched VkPipelineCreateFlags2CreateInfoKHR nodes on all paths.
  struct Flags2Restore {
    std::vector<VkPipelineCreateFlags2CreateInfoKHR*> nodes;
    std::vector<VkPipelineCreateFlags> saved;
    ~Flags2Restore() {
      for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i]) nodes[i]->flags = saved[i];
      }
    }
  } patches;

  for (uint32_t i = 0; i < createInfoCount; ++i) {
    VkGraphicsPipelineCreateInfo& ci = infos[i];

    bool eligible = false;
    if (ci.renderPass == VK_NULL_HANDLE) {
      eligible = true;  // dynamic rendering
    } else {
      std::shared_lock<std::shared_mutex> lock(data->lock);
      auto it = data->renderPasses.find(ci.renderPass);
      eligible = it != data->renderPasses.end() && it->second.modified;
    }
    if (!eligible) {
      g_pipelineStats.skipRenderPass.fetch_add(1, std::memory_order_relaxed);
      LOG_ONCE(DEBUG, "pipe-skip-pass",
               "pipelines bound to render passes we did not modify get no VRS state "
               "(their rate attachment would be ignored)");
      continue;
    }
    if (hasDynamicFragmentShadingRate(ci.pDynamicState)) {
      g_pipelineStats.skipDynamicState.fetch_add(1, std::memory_order_relaxed);
      LOG_ONCE(DEBUG, "pipe-skip-dynstate",
               "the app sets VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR, so its rate wins "
               "over our attachment");
      continue;
    }
    if (chainFind<VkPipelineFragmentShadingRateStateCreateInfoKHR>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR)) {
      g_pipelineStats.skipAppState.fetch_add(1, std::memory_order_relaxed);
      LOG_ONCE(DEBUG, "pipe-skip-appstate",
               "the app already provides VkPipelineFragmentShadingRateStateCreateInfoKHR");
      continue;
    }

    const bool safe = pipelineIsSafe(*data, ci);
    VkPipelineFragmentShadingRateStateCreateInfoKHR& state = states[i];
    state = VkPipelineFragmentShadingRateStateCreateInfoKHR{};
    state.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
    state.pNext = const_cast<void*>(ci.pNext);
    state.fragmentSize = {1, 1};
    state.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    state.combinerOps[1] =
        safe ? (data->caps.nonTrivialCombiners ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR
                                              : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR)
             : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    ci.pNext = &state;
    modifiedAny = true;
    LOG_TRACE("pipeline injection: renderPass=%p dynamic=%d safe=%d combiner=(%d,%d) size=%ux%u",
              (void*)ci.renderPass, (int)(ci.renderPass == VK_NULL_HANDLE), (int)safe,
              (int)state.combinerOps[0], (int)state.combinerOps[1], state.fragmentSize.width,
              state.fragmentSize.height);

    if (!safe) {
      g_pipelineStats.unsafe.fetch_add(1, std::memory_order_relaxed);
      LOG_ONCE(DEBUG, "pipeline-vrs-unsafe",
               "VRS disabled for pipelines the driver cannot combine with (depth/stencil writes, "
               "sample mask or sample shading)");
    } else {
      g_pipelineStats.injected.fetch_add(1, std::memory_order_relaxed);
    }

    if (ci.renderPass == VK_NULL_HANDLE) {
      if (auto* flags2 = chainFind<VkPipelineCreateFlags2CreateInfoKHR>(
              const_cast<void*>(ci.pNext), VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)) {
        patches.nodes.push_back(flags2);
        patches.saved.push_back(flags2->flags);
        flags2->flags |= VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
      } else {
        ci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
      }
    }
  }

  // One summary per second at debug level: enough to tell "the attachment is
  // ignored because the pipeline has no combiner state" from "it is applied".
  const uint64_t total = g_pipelineStats.total.fetch_add(createInfoCount, std::memory_order_relaxed) +
                         createInfoCount;
  const int64_t now = nowNs();
  int64_t last = g_pipelineStats.lastLogNs.load(std::memory_order_relaxed);
  if (now - last > 1000LL * 1000 * 1000) {
    g_pipelineStats.lastLogNs.store(now, std::memory_order_relaxed);
    LOG_DEBUG("pipeline injection: %llu pipelines so far - %llu with our 1x1 KEEP/MAX state, "
              "%llu left at KEEP/KEEP for safety, %llu skipped (app dynamic state %llu, app FSR "
              "state %llu, unmodified render pass %llu)",
              (unsigned long long)total,
              (unsigned long long)g_pipelineStats.injected.load(std::memory_order_relaxed),
              (unsigned long long)g_pipelineStats.unsafe.load(std::memory_order_relaxed),
              (unsigned long long)(g_pipelineStats.skipDynamicState.load(std::memory_order_relaxed) +
                                   g_pipelineStats.skipAppState.load(std::memory_order_relaxed) +
                                   g_pipelineStats.skipRenderPass.load(std::memory_order_relaxed)),
              (unsigned long long)g_pipelineStats.skipDynamicState.load(std::memory_order_relaxed),
              (unsigned long long)g_pipelineStats.skipAppState.load(std::memory_order_relaxed),
              (unsigned long long)g_pipelineStats.skipRenderPass.load(std::memory_order_relaxed));
  }

  if (!modifiedAny) {
    return data->d.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                           pAllocator, pPipelines);
  }

  VkResult result = data->d.CreateGraphicsPipelines(device, pipelineCache, createInfoCount,
                                                    infos.data(), pAllocator, pPipelines);
  return result;
  FRAMEFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

}  // namespace framefov
