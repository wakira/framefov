#include "test_framework.h"

#include "common/spirv_scan.h"

#include <vector>

using namespace framefov;

namespace {

uint32_t op(uint16_t opcode, uint16_t wordCount) {
  return ((uint32_t)wordCount << 16) | (uint32_t)opcode;
}

std::vector<uint32_t> spirv(const std::vector<uint32_t>& body) {
  std::vector<uint32_t> code = {0x07230203u, 0x00010000u, 0u, 100u, 0u};
  code.insert(code.end(), body.begin(), body.end());
  return code;
}

}  // namespace

FRAMEFOV_TEST(spirv_scan_empty_and_invalid) {
  CHECK(!scanSpirv(nullptr, 0).writesDepthStencil);
  const uint32_t bogus[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  SpirvScan s = scanSpirv(bogus, 8);
  CHECK(!s.writesDepthStencil);
  CHECK(!s.writesSampleMask);
  CHECK(!s.sampleRate);
  // Truncated header.
  const uint32_t tiny[3] = {0x07230203u, 0, 0};
  CHECK(!scanSpirv(tiny, 3).sampleRate);
}

FRAMEFOV_TEST(spirv_scan_sample_rate_capability) {
  auto code = spirv({op(17, 2), 35});  // OpCapability SampleRateShading
  SpirvScan s = scanSpirv(code.data(), code.size());
  CHECK(s.sampleRate);
  CHECK(!s.writesDepthStencil);

  auto other = spirv({op(17, 2), 1});  // OpCapability Shader
  CHECK(!scanSpirv(other.data(), other.size()).sampleRate);
}

FRAMEFOV_TEST(spirv_scan_frag_depth) {
  auto code = spirv({op(71, 4), 7, 11, 22});  // OpDecorate %7 BuiltIn FragDepth
  SpirvScan s = scanSpirv(code.data(), code.size());
  CHECK(s.writesDepthStencil);
  CHECK(!s.writesSampleMask);
  CHECK(!s.sampleRate);
}

FRAMEFOV_TEST(spirv_scan_frag_stencil) {
  auto code = spirv({op(71, 4), 9, 11, 5014});
  CHECK(scanSpirv(code.data(), code.size()).writesDepthStencil);
}

FRAMEFOV_TEST(spirv_scan_sample_mask) {
  auto code = spirv({op(71, 4), 3, 11, 20});
  SpirvScan s = scanSpirv(code.data(), code.size());
  CHECK(s.writesSampleMask);
  CHECK(!s.writesDepthStencil);
}

FRAMEFOV_TEST(spirv_scan_ignores_other_decorations) {
  auto code = spirv({
      op(71, 4), 3, 11, 0,    // BuiltIn Position
      op(71, 4), 4, 30, 0,    // Location
      op(71, 4), 5, 11, 1,    // BuiltIn FragCoord
  });
  SpirvScan s = scanSpirv(code.data(), code.size());
  CHECK(!s.writesDepthStencil);
  CHECK(!s.writesSampleMask);
}

FRAMEFOV_TEST(spirv_scan_stops_on_bad_word_count) {
  auto code = spirv({
      op(71, 4), 7, 11, 22,  // FragDepth
      0,                     // wc = 0: malformed, stop
      op(71, 4), 8, 11, 20,  // SampleMask (must not be reached)
  });
  SpirvScan s = scanSpirv(code.data(), code.size());
  CHECK(s.writesDepthStencil);  // seen before the bad instruction
  CHECK(!s.writesSampleMask);   // not scanned after it
}
