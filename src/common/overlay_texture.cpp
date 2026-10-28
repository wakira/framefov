#include "common/overlay_texture.h"

#include <algorithm>
#include <cmath>

namespace framefov {

void unpackColor(uint32_t rgb, float alpha, float& r, float& g, float& b, float& a) {
  r = (float)((rgb >> 16) & 0xff) / 255.f;
  g = (float)((rgb >> 8) & 0xff) / 255.f;
  b = (float)(rgb & 0xff) / 255.f;
  a = alpha;
}

std::vector<uint8_t> buildRingTexture(const RingTextureParams& p) {
  const uint32_t w = std::max(2u, p.width);
  const uint32_t h = std::max(2u, p.height);
  std::vector<uint8_t> out((size_t)w * h * 4, 0);
  const float halfX = (float)w * 0.5f;
  const float halfY = (float)h * 0.5f;
  const float rx = std::max(1e-4f, p.radiusX);
  const float ry = std::max(1e-4f, p.radiusY);
  const float thickness = std::max(0.0f, p.thickness);
  const float band = thickness * 0.5f;
  // One pixel in texture-fraction units, for the antialiasing width.
  const float px = 1.0f / (float)std::min(w, h);
  const float aa = std::max(px, band * 0.5f);

  if (p.pattern == TexturePattern::Solid) {
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
      out[i + 0] = (uint8_t)std::lround(std::clamp(p.r, 0.0f, 1.0f) * 255.0f);
      out[i + 1] = (uint8_t)std::lround(std::clamp(p.g, 0.0f, 1.0f) * 255.0f);
      out[i + 2] = (uint8_t)std::lround(std::clamp(p.b, 0.0f, 1.0f) * 255.0f);
      out[i + 3] = (uint8_t)std::lround(std::clamp(p.a, 0.0f, 1.0f) * 255.0f);
    }
    return out;
  }
  if (p.pattern == TexturePattern::Checker) {
    const uint32_t squares = std::max(2u, p.checkerSquares);
    const float cell = (float)std::min(w, h) / (float)squares;
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t cx = (uint32_t)((float)x / cell);
        const uint32_t cy = (uint32_t)((float)y / cell);
        const bool on = ((cx + cy) & 1u) != 0;
        uint8_t* texel = &out[((size_t)y * w + x) * 4];
        const float v = on ? 1.0f : 0.15f;
        texel[0] = (uint8_t)std::lround(v * 255.0f);
        texel[1] = (uint8_t)std::lround(on ? 0.2f : 0.9f * 255.0f) % 256;
        texel[2] = (uint8_t)std::lround(v * 255.0f);
        texel[3] = (uint8_t)std::lround(std::clamp(p.a, 0.0f, 1.0f) * 255.0f);
      }
    }
    return out;
  }

  for (uint32_t y = 0; y < h; ++y) {
    const float dy = (((float)y + 0.5f) - halfY) / halfY;  // -1..1
    for (uint32_t x = 0; x < w; ++x) {
      const float dx = (((float)x + 0.5f) - halfX) / halfX;
      const float dn = std::sqrt((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
      // Absolute radial distance from the ideal ellipse, in texture fractions
      // (dn is in units of the radius, so scale by the tighter axis).
      const float dist = std::fabs(dn - 1.0f) * std::min(rx, ry);
      float alpha = 1.0f - (dist - band) / aa;
      alpha = std::clamp(alpha, 0.0f, 1.0f);
      if (alpha <= 0.0f) continue;
      uint8_t* texel = &out[((size_t)y * w + x) * 4];
      texel[0] = (uint8_t)std::lround(std::clamp(p.r, 0.0f, 1.0f) * 255.0f);
      texel[1] = (uint8_t)std::lround(std::clamp(p.g, 0.0f, 1.0f) * 255.0f);
      texel[2] = (uint8_t)std::lround(std::clamp(p.b, 0.0f, 1.0f) * 255.0f);
      texel[3] = (uint8_t)std::lround(std::clamp(p.a * alpha, 0.0f, 1.0f) * 255.0f);
    }
  }
  return out;
}

}  // namespace framefov
