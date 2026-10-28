// Configuration: defaults, INI file with per-app/per-exe sections, environment
// overrides, validation. Pure apart from reading the file system.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace framefov {

struct Rate {
  uint8_t w = 1, h = 1;
};

bool operator==(const Rate& a, const Rate& b);
inline bool operator!=(const Rate& a, const Rate& b) { return !(a == b); }

struct Config {
  bool enabled = true;
  bool eyeTracking = true;
  // Radii in normalized eye-image units (the image spans [-1,1] in x and y).
  // Radii in normalized eye-image units (the image spans [-1,1] in x and y): the
  // covered width fraction is innerRadius * horizontalScale, the height fraction
  // is innerRadius.
  //
  // Defaults reproduce Valve's `lo` preset — the one VALVE_fdm_injection uses when
  // FDM_DEBUG has `enable` and names no preset: a full-density rectangle of
  // .295 x .286 of the image at the centre, with a single ~2x2 level outside and
  // no 4x4. Our geometry is an ellipse, so 0.286 with hscale 0.295/0.286 = 1.03
  // gives the same footprint (the ellipses inscribed in Valve's rectangle cover
  // 6.6% of the eye versus the rectangle's 8.4%). outerRadius stays below the
  // corner distance so the 4x4 band never appears; raise middle/outer rates or
  // shrink the radii to be more aggressive.
  float innerRadius = 0.286f, outerRadius = 0.95f;            // eye-tracked
  float fixedInnerRadius = 0.286f, fixedOuterRadius = 0.95f;  // fixed foveation
  Rate innerRate{1, 1}, middleRate{2, 2}, outerRate{2, 2};
  float horizontalScale = 1.03f;
  float fixedOffsetX = 0.0f, fixedOffsetY = 0.0f;  // fixed-mode gaze offset, tangent units
  float smoothing = 0.0f;                          // 0 = none, 0.9 = heavy (EMA factor)
  // Some stacks present the eye image flipped vertically relative to the gaze
  // convention (composition flip). Set to mirror v after conversion.
  bool gazeYFlip = false;
  // Named geometry preset, matching Valve's FDM profiles. "lo" is the built-in
  // default (what VALVE_fdm_injection uses); "med"/"hi" replace the unset radii
  // and rates, and any explicit radius/rate key (file or env) still wins.
  std::string preset = "lo";
  uint32_t texelSize = 0;                          // 0 = auto
  float sizeTolerance = 0.02f;
  float minSizeRatio = 0.5f;
  bool skipDepthOnly = true;
  bool swapchainOnly = false;
  // Steam Frame: the layer always runs standalone (no OpenXR half). Targets are
  // matched by size and foveated with a fixed gaze until eye tracking lands.
  bool standalone = true;
  uint32_t eyeWidth = 0, eyeHeight = 0;  // expected per-eye render size (0 = any)
  int debugMode = 0;  // 0 off, 1 = outer rate everywhere, 2 = inverted
  // Debug overlay: draw a thin band at each radius so the fovea can be found by
  // eye. It is a shading-rate band, not colour: the ring shows up as a line of
  // different sharpness. 0 off, 1 both, 2 inner only, 3 outer only.
  uint32_t drawRadii = 0;
  uint32_t ringWidth = 2;  // band width in rate-image texels
  std::string staleBehavior = "fixed";  // "fixed" | "neutral"
  std::string logLevel = "info";
  uint64_t generation = 0;  // bumped on every reload
};

struct IniSection {
  std::string name;
  std::vector<std::pair<std::string, std::string>> entries;
};

// Parses INI text: [section], key = value, ';' and '#' comments, trimmed.
std::vector<IniSection> parseIni(const std::string& text);

// "1x1", "2x2", "4x4", "2x1", "1x2", "2x4", "4x2" -> Rate. Returns false and
// leaves `out` untouched when the text is not one of the seven valid rates.
bool parseRate(const std::string& text, Rate& out);
std::string rateToString(const Rate& r);

// "WxH" (for example "2432x2304") -> width/height. Returns false when the text
// is not of that form or either side is zero.
bool parseSize(const std::string& text, uint32_t& width, uint32_t& height);

// $XDG_CONFIG_HOME/framefov/settings.ini, defaulting to ~/.config/...
std::string defaultConfigPath();
// mtime in ns, or 0 when the file does not exist.
int64_t fileMtimeNs(const std::string& path);

// Basenames of /proc/self/exe and of every /proc/self/cmdline token (this covers
// Proton, where argv[0] is a Windows path and the real exe is elsewhere).
std::vector<std::string> processExeNames();

// True when the env var is set to something that is not "", "0", "false" or "no".
bool envFlag(const char* name);

// Loads defaults, then the config file (global -> exe -> app), then env
// overrides (FRAMEFOV_<UPPER_SNAKE_KEY>), then validates. Only the sections
// matching `appName` / `exeName` are applied. `generation` is stored as-is.
// Applies the resulting log level to the logging module.
Config loadConfig(const std::string& appName, const std::string& exeName, uint64_t generation = 0,
                  const std::string& path = defaultConfigPath());

// Same, but matching every candidate process name returned by processExeNames().
Config loadConfigForProcess(const std::string& appName, uint64_t generation = 0,
                            const std::string& path = defaultConfigPath());

// Same layering but without the file (used before the app name is known).
Config loadConfigFromEnv(Config base, uint64_t generation = 0);

void validateConfig(Config& c);

// One-line description of the effective settings, for the log.
std::string configSummary(const Config& c);

// Applies one key/value pair (snake_case key). Unknown keys are ignored with a
// warning. Exposed for tests.
void applyConfigKey(Config& c, const std::string& key, const std::string& value,
                    const std::string& origin);

}  // namespace framefov
