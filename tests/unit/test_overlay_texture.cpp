#include "test_framework.h"

#include "common/overlay_texture.h"

#include <cmath>

using namespace framefov;

FRAMEFOV_TEST(overlay_ring_texture) {
  RingTextureParams p;
  p.width = 256;
  p.height = 256;
  p.radiusX = 0.25f;
  p.radiusY = 0.25f;
  p.thickness = 0.02f;
  p.r = 0.f;
  p.g = 1.f;
  p.b = 0.f;
  p.a = 1.f;
  const std::vector<uint8_t> tex = buildRingTexture(p);
  CHECK_EQ(tex.size(), (size_t)256 * 256 * 4);

  auto alphaAt = [&](uint32_t x, uint32_t y) { return tex[((size_t)y * 256 + x) * 4 + 3]; };
  auto greenAt = [&](uint32_t x, uint32_t y) { return tex[((size_t)y * 256 + x) * 4 + 1]; };

  // Centre and corners are transparent: it is a ring, not a disc.
  CHECK_EQ(alphaAt(128, 128), 0);
  CHECK_EQ(alphaAt(0, 0), 0);
  CHECK_EQ(alphaAt(255, 255), 0);

  // On the ring: radius 0.25 of the half texture (128 px) is 32 px.
  CHECK_EQ(alphaAt(128 + 32, 128), 255);
  CHECK_EQ(greenAt(128 + 32, 128), 255);
  CHECK_EQ(alphaAt(128, 128 + 32), 255);
  CHECK_EQ(alphaAt(128 - 32, 128), 255);
  CHECK_EQ(alphaAt(128, 128 - 32), 255);

  // Well inside/outside the band is transparent (band half width is 0.01 -> ~1.3 px).
  CHECK_EQ(alphaAt(128 + 20, 128), 0);
  CHECK_EQ(alphaAt(128 + 44, 128), 0);

  // radiusX != radiusY stretches the ring horizontally only: with radiusX 0.5 the
  // x extent is 32 px past the 96 we would otherwise have (0.5 * half texture).
  RingTextureParams e = p;
  e.radiusX = 0.5f;
  e.radiusY = 0.25f;
  const std::vector<uint8_t> ellipse = buildRingTexture(e);
  auto alpha = [&](uint32_t x, uint32_t y) { return ellipse[((size_t)y * 256 + x) * 4 + 3]; };
  CHECK_EQ(alpha(128, 128), 0);        // centre still empty
  CHECK_EQ(alpha(128 + 32, 128), 0);   // the old x radius is now inside the ring
  CHECK_EQ(alpha(128 + 64, 128), 255); // x extent 0.5 * 128 = 64 px
  CHECK_EQ(alpha(128, 128 + 32), 255); // y extent unchanged, 0.25 * 128 = 32 px
  CHECK_EQ(alpha(128 + 96, 128), 0);   // and it stops there
}
