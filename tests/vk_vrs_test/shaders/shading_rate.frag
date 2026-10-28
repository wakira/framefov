#version 450
// Writes the fragment shading rate the driver actually applied to the current
// fragment, encoded the same way the Vulkan API encodes VkFragmentShadingRate:
//   code = (log2(horizontal) << 2) | log2(vertical)
//   (1x1 = 0, 1x2 = 1, 2x1 = 4, 2x2 = 5, 2x4 = 6, 4x2 = 9, 4x4 = 10)
// With attachment-based VRS the values in the output image are the foveation
// pattern the layer injected.
#extension GL_EXT_fragment_shading_rate : require

layout(location = 0) out uint outRateCode;

int log2i(int v) {
  if (v >= 4) return 2;
  if (v == 2) return 1;
  return 0;
}

void main() {
  int flags = gl_ShadingRateEXT;
  // GLSL flags: 1 = 2 vertical, 2 = 4 vertical, 4 = 2 horizontal, 8 = 4 horizontal.
  int vertical = (flags & 1) != 0 ? 2 : ((flags & 2) != 0 ? 4 : 1);
  int horizontal = (flags & 4) != 0 ? 2 : ((flags & 8) != 0 ? 4 : 1);
  outRateCode = uint((log2i(horizontal) << 2) | log2i(vertical));
}
