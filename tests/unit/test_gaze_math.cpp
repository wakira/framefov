#include "test_framework.h"

#include "common/gaze_math.h"

using namespace framefov;

FRAMEFOV_TEST(gaze_math_openvr_ndc_to_uv) {
  float u = 0.f, v = 0.f;
  // Centre of the eye image.
  openvrNdcToUv(0.f, 0.f, u, v);
  CHECK_NEAR(u, 0.5, 1e-6);
  CHECK_NEAR(v, 0.5, 1e-6);
  // NDC is -1..1 with +y up; our UV has v measured downwards from the top.
  openvrNdcToUv(-1.f, 1.f, u, v);
  CHECK_NEAR(u, 0.0, 1e-6);
  CHECK_NEAR(v, 0.0, 1e-6);
  openvrNdcToUv(1.f, -1.f, u, v);
  CHECK_NEAR(u, 1.0, 1e-6);
  CHECK_NEAR(v, 1.0, 1e-6);
  // Looking up (positive y) puts the fovea higher in the image (smaller v).
  openvrNdcToUv(0.f, 0.5f, u, v);
  CHECK_NEAR(v, 0.25, 1e-6);
  // Out-of-range input is clamped to the eye image.
  openvrNdcToUv(-8.f, 8.f, u, v);
  CHECK_NEAR(u, 0.0, 1e-6);
  CHECK_NEAR(v, 0.0, 1e-6);
  openvrNdcToUv(8.f, -8.f, u, v);
  CHECK_NEAR(u, 1.0, 1e-6);
  CHECK_NEAR(v, 1.0, 1e-6);
}
