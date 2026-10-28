// Minimal SPIR-V scan: does the fragment stage write FragDepth/FragStencilRef,
// write SampleMask, or declare SampleRateShading?
//
// Pure, unit tested (05 §7).
#pragma once

#include <cstddef>
#include <cstdint>

namespace framefov {

struct SpirvScan {
  bool writesDepthStencil = false;  // FragDepth or FragStencilRefEXT decoration
  bool writesSampleMask = false;    // SampleMask built-in decoration
  bool sampleRate = false;          // OpCapability SampleRateShading
};

SpirvScan scanSpirv(const uint32_t* code, size_t wordCount);

}  // namespace framefov
