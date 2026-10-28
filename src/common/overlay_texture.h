// Ring texture for the OpenVR overlay tool (tools/overlay.cpp).
//
// Pure image generation, so it is unit tested and needs no VR. One texture holds
// one ring, drawn as an ellipse whose semi axes are in texture fractions (1.0 =
// half the texture), which is how a radius in the layer's normalised eye-image
// units maps onto a quad that spans the eye's field of view.
#pragma once

#include <cstdint>
#include <vector>

namespace framefov {

// Packed 0xRRGGBB + alpha -> 0..1 components.
void unpackColor(uint32_t rgb, float alpha, float& r, float& g, float& b, float& a);

// What to fill the texture with: the ring, or a diagnostic pattern that shows how
// the quad maps its texture onto the screen (straight squares = correct mapping,
// diagonal stripes or a diamond = something is off, and the extent shows the
// quad's actual size and position).
enum class TexturePattern { Ring = 0, Checker = 1, Solid = 2 };

struct RingTextureParams {
  TexturePattern pattern = TexturePattern::Ring;
  uint32_t checkerSquares = 16;  // across, for TexturePattern::Checker
  uint32_t width = 1024;     // RGBA8; height may differ so the texture matches the
  uint32_t height = 1024;    // quad's aspect, which is the eye's field of view
  float radiusX = 0.25f;     // texture fractions, 1.0 = half the texture
  float radiusY = 0.25f;
  float thickness = 0.02f;   // radial thickness, same units
  float r = 0.0f, g = 1.0f, b = 0.0f, a = 1.0f;
};

// Top-left origin, tightly packed RGBA8, transparent outside the ring. Edges are
// antialiased over roughly one pixel.
std::vector<uint8_t> buildRingTexture(const RingTextureParams& p);

}  // namespace framefov
