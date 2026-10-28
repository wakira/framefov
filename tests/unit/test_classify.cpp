#include "test_framework.h"

#include "common/classify.h"

#include <memory>

using namespace framefov;

namespace {

Snapshot makeSnapshot() {
  Snapshot s;
  s.active = true;
  s.publishTimeNs = nowNs();
  s.eyeW = 1832;
  s.eyeH = 1920;
  s.eyes[0] = EyeView{true, 0.25f, 0.30f, true};
  s.eyes[1] = EyeView{true, 0.75f, 0.70f, true};
  s.config = std::make_shared<Config>();
  return s;
}

TargetDesc makeTarget(uint32_t w, uint32_t h, bool color = true) {
  TargetDesc t;
  t.width = w;
  t.height = h;
  t.hasColor = color;
  t.vkDevice = 0x1234;
  return t;
}

}  // namespace

FRAMEFOV_TEST(classify_multiview_stereo) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);
  t.viewMask = 0b11;
  t.layers = 2;

  FoveationPlan plan = planForTarget(t, s, 2);
  CHECK_EQ(plan.regions.size(), 2u);
  CHECK_EQ(plan.regions[0].layer, 0u);
  CHECK_EQ(plan.regions[1].layer, 1u);
  CHECK_NEAR(plan.regions[0].u, 0.25, 1e-6);
  CHECK_NEAR(plan.regions[1].u, 0.75, 1e-6);
  CHECK_EQ(plan.regions[0].w, 1832u);
  CHECK_EQ(plan.regions[0].h, 1920u);

  // A single-layer rate image collapses both eyes onto layer 0 with the
  // averaged gaze.
  FoveationPlan single = planForTarget(t, s, 1);
  CHECK_EQ(single.regions.size(), 1u);
  CHECK_EQ(single.regions[0].layer, 0u);
  CHECK_NEAR(single.regions[0].u, 0.5, 1e-6);
  CHECK_NEAR(single.regions[0].v, 0.5, 1e-6);
}

FRAMEFOV_TEST(classify_side_by_side) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(3664, 1920);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 2u);
  CHECK_EQ(plan.regions[0].x, 0);
  CHECK_EQ(plan.regions[0].w, 1832u);
  CHECK_EQ(plan.regions[1].x, 1832);
  CHECK_NEAR(plan.regions[0].u, 0.25, 1e-6);
  CHECK_NEAR(plan.regions[1].u, 0.75, 1e-6);
}

FRAMEFOV_TEST(classify_eye_sized_single_target) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);  // average of both eyes
  CHECK_NEAR(plan.regions[0].v, 0.5, 1e-6);
}

FRAMEFOV_TEST(classify_rejects_shadow_map) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(2048, 2048);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 0u);
  CHECK(!isEyeCandidateSize(2048, 2048, s));
  CHECK(isEyeCandidateSize(1832, 1920, s));
  CHECK(isEyeCandidateSize(916, 960, s));   // >= 0.5 * recommended width
  CHECK(!isEyeCandidateSize(400, 420, s));  // too small
}

FRAMEFOV_TEST(classify_rejects_depth_only) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920, /*color=*/false);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 0u);

  // With skipDepthOnly off the same target is foveated.
  Config cfg;
  cfg.skipDepthOnly = false;
  s.config = std::make_shared<Config>(cfg);
  plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
}

FRAMEFOV_TEST(classify_stale_gaze_modes) {
  Snapshot s = makeSnapshot();
  s.publishTimeNs = nowNs() - 500 * 1000 * 1000;  // 500 ms ago
  TargetDesc t = makeTarget(1832, 1920);

  // default staleBehavior = "fixed": center, still foveated
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);
  CHECK_NEAR(plan.regions[0].v, 0.5, 1e-6);

  // invalid eyes + "neutral": no foveation at all
  Config cfg;
  cfg.staleBehavior = "neutral";
  s.config = std::make_shared<Config>(cfg);
  s.eyes[0].valid = false;
  s.eyes[1].valid = false;
  plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 0u);
}

FRAMEFOV_TEST(classify_synthetic_any_device) {
  // A synthesized snapshot (standalone/fake-eye) describes the whole process, so
  // it must foveate every device in it - a game and SteamVR's vrclient each create
  // one, and tying the snapshot to the last one left the game unfoveated.
  Snapshot s = makeSnapshot();
  s.synthetic = true;
  s.anyDevice = true;
  TargetDesc t = makeTarget(1832, 1920);
  t.vkDevice = 0x2222;
  CHECK_EQ(planForTarget(t, s, 1).regions.size(), 1u);
}

FRAMEFOV_TEST(classify_preconditions) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);

  // A synthesized snapshot applies to every device in the process.
  TargetDesc otherDevice = t;
  otherDevice.vkDevice = 0x9999;
  CHECK_EQ(planForTarget(otherDevice, s, 1).regions.size(), 1u);

  Snapshot inactive = s;
  inactive.active = false;
  CHECK_EQ(planForTarget(t, inactive, 1).regions.size(), 0u);

  Config off;
  off.enabled = false;
  Snapshot disabled = s;
  disabled.config = std::make_shared<Config>(off);
  CHECK_EQ(planForTarget(t, disabled, 1).regions.size(), 0u);

  // swapchainOnly disables the size heuristics (C and D) but not multiview (B).
  Config scOnly;
  scOnly.swapchainOnly = true;
  Snapshot only = s;
  only.config = std::make_shared<Config>(scOnly);
  CHECK_EQ(planForTarget(t, only, 1).regions.size(), 0u);
  TargetDesc mv = makeTarget(1832, 1920);
  mv.viewMask = 0b11;
  mv.layers = 2;
  CHECK_EQ(planForTarget(mv, only, 2).regions.size(), 2u);
}

FRAMEFOV_TEST(classify_known_size_heuristic_with_no_reference) {
  // No reference size: everything is a candidate.
  Snapshot s = makeSnapshot();
  s.eyeW = s.eyeH = 0;
  CHECK(isEyeCandidateSize(1920, 1080, s));
  TargetDesc t = makeTarget(1920, 1080);
  CHECK_EQ(planForTarget(t, s, 1).regions.size(), 1u);
}

FRAMEFOV_TEST(classify_doublewide_prefers_side_by_side) {
  // A known per-eye size: 3840x1920 must split, not become one centred region.
  Snapshot s = makeSnapshot();
  s.eyeW = 1920;
  s.eyeH = 1920;
  TargetDesc t = makeTarget(3840, 1920);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 2u);
  CHECK_EQ(plan.regions[0].w, 1920u);
  CHECK_EQ(plan.regions[1].x, 1920);
  CHECK(plan.reason == "side-by-side");

  // The per-eye target itself stays one region.
  TargetDesc one = makeTarget(1920, 1920);
  CHECK_EQ(planForTarget(one, s, 1).regions.size(), 1u);
}

FRAMEFOV_TEST(classify_doublewide_without_eye_size) {
  // No reference size: a ~2:1 target is two eyes; 16:9 stays a single target.
  Snapshot s = makeSnapshot();
  s.eyeW = s.eyeH = 0;
  CHECK_EQ(planForTarget(makeTarget(3840, 1920), s, 1).regions.size(), 2u);
  CHECK_EQ(planForTarget(makeTarget(3584, 1728), s, 1).regions.size(), 2u);
  CHECK_EQ(planForTarget(makeTarget(1920, 1080), s, 1).regions.size(), 1u);
  CHECK_EQ(planForTarget(makeTarget(1280, 720), s, 1).regions.size(), 1u);
}
