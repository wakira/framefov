#include "common/gaze_projection.h"

#include <algorithm>

namespace framefov {

GazeUv projectGaze(const Vec3& dView, const Quat& qEye, const FovTan& fov) {
  GazeUv out;
  const Vec3 dEye = quatRotate(quatConjugate(qEye), dView);
  if (dEye.z > -1e-4f) return out;  // looking sideways or backwards
  const float tx = dEye.x / -dEye.z;
  const float ty = dEye.y / -dEye.z;
  const float du = fov.right - fov.left;
  const float dv = fov.up - fov.down;
  if (std::fabs(du) < 1e-9f || std::fabs(dv) < 1e-9f) return out;
  out.u = std::min(1.f, std::max(0.f, (tx - fov.left) / du));
  out.v = std::min(1.f, std::max(0.f, (fov.up - ty) / dv));
  out.valid = true;
  return out;
}

}  // namespace framefov
