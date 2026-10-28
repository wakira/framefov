// Small pure conversions between gaze conventions. Header-only so the unit tests
// can use them without pulling in the OpenVR headers.
#pragma once

namespace framefov {

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// OpenVR eye-tracked foveation centre (NDC, -1..1, +y up) to our eye-image UV
// (0..1, v measured from the top of the eye image) - the convention the gaze in
// the shared snapshot uses, and therefore the one the pattern is placed by.
inline void openvrNdcToUv(float ndcX, float ndcY, float& u, float& v) {
  u = clamp01(0.5f * (ndcX + 1.0f));
  v = clamp01(0.5f * (1.0f - ndcY));
}

}  // namespace framefov
