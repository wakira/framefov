#include "test_framework.h"

#include "common/pattern.h"

#include <cmath>

using namespace framefov;

FRAMEFOV_TEST(pattern_rate_encoding) {
  CHECK_EQ(encodeRate(Rate{1, 1}), 0);
  CHECK_EQ(encodeRate(Rate{1, 2}), 1);
  CHECK_EQ(encodeRate(Rate{2, 1}), 4);
  CHECK_EQ(encodeRate(Rate{2, 2}), 5);
  CHECK_EQ(encodeRate(Rate{2, 4}), 6);
  CHECK_EQ(encodeRate(Rate{4, 2}), 9);
  CHECK_EQ(encodeRate(Rate{4, 4}), 10);
  Rate r;
  CHECK(decodeRate(0, r));
  CHECK_EQ((int)r.w, 1);
  CHECK_EQ((int)r.h, 1);
  CHECK(decodeRate(5, r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 2);
  CHECK(decodeRate(10, r));
  CHECK_EQ((int)r.w, 4);
  CHECK_EQ((int)r.h, 4);
  CHECK(!decodeRate(3, r));  // 4x2? no: 3 = log2(w)=0, log2(h)=3 -> invalid
}

FRAMEFOV_TEST(pattern_generation) {
  PatternParams p;
  p.innerRadius = 0.35f;
  p.outerRadius = 0.70f;
  p.innerRate = Rate{1, 1};
  p.middleRate = Rate{2, 1};
  p.outerRate = Rate{2, 2};
  const uint32_t ew = 64, eh = 64;
  const uint32_t w = 2 * ew;
  std::vector<uint8_t> buf = buildPattern(ew, eh, p);
  CHECK_EQ(buf.size(), (size_t)w * (2 * eh));
  auto at = [&](uint32_t x, uint32_t y) { return buf[(size_t)y * w + x]; };

  CHECK_EQ(at(ew, eh), encodeRate(p.innerRate));       // fovea center
  CHECK_EQ(at(0, 0), encodeRate(p.outerRate));         // corner
  CHECK_EQ(at(w - 1, 2 * eh - 1), encodeRate(p.outerRate));
  // d ≈ 0.484: between inner (0.35) and outer (0.70).
  CHECK_EQ(at(ew + 15, eh), encodeRate(p.middleRate));
  // d ≈ 0.90: outside.
  CHECK_EQ(at(ew + 29, eh), encodeRate(p.outerRate));

  // Symmetry about both axes.
  for (uint32_t x = 0; x < w; ++x) {
    CHECK_EQ(at(x, eh), at(w - 1 - x, eh));
    CHECK_EQ(at(ew, x), at(ew, 2 * eh - 1 - x));
  }

  // Horizontal scale widens the ellipse: with hscale = 4 a horizontally distant
  // texel falls back into the inner region.
  PatternParams wide = p;
  wide.horizontalScale = 4.0f;
  std::vector<uint8_t> wideBuf = buildPattern(ew, eh, wide);
  CHECK_EQ(wideBuf[(size_t)eh * w + (ew + 15)], encodeRate(wide.innerRate));
}

FRAMEFOV_TEST(pattern_debug_modes) {
  PatternParams p;
  p.innerRadius = 0.35f;
  p.outerRadius = 0.70f;
  p.innerRate = Rate{1, 1};
  p.middleRate = Rate{2, 1};
  p.outerRate = Rate{2, 2};
  const uint32_t ew = 32, eh = 32, w = 2 * ew;

  p.debugMode = 1;
  std::vector<uint8_t> flat = buildPattern(ew, eh, p);
  for (uint8_t v : flat) CHECK_EQ(v, encodeRate(p.outerRate));

  p.debugMode = 2;
  std::vector<uint8_t> inverted = buildPattern(ew, eh, p);
  CHECK_EQ(inverted[(size_t)eh * w + ew], encodeRate(p.outerRate));  // fovea coarse
  CHECK_EQ(inverted[0], encodeRate(p.innerRate));                    // periphery fine
}

FRAMEFOV_TEST(pattern_copy_region_math) {
  CopyRegion r;
  // 80x80 px region at t=8 -> 10x10 texels; S is 100x100 texels.
  CHECK(gazeCopyRegion(0, 0, 80, 80, 0, 8, 100, 100, 0.5f, 0.5f, r));
  CHECK_EQ(r.imageX, 0);
  CHECK_EQ(r.imageY, 0);
  CHECK_EQ(r.imageW, 10u);
  CHECK_EQ(r.imageH, 10u);
  CHECK_EQ(r.srcX, 5u);
  CHECK_EQ(r.srcY, 5u);
  CHECK_EQ(r.srcRowLength, 20u);
  CHECK_EQ(r.srcImageHeight, 20u);
  CHECK_EQ(r.bufferOffset, 105);

  CHECK(gazeCopyRegion(0, 0, 80, 80, 0, 8, 100, 100, 0.0f, 0.0f, r));
  CHECK_EQ(r.srcX, 10u);
  CHECK_EQ(r.srcY, 10u);
  CHECK_EQ(r.bufferOffset, 210);

  CHECK(gazeCopyRegion(0, 0, 80, 80, 0, 8, 100, 100, 1.0f, 1.0f, r));
  CHECK_EQ(r.srcX, 0u);
  CHECK_EQ(r.srcY, 0u);
  CHECK_EQ(r.bufferOffset, 0);

  // Not a multiple of t: px=5, py=3, 10x10 px at t=8 -> 2x2 texels.
  CHECK(gazeCopyRegion(5, 3, 10, 10, 1, 8, 100, 100, 0.5f, 0.5f, r));
  CHECK_EQ(r.imageX, 0);
  CHECK_EQ(r.imageY, 0);
  CHECK_EQ(r.imageW, 2u);
  CHECK_EQ(r.imageH, 2u);
  CHECK_EQ(r.srcRowLength, 4u);
  CHECK_EQ(r.srcX, 1u);
  CHECK_EQ(r.srcY, 1u);
  CHECK_EQ(r.bufferOffset, 5);
  CHECK_EQ(r.layer, 1u);

  // Clamped to the S image edge: a very wide region stops at sw.
  CHECK(gazeCopyRegion(0, 0, 1000, 100, 0, 8, 100, 100, 0.5f, 0.5f, r));
  CHECK_EQ(r.imageW, 100u);
  CHECK_EQ(r.srcRowLength, 200u);
  CHECK_EQ(r.imageH, 13u);  // ceil(100/8) = 13

  // u = 0.9 of a 10 texel region -> 9 -> srcX = 1.
  CHECK(gazeCopyRegion(0, 0, 80, 80, 0, 8, 100, 100, 0.9f, 0.1f, r));
  CHECK_EQ(r.srcX, 1u);
  CHECK_EQ(r.srcY, 9u);

  // Empty / outside regions.
  CHECK(!gazeCopyRegion(0, 0, 0, 80, 0, 8, 100, 100, 0.5f, 0.5f, r));
  CHECK(!gazeCopyRegion(1000, 1000, 80, 80, 0, 8, 100, 100, 0.5f, 0.5f, r));
  CHECK(!gazeCopyRegion(0, 0, 80, 80, 0, 0, 100, 100, 0.5f, 0.5f, r));

  VkBufferImageCopy c = toVkBufferImageCopy(r);
  CHECK_EQ((int)c.bufferOffset, r.bufferOffset);
  CHECK_EQ(c.bufferRowLength, r.srcRowLength);
  CHECK_EQ(c.bufferImageHeight, r.srcImageHeight);
  CHECK_EQ((int)c.imageSubresource.aspectMask, (int)VK_IMAGE_ASPECT_COLOR_BIT);
  CHECK_EQ(c.imageSubresource.baseArrayLayer, r.layer);
  CHECK_EQ(c.imageSubresource.layerCount, 1u);
  CHECK_EQ(c.imageOffset.x, r.imageX);
  CHECK_EQ(c.imageOffset.y, r.imageY);
  CHECK_EQ(c.imageExtent.width, r.imageW);
  CHECK_EQ(c.imageExtent.height, r.imageH);
  CHECK_EQ(c.imageExtent.depth, 1u);
}

FRAMEFOV_TEST(pattern_draw_radii_rings) {
  PatternParams p;
  p.innerRadius = 0.4f;
  p.outerRadius = 0.8f;
  p.innerRate = Rate{1, 1};
  p.middleRate = Rate{2, 2};
  p.outerRate = Rate{2, 2};
  p.debugMode = 0;
  p.drawRadii = 1;  // both
  p.ringWidth = 2;
  p.ringInnerRate = encodeRate(Rate{2, 1});
  p.ringOuterRate = encodeRate(Rate{1, 2});
  const uint32_t ew = 64, eh = 64, w = 2 * ew;
  const std::vector<uint8_t> buf = buildPattern(ew, eh, p);
  auto at = [&](uint32_t x, uint32_t y) { return buf[(size_t)y * w + x]; };
  // ringWidth / min(ew, eh) = 0.03125: the bands are 2 texels wide at the radii.
  const uint32_t xIn = ew + 12;    // d = 0.391
  const uint32_t xInOut = ew + 13; // d = 0.422, just outside the inner radius
  const uint32_t xMid = ew + 20;   // d = 0.641
  const uint32_t xOut = ew + 25;   // d = 0.797
  CHECK_EQ(at(xIn, eh), encodeRate(Rate{2, 1}));     // ring at the inner radius
  CHECK_EQ(at(xInOut, eh), encodeRate(Rate{2, 1}));
  CHECK_EQ(at(xMid, eh), encodeRate(Rate{2, 2}));    // unmarked middle zone
  CHECK_EQ(at(xOut, eh), encodeRate(Rate{1, 2}));    // ring at the outer radius
  CHECK_EQ(at(ew, eh), encodeRate(Rate{1, 1}));      // fovea centre is untouched
  CHECK_EQ(at(0, 0), encodeRate(Rate{2, 2}));        // corner is untouched

  // The ring closes: on the y axis at the same distance.
  CHECK_EQ(at(ew, eh - 13), encodeRate(Rate{2, 1}));

  // Without the overlay those texels are plain zone rates.
  p.drawRadii = 0;
  const std::vector<uint8_t> plain = buildPattern(ew, eh, p);
  CHECK_EQ(plain[(size_t)eh * w + xIn], encodeRate(Rate{1, 1}));
  CHECK_EQ(plain[(size_t)eh * w + xInOut], encodeRate(Rate{2, 2}));
  CHECK_EQ(plain[(size_t)eh * w + xMid], encodeRate(Rate{2, 2}));
  CHECK_EQ(plain[(size_t)eh * w + xOut], encodeRate(Rate{2, 2}));

  // inner only / outer only.
  p.drawRadii = 2;
  const std::vector<uint8_t> innerOnly = buildPattern(ew, eh, p);
  CHECK_EQ(innerOnly[(size_t)eh * w + xIn], encodeRate(Rate{2, 1}));
  CHECK_EQ(innerOnly[(size_t)eh * w + xOut], encodeRate(Rate{2, 2}));
  p.drawRadii = 3;
  const std::vector<uint8_t> outerOnly = buildPattern(ew, eh, p);
  CHECK_EQ(outerOnly[(size_t)eh * w + xIn], encodeRate(Rate{1, 1}));
  CHECK_EQ(outerOnly[(size_t)eh * w + xOut], encodeRate(Rate{1, 2}));

  // ring_width = 1 -> one texel wide: xIn (d = 0.391) still is a ring, xInOut
  // (d = 0.422) falls back to the middle zone.
  p.drawRadii = 1;
  p.ringWidth = 1;
  const std::vector<uint8_t> thin = buildPattern(ew, eh, p);
  CHECK_EQ(thin[(size_t)eh * w + xIn], encodeRate(Rate{2, 1}));
  CHECK_EQ(thin[(size_t)eh * w + xInOut], encodeRate(Rate{2, 2}));
}

FRAMEFOV_TEST(pattern_ring_rate_choice) {
  const std::vector<Rate> supported = {Rate{1, 1}, Rate{2, 1}, Rate{1, 2}, Rate{2, 2}};
  Config cfg;
  cfg.innerRadius = 0.3f;
  cfg.outerRadius = 0.6f;
  cfg.drawRadii = 1;
  cfg.ringWidth = 2;

  // With 1x1 inside and 2x2 outside only an anisotropic rate differs from both.
  PatternParams p = makePatternParams(cfg, Rate{1, 1}, Rate{2, 2}, Rate{2, 2}, true, &supported);
  CHECK_EQ(p.drawRadii, 1u);
  CHECK_EQ(p.ringWidth, 2u);
  Rate r;
  CHECK(decodeRate(p.ringInnerRate, r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 1);
  CHECK(decodeRate(p.ringOuterRate, r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 1);

  // Debug mode 2 swaps the zones; the ring rate must still differ from both.
  cfg.debugMode = 2;
  PatternParams inv = makePatternParams(cfg, Rate{1, 1}, Rate{2, 2}, Rate{2, 2}, true, &supported);
  CHECK(decodeRate(inv.ringInnerRate, r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 1);
  CHECK(decodeRate(inv.ringOuterRate, r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 1);

  // Hardware with only 1x1 and 2x2: no rate differs from both, so the ring
  // degrades to the zone rate that differs from the outside - the radius is
  // still visible as the edge of the inner zone.
  cfg.debugMode = 0;
  const std::vector<Rate> square = {Rate{1, 1}, Rate{2, 2}};
  PatternParams deg = makePatternParams(cfg, Rate{1, 1}, Rate{2, 2}, Rate{2, 2}, true, &square);
  CHECK_EQ(deg.ringInnerRate, encodeRate(Rate{1, 1}));
  CHECK_EQ(deg.ringOuterRate, encodeRate(Rate{1, 1}));
}

FRAMEFOV_TEST(pattern_params_from_config) {
  Config cfg;
  cfg.innerRadius = 0.1f;
  cfg.outerRadius = 0.2f;
  cfg.fixedInnerRadius = 0.3f;
  cfg.fixedOuterRadius = 0.4f;
  cfg.horizontalScale = 1.5f;
  cfg.debugMode = 2;

  PatternParams tracked = makePatternParams(cfg, Rate{1, 1}, Rate{2, 2}, Rate{2, 2}, true);
  CHECK_NEAR(tracked.innerRadius, 0.1, 1e-6);
  CHECK_NEAR(tracked.outerRadius, 0.2, 1e-6);
  CHECK_EQ(tracked.debugMode, 2);
  CHECK_NEAR(tracked.horizontalScale, 1.5, 1e-6);

  PatternParams fixed = makePatternParams(cfg, Rate{1, 1}, Rate{2, 2}, Rate{2, 2}, false);
  CHECK_NEAR(fixed.innerRadius, 0.3, 1e-6);
  CHECK_NEAR(fixed.outerRadius, 0.4, 1e-6);
  CHECK(!fixed.eyeTracked);
}
