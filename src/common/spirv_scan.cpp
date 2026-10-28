#include "common/spirv_scan.h"

namespace framefov {

namespace {

constexpr uint32_t kMagic = 0x07230203;
constexpr uint16_t kOpCapability = 17;
constexpr uint16_t kOpDecorate = 71;
constexpr uint32_t kDecorationBuiltIn = 11;
constexpr uint32_t kCapabilitySampleRateShading = 35;
constexpr uint32_t kBuiltInSampleMask = 20;
constexpr uint32_t kBuiltInFragDepth = 22;
constexpr uint32_t kBuiltInFragStencilRefEXT = 5014;

}  // namespace

SpirvScan scanSpirv(const uint32_t* code, size_t wordCount) {
  SpirvScan result;
  if (!code || wordCount < 5) return result;
  if (code[0] != kMagic) return result;

  size_t i = 5;  // skip the header
  while (i < wordCount) {
    const uint32_t w = code[i];
    const uint16_t wc = (uint16_t)(w >> 16);
    const uint16_t op = (uint16_t)(w & 0xffffu);
    if (wc == 0) break;  // malformed; stop
    if (i + wc > wordCount) break;

    if (op == kOpCapability && wc >= 2) {
      if (code[i + 1] == kCapabilitySampleRateShading) result.sampleRate = true;
    } else if (op == kOpDecorate && wc >= 3) {
      const uint32_t decoration = code[i + 2];
      if (decoration == kDecorationBuiltIn && wc >= 4) {
        const uint32_t builtIn = code[i + 3];
        if (builtIn == kBuiltInFragDepth || builtIn == kBuiltInFragStencilRefEXT) {
          result.writesDepthStencil = true;
        } else if (builtIn == kBuiltInSampleMask) {
          result.writesSampleMask = true;
        }
      }
    }
    i += wc;
  }
  return result;
}

}  // namespace framefov
