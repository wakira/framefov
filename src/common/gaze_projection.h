// Gaze direction -> normalized eye-image coordinates.
//
// Pure math, unit tested (03 §5.1).
#pragma once

#include <cmath>

#include "common/xr_math.h"

namespace framefov {

// Tangent of the four half-angles of an XrFovf. angleLeft / angleDown are
// negative.
struct FovTan {
  float left = -1.f, right = 1.f, up = 1.f, down = -1.f;
};

inline FovTan fovToTan(float angleLeft, float angleRight, float angleUp, float angleDown) {
  FovTan f;
  f.left = std::tan(angleLeft);
  f.right = std::tan(angleRight);
  f.up = std::tan(angleUp);
  f.down = std::tan(angleDown);
  return f;
}

struct GazeUv {
  float u = 0.5f, v = 0.5f;  // u right, v down (Vulkan image space)
  bool valid = false;
};

// `dView` is the gaze direction in view space, `qEye` the eye pose orientation
// (view space -> eye space). Returns (0.5, 0.5) with valid=false when the gaze
// points sideways or backwards.
GazeUv projectGaze(const Vec3& dView, const Quat& qEye, const FovTan& fov);

}  // namespace framefov
