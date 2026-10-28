#include "common/pattern.h"

#include <algorithm>
#include <cmath>

namespace framefov {

uint8_t encodeRate(const Rate& r) {
  auto log2i = [](uint8_t v) -> uint8_t {
    switch (v) {
      case 1: return 0;
      case 2: return 1;
      case 4: return 2;
      default: return 0;
    }
  };
  return (uint8_t)((log2i(r.w) << 2) | log2i(r.h));
}

bool decodeRate(uint8_t code, Rate& out) {
  const uint8_t lw = (code >> 2) & 0x3;
  const uint8_t lh = code & 0x3;
  if (lw > 2 || lh > 2) return false;
  out.w = (uint8_t)(1 << lw);
  out.h = (uint8_t)(1 << lh);
  return true;
}

namespace {

// Which rate to stamp for a debug ring at one radius. The ring is meant to be
// visible as a line, so prefer a rate that differs from the zones on both sides
// (an anisotropic rate does that when the hardware has one); otherwise fall back
// to the pattern rate that differs from the zone outside the ring, which still
// draws the radius, just as the edge of an enlarged/eroded zone.
uint8_t pickRingRate(const Rate& inner, const Rate& middle, const Rate& outer,
                     const Rate& ringInside, const Rate& ringOutside,
                     const std::vector<Rate>* supported) {
  auto allowed = [&](const Rate& r) {
    if (!supported) return true;
    for (const Rate& s : *supported) {
      if (s == r) return true;
    }
    return false;
  };
  const Rate prefer[2] = {Rate{2, 1}, Rate{1, 2}};
  for (const Rate& c : prefer) {
    if (allowed(c) && c != ringInside && c != ringOutside) return encodeRate(c);
  }
  const Rate cand[3] = {inner, middle, outer};
  for (const Rate& c : cand) {
    if (c != ringInside && c != ringOutside) return encodeRate(c);
  }
  for (const Rate& c : cand) {
    if (c != ringOutside) return encodeRate(c);
  }
  return encodeRate(inner);
}

}  // namespace

PatternParams makePatternParams(const Config& cfg, const Rate& inner, const Rate& middle,
                                const Rate& outer, bool eyeTracked,
                                const std::vector<Rate>* supported) {
  PatternParams p;
  p.eyeTracked = eyeTracked;
  p.innerRadius = eyeTracked ? cfg.innerRadius : cfg.fixedInnerRadius;
  p.outerRadius = eyeTracked ? cfg.outerRadius : cfg.fixedOuterRadius;
  p.innerRate = inner;
  p.middleRate = middle;
  p.outerRate = outer;
  p.horizontalScale = cfg.horizontalScale;
  p.debugMode = cfg.debugMode;
  p.drawRadii = cfg.drawRadii;
  p.ringWidth = cfg.ringWidth;

  // The zones the ring sits between, per debug mode:
  //   0: inner | middle | outer      1: outer everywhere      2: outer | inner
  const Rate zoneInner = cfg.debugMode == 2 ? outer : inner;
  const Rate zoneMiddle = cfg.debugMode == 1 ? outer : (cfg.debugMode == 2 ? inner : middle);
  const Rate zoneOuter = cfg.debugMode == 2 ? inner : outer;
  p.ringInnerRate = pickRingRate(inner, middle, outer, zoneInner, zoneMiddle, supported);
  p.ringOuterRate = pickRingRate(inner, middle, outer, zoneMiddle, zoneOuter, supported);
  return p;
}

std::vector<uint8_t> buildPattern(uint32_t ew, uint32_t eh, const PatternParams& params) {
  const uint32_t w = 2 * ew, h = 2 * eh;
  std::vector<uint8_t> out((size_t)w * h, encodeRate(params.outerRate));
  if (w == 0 || h == 0) return out;

  const uint8_t inner = encodeRate(params.innerRate);
  const uint8_t middle = encodeRate(params.middleRate);
  const uint8_t outer = encodeRate(params.outerRate);
  const float hscale = params.horizontalScale > 0.f ? params.horizontalScale : 1.f;
  const float cx = (float)ew, cy = (float)eh;

  // A ring of `ringWidth` texels: one texel is 2/size in normalized radius, so
  // |d - r| < ringWidth / size covers ringWidth texels. The band is a little
  // wider on the axis that is not the smaller one, which is fine for a debug
  // marker.
  const uint32_t ringDim = std::min(ew, eh);
  const float ringEps =
      params.ringWidth > 0 && ringDim > 0 ? (float)params.ringWidth / (float)ringDim : 0.f;
  const uint32_t drawRadii = ringEps > 0.f ? params.drawRadii : 0u;
  const bool drawInner = drawRadii == 1 || drawRadii == 2;
  const bool drawOuter = drawRadii == 1 || drawRadii == 3;
  const uint8_t ringInner = params.ringInnerRate;
  const uint8_t ringOuter = params.ringOuterRate;

  for (uint32_t y = 0; y < h; ++y) {
    const float dy = ((float)y + 0.5f) - cy;
    const float ny = 2.f * dy / (float)eh;
    for (uint32_t x = 0; x < w; ++x) {
      const float dx = ((float)x + 0.5f) - cx;
      const float nx = 2.f * dx / (float)ew;
      const float d = std::sqrt((nx / hscale) * (nx / hscale) + ny * ny);
      uint8_t rate;
      if (params.debugMode == 1) {
        rate = outer;
      } else if (params.debugMode == 2) {
        rate = d < params.innerRadius ? outer : inner;
      } else {
        rate = d < params.innerRadius ? inner : (d < params.outerRadius ? middle : outer);
      }
      // Rings on top, so they are drawn in every debug mode (this is what makes
      // the radii visible at all when middle and outer clamp to the same rate).
      if (drawInner && std::fabs(d - params.innerRadius) < ringEps) rate = ringInner;
      if (drawOuter && std::fabs(d - params.outerRadius) < ringEps) rate = ringOuter;
      out[(size_t)y * w + x] = rate;
    }
  }
  return out;
}

bool operator==(const PatternKey& a, const PatternKey& b) {
  return a.ew == b.ew && a.eh == b.eh && a.generation == b.generation &&
         a.debugMode == b.debugMode && a.eyeTracked == b.eyeTracked &&
         a.drawRadii == b.drawRadii && a.ringWidth == b.ringWidth;
}

size_t PatternKeyHash::operator()(const PatternKey& k) const {
  size_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    h ^= (size_t)v;
    h *= 1099511628211ull;
  };
  mix(k.ew);
  mix(k.eh);
  mix(k.generation);
  mix((uint64_t)k.debugMode);
  mix(k.eyeTracked ? 1u : 0u);
  mix(k.drawRadii);
  mix(k.ringWidth);
  return h;
}

bool gazeCopyRegion(int32_t px, int32_t py, uint32_t pw, uint32_t ph, uint32_t layer, uint32_t texel,
                    uint32_t sw, uint32_t sh, float u, float v, CopyRegion& out) {
  if (texel == 0 || sw == 0 || sh == 0 || pw == 0 || ph == 0) return false;

  const int32_t rx = px / (int32_t)texel;
  const int32_t ry = py / (int32_t)texel;
  const int32_t right = (int32_t)std::min<int64_t>((int64_t)std::ceil((double)(px + (int32_t)pw) / texel), sw);
  const int32_t bottom = (int32_t)std::min<int64_t>((int64_t)std::ceil((double)(py + (int32_t)ph) / texel), sh);
  if (right <= rx || bottom <= ry || rx < 0 || ry < 0) return false;

  const uint32_t rw = (uint32_t)(right - rx);
  const uint32_t rh = (uint32_t)(bottom - ry);

  const int32_t gx = (int32_t)std::lround(u * (float)rw);
  const int32_t gy = (int32_t)std::lround(v * (float)rh);

  // The destination window [rx, rx+rw) x [ry, ry+rh) reads P at
  // [sx, sx+rw) x [sy, sy+rh), where distance from P's center (rw, rh) is the
  // distance from the fovea in destination texels.
  const int32_t sx = std::max(0, std::min((int32_t)rw - gx, (int32_t)rw));
  const int32_t sy = std::max(0, std::min((int32_t)rh - gy, (int32_t)rh));

  out.imageX = rx;
  out.imageY = ry;
  out.imageW = rw;
  out.imageH = rh;
  out.layer = layer;
  out.srcX = (uint32_t)sx;
  out.srcY = (uint32_t)sy;
  out.srcRowLength = 2 * rw;
  out.srcImageHeight = 2 * rh;
  out.bufferOffset = (int64_t)sy * (2 * (int64_t)rw) + sx;
  return true;
}

VkBufferImageCopy toVkBufferImageCopy(const CopyRegion& r) {
  VkBufferImageCopy c{};
  c.bufferOffset = (VkDeviceSize)r.bufferOffset;
  c.bufferRowLength = r.srcRowLength;    // in texels == bytes (R8)
  c.bufferImageHeight = r.srcImageHeight;
  c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  c.imageSubresource.mipLevel = 0;
  c.imageSubresource.baseArrayLayer = r.layer;
  c.imageSubresource.layerCount = 1;
  c.imageOffset.x = r.imageX;
  c.imageOffset.y = r.imageY;
  c.imageOffset.z = 0;
  c.imageExtent.width = r.imageW;
  c.imageExtent.height = r.imageH;
  c.imageExtent.depth = 1;
  return c;
}

}  // namespace framefov
