#version 450
// Fallback probe for drivers that do not report the applied rate through
// gl_ShadingRateEXT: writes a value derived from gl_FragCoord, which is constant
// over a multi-pixel fragment. Identical neighbouring bytes therefore mean the
// fragment covered multiple pixels, i.e. a coarse shading rate was applied.
layout(location = 0) out uint outByte;

void main() {
  outByte = uint(int(gl_FragCoord.x + gl_FragCoord.y)) & 0xFFu;
}
