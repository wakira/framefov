#include "test_framework.h"

#include "common/gaze_projection.h"

#include <cmath>

using namespace framefov;

namespace {

constexpr float kDeg = 3.14159265358979323846f / 180.f;

Quat yaw(float degrees) { return quatFromAxisAngle({0.f, 1.f, 0.f}, degrees * kDeg); }
Quat pitch(float degrees) { return quatFromAxisAngle({1.f, 0.f, 0.f}, degrees * kDeg); }

}  // namespace

FRAMEFOV_TEST(gaze_projection_straight_ahead) {
  const FovTan fov = fovToTan(-45.f * kDeg, 45.f * kDeg, 45.f * kDeg, -45.f * kDeg);
  const GazeUv r = projectGaze({0.f, 0.f, -1.f}, Quat{}, fov);
  CHECK(r.valid);
  CHECK_NEAR(r.u, 0.5, 1e-5);
  CHECK_NEAR(r.v, 0.5, 1e-5);
}

FRAMEFOV_TEST(gaze_projection_yaw_right) {
  const FovTan fov = fovToTan(-45.f * kDeg, 45.f * kDeg, 45.f * kDeg, -45.f * kDeg);
  // Yaw -22.5 degrees about +Y turns the gaze to the right.
  const Vec3 d = quatRotate(yaw(-22.5f), {0.f, 0.f, -1.f});
  const GazeUv r = projectGaze(d, Quat{}, fov);
  CHECK(r.valid);
  CHECK_NEAR(r.u, (std::tan(22.5f * kDeg) + 1.f) * 0.5f, 1e-4);  // ~0.7071
  CHECK_NEAR(r.v, 0.5, 1e-5);
}

FRAMEFOV_TEST(gaze_projection_pitch_up) {
  const FovTan fov = fovToTan(-45.f * kDeg, 45.f * kDeg, 45.f * kDeg, -45.f * kDeg);
  const Vec3 d = quatRotate(pitch(22.5f), {0.f, 0.f, -1.f});
  const GazeUv r = projectGaze(d, Quat{}, fov);
  CHECK(r.valid);
  CHECK_NEAR(r.u, 0.5, 1e-5);
  CHECK_NEAR(r.v, 1.f - (std::tan(22.5f * kDeg) + 1.f) * 0.5f, 1e-4);  // ~0.2929
}

FRAMEFOV_TEST(gaze_projection_asymmetric_fov) {
  const FovTan fov = fovToTan(-50.f * kDeg, 40.f * kDeg, 40.f * kDeg, -40.f * kDeg);
  const GazeUv r = projectGaze({0.f, 0.f, -1.f}, Quat{}, fov);
  CHECK(r.valid);
  CHECK_NEAR(r.u, std::tan(50.f * kDeg) / (std::tan(50.f * kDeg) + std::tan(40.f * kDeg)), 1e-4);
  CHECK_NEAR(r.v, 0.5, 1e-5);
}

FRAMEFOV_TEST(gaze_projection_canted_eye) {
  const FovTan fov = fovToTan(-45.f * kDeg, 45.f * kDeg, 45.f * kDeg, -45.f * kDeg);
  // Eye canted 10 degrees outward (to the right, the same sense as the yaw_right
  // test): the view-space forward gaze lands left of the eye image center.
  const GazeUv r = projectGaze({0.f, 0.f, -1.f}, yaw(-10.f), fov);
  CHECK(r.valid);
  const float expected = (std::tan(-10.f * kDeg) + 1.f) * 0.5f;
  CHECK_NEAR(r.u, expected, 1e-4);
  CHECK(r.u < 0.5f);
}

FRAMEFOV_TEST(gaze_projection_rejects_backwards) {
  const FovTan fov = fovToTan(-45.f * kDeg, 45.f * kDeg, 45.f * kDeg, -45.f * kDeg);
  const GazeUv r = projectGaze({0.f, 0.f, 1.f}, Quat{}, fov);
  CHECK(!r.valid);
  CHECK_NEAR(r.u, 0.5, 1e-6);
  CHECK_NEAR(r.v, 0.5, 1e-6);
}

FRAMEFOV_TEST(gaze_projection_clamps) {
  const FovTan fov = fovToTan(-20.f * kDeg, 20.f * kDeg, 20.f * kDeg, -20.f * kDeg);
  // 60 degrees off axis is outside the FOV: clamps to the image edge.
  const Vec3 d = quatRotate(yaw(-60.f), {0.f, 0.f, -1.f});
  const GazeUv r = projectGaze(d, Quat{}, fov);
  CHECK(r.valid);
  CHECK_NEAR(r.u, 1.0, 1e-5);
  CHECK_NEAR(r.v, 0.5, 1e-5);
}
