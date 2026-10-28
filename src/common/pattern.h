// Shading-rate pattern generation and gaze -> copy-region math.
//
// Pure C++ (no Vulkan calls, no device state), unit tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "common/config.h"

namespace framefov {

// Rate value: (log2(fw) << 2) | log2(fh). The seven legal sizes encode to
// 1x1=0, 1x2=1, 2x1=4, 2x2=5, 2x4=6, 4x2=9, 4x4=10.
uint8_t encodeRate(const Rate& r);
bool decodeRate(uint8_t code, Rate& out);

struct PatternParams {
  float innerRadius = 0.35f, outerRadius = 0.70f;
  Rate innerRate{1, 1}, middleRate{2, 2}, outerRate{2, 2};
  float horizontalScale = 1.0f;
  int debugMode = 0;      // 0 off, 1 = outer everywhere, 2 = inverted
  bool eyeTracked = false;
  // Debug radii overlay (see Config::drawRadii). The rings are stamped in the
  // ring rates, which are chosen to differ from the zones on both sides of the
  // radius so the circle is visible as a line of different sharpness.
  uint32_t drawRadii = 0;  // 0 off, 1 both, 2 inner, 3 outer
  uint32_t ringWidth = 2;  // band width in texels
  uint8_t ringInnerRate = 0;
  uint8_t ringOuterRate = 0;
};

// Fills the radii and rates from the config: eye-tracked or fixed radii, and the
// rates already clamped to the device's supported set. `supported` (optional) is
// the device's supported rate set, used to pick a rate for the draw_radii rings
// that the device can actually do; the pattern rates are assumed supported.
PatternParams makePatternParams(const Config& cfg, const Rate& inner, const Rate& middle,
                                const Rate& outer, bool eyeTracked,
                                const std::vector<Rate>* supported = nullptr);
// Buffer of (2*ew) x (2*eh) bytes, row major, gaze/fovea center at (ew, eh).
// `ew`/`eh` are the eye region size in texels (06 §1).
std::vector<uint8_t> buildPattern(uint32_t ew, uint32_t eh, const PatternParams& params);

// Cache key for pattern buffers.
struct PatternKey {
  uint32_t ew = 0, eh = 0;
  uint64_t generation = 0;
  int debugMode = 0;
  bool eyeTracked = false;
  uint32_t drawRadii = 0;
  uint32_t ringWidth = 0;
};
bool operator==(const PatternKey& a, const PatternKey& b);
struct PatternKeyHash {
  size_t operator()(const PatternKey& k) const;
};

// One region of a foveation plan translated to texel-space copies (06 §2).
struct CopyRegion {
  int32_t imageX = 0, imageY = 0;          // destination texel origin in S
  uint32_t imageW = 0, imageH = 0;         // destination texel size
  uint32_t layer = 0;
  uint32_t srcX = 0, srcY = 0;             // source window origin inside P
  uint32_t srcRowLength = 0;               // 2 * imageW
  uint32_t srcImageHeight = 0;             // 2 * imageH
  int64_t bufferOffset = 0;                // bytes into P
};

// Region rect in pixels (px, py, pw, ph), gaze (u, v) in [0,1], texel size t,
// S image size (sw, sh) in texels. Returns false when the region is empty or
// fully outside S.
bool gazeCopyRegion(int32_t px, int32_t py, uint32_t pw, uint32_t ph, uint32_t layer, uint32_t texel,
                    uint32_t sw, uint32_t sh, float u, float v, CopyRegion& out);

VkBufferImageCopy toVkBufferImageCopy(const CopyRegion& r);

}  // namespace framefov
