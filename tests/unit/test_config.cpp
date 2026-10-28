#include "test_framework.h"

#include "common/config.h"

#include <cstdlib>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace framefov;

namespace {

std::string tmpIniPath() {
  static std::string p = "/tmp/framefov-test-" + std::to_string((long)getpid()) + ".ini";
  return p;
}

void writeFile(const std::string& path, const std::string& text) {
  std::ofstream f(path);
  f << text;
}

void clearEnv() {
  static const char* keys[] = {
      "enabled",           "eye_tracking",      "inner_radius",   "outer_radius",
      "fixed_inner_radius", "fixed_outer_radius", "inner_rate",    "middle_rate",
      "outer_rate",        "horizontal_scale",  "fixed_offset_x", "fixed_offset_y",
      "smoothing",         "texel_size",        "size_tolerance", "min_size_ratio",
      "skip_depth_only",   "swapchain_only",    "standalone",     "eye_size",
      "debug_mode",        "stale_behavior",    "log_level"};
  for (const char* k : keys) {
    std::string env = std::string("FRAMEFOV_") + k;
    for (auto& c : env) c = (char)toupper((unsigned char)c);
    unsetenv(env.c_str());
  }
}

}  // namespace

FRAMEFOV_TEST(config_ini_parse) {
  auto sections = parseIni(
      "; comment\n"
      "stray = value\n"
      "[global]\n"
      "inner_radius = 0.25   ; an inline comment is stripped\n"
      "# another comment\n"
      "\n"
      "[app:Hello XR]\n"
      "outer_rate=2x2\n");
  CHECK_EQ(sections.size(), 3u);
  CHECK_EQ(sections[0].name, std::string("global"));  // implicit, from the stray key
  CHECK_EQ(sections[1].name, std::string("global"));
  CHECK_EQ(sections[2].name, std::string("app:Hello XR"));
  CHECK_EQ(sections[1].entries.size(), 1u);
  CHECK_EQ(sections[1].entries[0].first, std::string("inner_radius"));
  CHECK_EQ(sections[1].entries[0].second, std::string("0.25"));
  CHECK_EQ(sections[2].entries[0].first, std::string("outer_rate"));
  CHECK_EQ(sections[2].entries[0].second, std::string("2x2"));
}

FRAMEFOV_TEST(config_rate_parse) {
  Rate r;
  CHECK(parseRate("2x2", r));
  CHECK_EQ((int)r.w, 2);
  CHECK_EQ((int)r.h, 2);
  CHECK(parseRate("4X4", r));
  CHECK_EQ((int)r.w, 4);
  CHECK(parseRate(" 1x2 ", r));
  CHECK_EQ((int)r.w, 1);
  CHECK_EQ((int)r.h, 2);
  // The seven legal sizes.
  for (const char* ok : {"1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4"}) {
    CHECK(parseRate(ok, r));
  }
  // Illegal: 1x4 / 4x1 are not fragment sizes, nor is anything else.
  Rate keep{3, 3};
  for (const char* bad : {"1x4", "4x1", "3x3", "8x8", "1", "x", "1xx2", "0x0"}) {
    keep = Rate{3, 3};
    CHECK(!parseRate(bad, keep));
    CHECK_EQ((int)keep.w, 3);
  }
  CHECK_EQ(rateToString(Rate{4, 2}), std::string("4x2"));
}

FRAMEFOV_TEST(config_key_application) {
  Config c;
  applyConfigKey(c, "inner_radius", "0.5", "test");
  CHECK_NEAR(c.innerRadius, 0.5, 1e-6);
  applyConfigKey(c, "outer_rate", "2x1", "test");
  CHECK_EQ((int)c.outerRate.w, 2);
  CHECK_EQ((int)c.outerRate.h, 1);
  applyConfigKey(c, "skip_depth_only", "no", "test");
  CHECK(!c.skipDepthOnly);
  applyConfigKey(c, "enabled", "off", "test");
  CHECK(!c.enabled);
  applyConfigKey(c, "debug_mode", "2", "test");
  CHECK_EQ(c.debugMode, 2);
  // Invalid values keep the previous value.
  applyConfigKey(c, "outer_rate", "1x4", "test");
  CHECK_EQ((int)c.outerRate.w, 2);
  const Rate before = c.middleRate;
  applyConfigKey(c, "middle_rate", "banana", "test");
  CHECK(before == c.middleRate);
}

FRAMEFOV_TEST(config_draw_radii) {
  Config c;
  CHECK_EQ(c.drawRadii, 0u);
  CHECK_EQ(c.ringWidth, 2u);
  applyConfigKey(c, "draw_radii", "both", "test");
  CHECK_EQ(c.drawRadii, 1u);
  applyConfigKey(c, "draw_radii", "inner", "test");
  CHECK_EQ(c.drawRadii, 2u);
  applyConfigKey(c, "draw_radii", "outer", "test");
  CHECK_EQ(c.drawRadii, 3u);
  applyConfigKey(c, "draw_radii", "off", "test");
  CHECK_EQ(c.drawRadii, 0u);
  applyConfigKey(c, "draw_radii", "on", "test");
  CHECK_EQ(c.drawRadii, 1u);
  applyConfigKey(c, "draw_radii", "2", "test");
  CHECK_EQ(c.drawRadii, 2u);
  applyConfigKey(c, "draw_radii", "banana", "test");  // keeps the previous value
  CHECK_EQ(c.drawRadii, 2u);
  applyConfigKey(c, "ring_width", "3", "test");
  CHECK_EQ(c.ringWidth, 3u);

  c.drawRadii = 9;
  c.ringWidth = 100;
  validateConfig(c);
  CHECK_EQ(c.drawRadii, 3u);
  CHECK_EQ(c.ringWidth, 8u);
  c.ringWidth = 0;
  validateConfig(c);
  CHECK_EQ(c.ringWidth, 1u);

  clearEnv();
  setenv("FRAMEFOV_DRAW_RADII", "both", 1);
  setenv("FRAMEFOV_RING_WIDTH", "4", 1);
  Config e = loadConfigFromEnv(Config{}, 1);
  CHECK_EQ(e.drawRadii, 1u);
  CHECK_EQ(e.ringWidth, 4u);
  clearEnv();

  // The summary mentions the overlay so it can be confirmed from the log.
  c.drawRadii = 1;
  c.ringWidth = 2;
  CHECK(configSummary(c).find("rings both x2") != std::string::npos);
}

FRAMEFOV_TEST(config_standalone_and_eye_size) {
  uint32_t w = 0, h = 0;
  CHECK(parseSize("2432x2304", w, h));
  CHECK_EQ(w, 2432u);
  CHECK_EQ(h, 2304u);
  CHECK(parseSize(" 1024X768 ", w, h));  // trimmed, case-insensitive
  CHECK_EQ(w, 1024u);
  CHECK_EQ(h, 768u);
  CHECK(!parseSize("1024", w, h));
  CHECK(!parseSize("0x0", w, h));
  CHECK(!parseSize("axb", w, h));
  CHECK(!parseSize("1024x", w, h));

  Config c;
  applyConfigKey(c, "standalone", "1", "test");
  CHECK(c.standalone);
  applyConfigKey(c, "eye_size", "1024x1920", "test");
  CHECK_EQ(c.eyeWidth, 1024u);
  CHECK_EQ(c.eyeHeight, 1920u);
  applyConfigKey(c, "eye_size", "nonsense", "test");  // keeps the previous value
  CHECK_EQ(c.eyeWidth, 1024u);
  CHECK_EQ(c.eyeHeight, 1920u);

  // standalone matches targets by size, so swapchain_only cannot apply
  c.swapchainOnly = true;
  validateConfig(c);
  CHECK(!c.swapchainOnly);

  c.eyeWidth = 100000;
  c.eyeHeight = 4;
  validateConfig(c);
  CHECK_EQ(c.eyeWidth, 16384u);
  CHECK_EQ(c.eyeHeight, 16u);

  // "no size filter" spellings, in the file and as an override
  Config z;
  for (const char* v : {"0x0", "auto", "any", "none", "AUTO", " 0x0 "}) {
    z.eyeWidth = 1234;
    z.eyeHeight = 5678;
    applyConfigKey(z, "eye_size", v, "test");
    CHECK_EQ(z.eyeWidth, 0u);
    CHECK_EQ(z.eyeHeight, 0u);
  }
  z.eyeWidth = 100;
  applyConfigKey(z, "eye_size", "rubbish", "test");  // invalid keeps the previous value
  CHECK_EQ(z.eyeWidth, 100u);

  clearEnv();
  setenv("FRAMEFOV_STANDALONE", "1", 1);
  setenv("FRAMEFOV_EYE_SIZE", "800x600", 1);
  Config e = loadConfigFromEnv(Config{}, 3);
  CHECK(e.standalone);
  CHECK_EQ(e.eyeWidth, 800u);
  CHECK_EQ(e.eyeHeight, 600u);
  CHECK_EQ(e.generation, 3u);
  clearEnv();
}

FRAMEFOV_TEST(config_validation) {
  Config c;
  c.innerRadius = 5.f;
  c.outerRadius = -2.f;
  c.smoothing = 2.f;
  c.debugMode = 9;
  c.texelSize = 12;  // not a power of two
  c.staleBehavior = "nope";
  c.logLevel = "loud";
  validateConfig(c);
  CHECK_NEAR(c.innerRadius, 3.f, 1e-6);
  CHECK_NEAR(c.outerRadius, 3.f, 1e-6);  // clamped up to innerRadius
  CHECK_NEAR(c.smoothing, 0.99f, 1e-6);
  CHECK_EQ(c.debugMode, 2);
  CHECK_EQ(c.texelSize, 0u);
  CHECK_EQ(c.staleBehavior, std::string("fixed"));
  CHECK_EQ(c.logLevel, std::string("info"));

  Config d;
  d.innerRadius = 0.6f;
  d.outerRadius = 0.3f;
  validateConfig(d);
  CHECK(d.outerRadius >= d.innerRadius);
}

FRAMEFOV_TEST(config_file_precedence) {
  const std::string path = tmpIniPath();
  writeFile(path,
            "[global]\n"
            "outer_rate = 2x2\n"
            "inner_radius = 0.4\n"
            "[exe:mysim.exe]\n"
            "outer_rate = 2x1\n"
            "[app:Hello XR]\n"
            "outer_rate = 4x2\n");
  clearEnv();
  Config c = loadConfig("Hello XR", "mysim.exe", 7, path);
  CHECK_EQ((int)c.outerRate.w, 4);
  CHECK_EQ((int)c.outerRate.h, 2);
  CHECK_NEAR(c.innerRadius, 0.4, 1e-6);
  CHECK_EQ(c.generation, 7u);

  // Only the exe section matches.
  Config d = loadConfig("Other App", "mysim.exe", 0, path);
  CHECK_EQ((int)d.outerRate.w, 2);
  CHECK_EQ((int)d.outerRate.h, 1);

  // Nothing matches beyond [global]; matching is case-insensitive.
  Config e = loadConfig("HELLO xr", "MYSIM.EXE", 0, path);
  CHECK_EQ((int)e.outerRate.w, 4);
  Config f = loadConfig("Nobody", "nothing", 0, path);
  CHECK_EQ((int)f.outerRate.w, 2);
  CHECK_EQ((int)f.outerRate.h, 2);

  unlink(path.c_str());
}

FRAMEFOV_TEST(config_env_overrides_file) {
  const std::string path = tmpIniPath();
  writeFile(path, "[global]\nouter_rate = 2x2\ninner_rate = 2x2\n");
  clearEnv();
  setenv("FRAMEFOV_OUTER_RATE", "4x2", 1);
  setenv("FRAMEFOV_INNER_RATE", "1x1", 1);
  Config c = loadConfig("app", "exe", 0, path);
  CHECK_EQ((int)c.outerRate.w, 4);
  CHECK_EQ((int)c.outerRate.h, 2);
  CHECK_EQ((int)c.innerRate.w, 1);
  CHECK_EQ((int)c.innerRate.h, 1);
  clearEnv();
  unlink(path.c_str());
}

FRAMEFOV_TEST(config_missing_file_is_defaults) {
  clearEnv();
  Config c = loadConfig("app", "exe", 3, "/nonexistent/framefov/settings.ini");
  CHECK_EQ(c.generation, 3u);
  CHECK(c.enabled);
  CHECK_EQ((int)c.middleRate.w, 2);
}

FRAMEFOV_TEST(config_env_flag) {
  unsetenv("FRAMEFOV_TEST_FLAG");
  CHECK(!envFlag("FRAMEFOV_TEST_FLAG"));
  setenv("FRAMEFOV_TEST_FLAG", "0", 1);
  CHECK(!envFlag("FRAMEFOV_TEST_FLAG"));
  setenv("FRAMEFOV_TEST_FLAG", "1", 1);
  CHECK(envFlag("FRAMEFOV_TEST_FLAG"));
  setenv("FRAMEFOV_TEST_FLAG", "yes", 1);
  CHECK(envFlag("FRAMEFOV_TEST_FLAG"));
  unsetenv("FRAMEFOV_TEST_FLAG");
}

FRAMEFOV_TEST(config_preset_switch) {
  // Valve's profiles: "med"/"hi" replace unset radii and rates, explicit keys win.
  Config lo;
  CHECK(lo.preset == "lo");
  CHECK_NEAR(lo.innerRadius, 0.286, 1e-4);

  Config med;
  applyConfigKey(med, "preset", "med", "test");
  CHECK(med.preset == "med");

  Config hi;
  applyConfigKey(hi, "preset", "hi", "test");
  CHECK(hi.preset == "hi");

  Config bad;
  applyConfigKey(bad, "preset", "ultra", "test");
  CHECK(bad.preset == "lo");  // unknown values are rejected, not applied

  // Explicit radius/rate keys still override a preset (CTest covers the layering
  // through loadConfigFromEnv).
  Config tuned;
  tuned.preset = "hi";
  applyConfigKey(tuned, "inner_radius", "0.30", "test");
  CHECK_NEAR(tuned.innerRadius, 0.30, 1e-6);
}
