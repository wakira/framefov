#include "common/config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"

namespace framefov {

bool operator==(const Rate& a, const Rate& b) { return a.w == b.w && a.h == b.h; }

namespace {

std::string trim(const std::string& s) {  size_t b = 0, e = s.size();
  while (b < e && isspace((unsigned char)s[b])) ++b;
  while (e > b && isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::string toLower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string toUpper(std::string s) {
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::string baseName(const std::string& p) {
  size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool parseBool(const std::string& v, bool& out) {
  std::string s = toLower(trim(v));
  if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
  if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
  return false;
}

bool parseFloat(const std::string& v, float& out) {
  std::string s = trim(v);
  if (s.empty()) return false;
  char* end = nullptr;
  float f = strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return false;
  out = f;
  return true;
}

bool parseUint(const std::string& v, uint32_t& out) {
  std::string s = trim(v);
  if (s.empty()) return false;
  char* end = nullptr;
  unsigned long l = strtoul(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || l > 0xffffffffUL) return false;
  out = (uint32_t)l;
  return true;
}

bool parseInt(const std::string& v, int& out) {
  std::string s = trim(v);
  if (s.empty()) return false;
  char* end = nullptr;
  long l = strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0') return false;
  out = (int)l;
  return true;
}

bool isPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

const char* drawRadiiName(uint32_t v) {
  switch (v) {
    case 1: return "both";
    case 2: return "inner";
    case 3: return "outer";
    default: return "off";
  }
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Every canonical key name, in the same spelling as the INI file. Also used to
// build the environment variable names.
const char* const kKeys[] = {
    "enabled",           "eye_tracking",      "inner_radius",   "outer_radius",
    "fixed_inner_radius", "fixed_outer_radius", "inner_rate",    "middle_rate",
    "outer_rate",        "horizontal_scale",  "fixed_offset_x", "fixed_offset_y",
    "smoothing",         "texel_size",        "size_tolerance", "min_size_ratio",
    "skip_depth_only",   "swapchain_only",    "standalone",     "eye_size",
    "debug_mode",        "draw_radii",        "ring_width",     "stale_behavior",
    "gaze_yflip",        "preset",           "log_level"};

bool knownKey(const std::string& k) {
  for (const char* known : kKeys) {
    if (k == known) return true;
  }
  return false;
}

}  // namespace

std::vector<IniSection> parseIni(const std::string& text) {
  std::vector<IniSection> out;
  IniSection* cur = nullptr;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string s = trim(line);
    if (s.empty()) continue;
    if (s[0] == ';' || s[0] == '#') continue;
    if (s[0] == '[') {
      size_t close = s.find(']');
      if (close == std::string::npos) continue;
      std::string name = trim(s.substr(1, close - 1));
      out.push_back(IniSection{name, {}});
      cur = &out.back();
      continue;
    }
    size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    std::string key = toLower(trim(s.substr(0, eq)));
    std::string value = trim(s.substr(eq + 1));
    // An inline comment starts at the first unquoted ';' or '#'.
    const size_t comment = value.find_first_of(";#");
    if (comment != std::string::npos) value = trim(value.substr(0, comment));
    if (key.empty()) continue;
    if (!cur) {
      // Keys before any section header land in an implicit [global].
      out.push_back(IniSection{"global", {}});
      cur = &out.back();
    }
    cur->entries.emplace_back(key, value);
  }
  return out;
}

bool parseRate(const std::string& text, Rate& out) {
  std::string s = toLower(trim(text));
  size_t x = s.find('x');
  if (x == std::string::npos) return false;
  std::string ws = s.substr(0, x), hs = s.substr(x + 1);
  auto parseOne = [](const std::string& t, uint8_t& v) {
    if (t.size() != 1) return false;
    if (t[0] != '1' && t[0] != '2' && t[0] != '4') return false;
    v = (uint8_t)(t[0] - '0');
    return true;
  };
  Rate r;
  if (!parseOne(ws, r.w) || !parseOne(hs, r.h)) return false;
  // The seven legal fragment sizes: 1x1 1x2 2x1 2x2 2x4 4x2 4x4.
  if (r.w == 4 && r.h == 1) return false;
  if (r.w == 1 && r.h == 4) return false;
  out = r;
  return true;
}

std::string rateToString(const Rate& r) {
  std::string s;
  s += (char)('0' + r.w);
  s += 'x';
  s += (char)('0' + r.h);
  return s;
}

bool parseSize(const std::string& text, uint32_t& width, uint32_t& height) {
  std::string s = toLower(trim(text));
  size_t x = s.find('x');
  if (x == std::string::npos) return false;
  const std::string ws = trim(s.substr(0, x));
  const std::string hs = trim(s.substr(x + 1));
  if (ws.empty() || hs.empty()) return false;
  char* end = nullptr;
  const unsigned long w = strtoul(ws.c_str(), &end, 10);
  if (end == ws.c_str() || *end != '\0') return false;
  const unsigned long h = strtoul(hs.c_str(), &end, 10);
  if (end == hs.c_str() || *end != '\0') return false;
  if (w == 0 || h == 0 || w > 0xffffffffUL || h > 0xffffffffUL) return false;
  width = (uint32_t)w;
  height = (uint32_t)h;
  return true;
}

std::string defaultConfigPath() {
  const char* xdg = getenv("XDG_CONFIG_HOME");
  std::string dir = (xdg && *xdg) ? std::string(xdg) : (std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config");
  return dir + "/framefov/settings.ini";
}

int64_t fileMtimeNs(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
}

std::vector<std::string> processExeNames() {
  std::vector<std::string> names;
  char buf[PATH_MAX];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    names.push_back(baseName(buf));
  }
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  if (f) {
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < all.size()) {
      size_t end = all.find('\0', start);
      if (end == std::string::npos) end = all.size();
      if (end > start) names.push_back(baseName(all.substr(start, end - start)));
      start = end + 1;
    }
  }
  return names;
}

bool envFlag(const char* name) {
  const char* v = getenv(name);
  if (!v || !*v) return false;
  std::string s = toLower(v);
  return !(s == "0" || s == "false" || s == "no" || s == "off");
}

void applyConfigKey(Config& c, const std::string& keyIn, const std::string& value,
                    const std::string& origin) {
  const std::string key = toLower(trim(keyIn));
  const std::string where = origin.empty() ? std::string("config") : origin;
  bool b = false;
  float f = 0.f;
  uint32_t u = 0;
  int i = 0;
  Rate r;

  if (key == "enabled") {
    if (!parseBool(value, b)) goto bad;
    c.enabled = b;
  } else if (key == "eye_tracking") {
    if (!parseBool(value, b)) goto bad;
    c.eyeTracking = b;
  } else if (key == "inner_radius") {
    if (!parseFloat(value, f)) goto bad;
    c.innerRadius = f;
  } else if (key == "outer_radius") {
    if (!parseFloat(value, f)) goto bad;
    c.outerRadius = f;
  } else if (key == "fixed_inner_radius") {
    if (!parseFloat(value, f)) goto bad;
    c.fixedInnerRadius = f;
  } else if (key == "fixed_outer_radius") {
    if (!parseFloat(value, f)) goto bad;
    c.fixedOuterRadius = f;
  } else if (key == "inner_rate") {
    if (!parseRate(value, r)) goto bad_rate;
    c.innerRate = r;
  } else if (key == "middle_rate") {
    if (!parseRate(value, r)) goto bad_rate;
    c.middleRate = r;
  } else if (key == "outer_rate") {
    if (!parseRate(value, r)) goto bad_rate;
    c.outerRate = r;
  } else if (key == "horizontal_scale") {
    if (!parseFloat(value, f)) goto bad;
    c.horizontalScale = f;
  } else if (key == "fixed_offset_x") {
    if (!parseFloat(value, f)) goto bad;
    c.fixedOffsetX = f;
  } else if (key == "fixed_offset_y") {
    if (!parseFloat(value, f)) goto bad;
    c.fixedOffsetY = f;
  } else if (key == "smoothing") {
    if (!parseFloat(value, f)) goto bad;
    c.smoothing = f;
  } else if (key == "texel_size") {
    if (!parseUint(value, u)) goto bad;
    c.texelSize = u;
  } else if (key == "size_tolerance") {
    if (!parseFloat(value, f)) goto bad;
    c.sizeTolerance = f;
  } else if (key == "min_size_ratio") {
    if (!parseFloat(value, f)) goto bad;
    c.minSizeRatio = f;
  } else if (key == "skip_depth_only") {
    if (!parseBool(value, b)) goto bad;
    c.skipDepthOnly = b;
  } else if (key == "swapchain_only") {
    if (!parseBool(value, b)) goto bad;
    c.swapchainOnly = b;
  } else if (key == "standalone") {
    if (!parseBool(value, b)) goto bad;
    c.standalone = b;
  } else if (key == "gaze_yflip") {
    if (!parseBool(value, b)) goto bad;
    c.gazeYFlip = b;
  } else if (key == "preset") {
    const std::string v = toLower(trim(value));
    if (v == "lo" || v == "default" || v == "low") {
      c.preset = "lo";
    } else if (v == "med" || v == "medium") {
      c.preset = "med";
    } else if (v == "hi" || v == "high") {
      c.preset = "hi";
    } else {
      goto bad;
    }
  } else if (key == "eye_size") {
    // "0x0", "auto" and "any" mean "no size filter": every colour target that
    // passes skip_depth_only is treated as an eye target. Any of these can also
    // be used as an environment override to clear a value from the file.
    const std::string v = toLower(trim(value));
    if (v.empty() || v == "0x0" || v == "auto" || v == "any" || v == "none") {
      c.eyeWidth = 0;
      c.eyeHeight = 0;
      return;
    }
    uint32_t w = 0, h = 0;
    if (!parseSize(v, w, h)) {
      LOG_WARN("%s: '%s' is not a WxH size (or 0x0/auto); ignoring '%s'", where.c_str(),
               value.c_str(), key.c_str());
      return;
    }
    c.eyeWidth = w;
    c.eyeHeight = h;
  } else if (key == "debug_mode") {
    if (!parseInt(value, i)) goto bad;
    c.debugMode = i;
  } else if (key == "draw_radii") {
    // off | both | inner | outer (numeric 0..3 is accepted as well).
    const std::string v = toLower(trim(value));
    if (v == "off" || v == "none" || v == "no" || v == "false" || v == "0") {
      c.drawRadii = 0;
    } else if (v == "both" || v == "all" || v == "on" || v == "yes" || v == "true" || v == "1") {
      c.drawRadii = 1;
    } else if (v == "inner" || v == "2") {
      c.drawRadii = 2;
    } else if (v == "outer" || v == "3") {
      c.drawRadii = 3;
    } else {
      goto bad;
    }
  } else if (key == "ring_width") {
    if (!parseUint(value, u)) goto bad;
    c.ringWidth = u;
  } else if (key == "stale_behavior") {
    c.staleBehavior = toLower(trim(value));
  } else if (key == "log_level") {
    c.logLevel = toLower(trim(value));
  } else {
    if (knownKey(key)) return;
    LOG_WARN("%s: unknown config key '%s'", where.c_str(), key.c_str());
    return;
  }
  return;

bad_rate:
  LOG_WARN("%s: '%s' is not a valid shading rate (use 1x1 1x2 2x1 2x2 2x4 4x2 4x4); ignoring '%s'",
           where.c_str(), value.c_str(), key.c_str());
  return;
bad:
  LOG_WARN("%s: invalid value '%s' for '%s'", where.c_str(), value.c_str(), key.c_str());
}

void validateConfig(Config& c) {
  c.innerRadius = clampf(c.innerRadius, 0.f, 3.f);
  c.outerRadius = clampf(c.outerRadius, 0.f, 3.f);
  if (c.outerRadius < c.innerRadius) c.outerRadius = c.innerRadius;
  c.fixedInnerRadius = clampf(c.fixedInnerRadius, 0.f, 3.f);
  c.fixedOuterRadius = clampf(c.fixedOuterRadius, 0.f, 3.f);
  if (c.fixedOuterRadius < c.fixedInnerRadius) c.fixedOuterRadius = c.fixedInnerRadius;
  c.horizontalScale = clampf(c.horizontalScale, 0.05f, 20.f);
  c.fixedOffsetX = clampf(c.fixedOffsetX, -10.f, 10.f);
  c.fixedOffsetY = clampf(c.fixedOffsetY, -10.f, 10.f);
  c.smoothing = clampf(c.smoothing, 0.f, 0.99f);
  c.sizeTolerance = clampf(c.sizeTolerance, 0.f, 1.f);
  c.minSizeRatio = clampf(c.minSizeRatio, 0.05f, 2.f);
  if (c.debugMode < 0) c.debugMode = 0;
  if (c.debugMode > 2) c.debugMode = 2;
  c.drawRadii = c.drawRadii > 3 ? 3 : c.drawRadii;
  if (c.ringWidth < 1) c.ringWidth = 1;
  if (c.ringWidth > 8) c.ringWidth = 8;
  if (c.eyeWidth > 16384) c.eyeWidth = 16384;
  if (c.eyeHeight > 16384) c.eyeHeight = 16384;
  if (c.eyeWidth < 16 && c.eyeWidth != 0) c.eyeWidth = 16;
  if (c.eyeHeight < 16 && c.eyeHeight != 0) c.eyeHeight = 16;
  if (c.standalone && c.swapchainOnly) {
    LOG_WARN("config: standalone mode matches targets by size, so swapchain_only is ignored");
    c.swapchainOnly = false;
  }
  if (c.texelSize != 0) {
    if (!isPowerOfTwo(c.texelSize) || c.texelSize < 1 || c.texelSize > 64) {
      LOG_WARN("config: texel_size %u is not a power of two in [1,64]; using auto", c.texelSize);
      c.texelSize = 0;
    }
  }
  if (c.staleBehavior != "fixed" && c.staleBehavior != "neutral") {
    LOG_WARN("config: stale_behavior '%s' is not 'fixed' or 'neutral'; using 'fixed'",
             c.staleBehavior.c_str());
    c.staleBehavior = "fixed";
  }
  static const char* kLevels[] = {"error", "warn", "info", "debug", "trace"};
  bool ok = false;
  for (const char* l : kLevels) {
    if (c.logLevel == l) ok = true;
  }
  if (!ok) {
    LOG_WARN("config: unknown log_level '%s'; using 'info'", c.logLevel.c_str());
    c.logLevel = "info";
  }
}

Config loadConfigFromEnv(Config base, uint64_t generation) {  for (const char* key : kKeys) {
    std::string env = std::string("FRAMEFOV_") + toUpper(key);
    const char* v = getenv(env.c_str());
    if (v && *v) applyConfigKey(base, key, v, "env:" + env);
  }
  base.generation = generation;
  validateConfig(base);
  return base;
}

static Config loadConfigInternal(const std::string& appName,
                                 const std::vector<std::string>& exeNames, uint64_t generation,
                                 const std::string& path);

Config loadConfig(const std::string& appName, const std::string& exeName, uint64_t generation,
                  const std::string& path) {  std::vector<std::string> exeNames;
  if (!exeName.empty()) exeNames.push_back(exeName);
  return loadConfigInternal(appName, exeNames, generation, path);
}

Config loadConfigForProcess(const std::string& appName, uint64_t generation, const std::string& path) {
  return loadConfigInternal(appName, processExeNames(), generation, path);
}

namespace {

// Named presets mirror Valve's FDM profiles. Valve has one more band than this
// layer's three zones (inner/middle/outer), so the two coarse bands are folded
// into the outer rate. Radii are Valve's band heights; horizontalScale keeps the
// matching width (e.g. lo .295/.286).
Config presetBase(const std::string& name) {
  Config p;
  p.preset = name;
  if (name == "med") {
    p.innerRadius = p.fixedInnerRadius = 0.245f;
    p.outerRadius = p.fixedOuterRadius = 0.367f;
    p.innerRate = Rate{1, 1};
    p.middleRate = Rate{2, 2};
    p.outerRate = Rate{4, 4};
    p.horizontalScale = 1.03f;
  } else if (name == "hi") {
    p.innerRadius = p.fixedInnerRadius = 0.211f;
    p.outerRadius = p.fixedOuterRadius = 0.351f;
    p.innerRate = Rate{1, 1};
    p.middleRate = Rate{2, 2};
    p.outerRate = Rate{4, 4};
    p.horizontalScale = 1.12f;
  }
  return p;  // "lo" (and anything else) = the built-in defaults
}

}  // namespace

// Applies the INI file, then environment overrides, onto `c`.
static void applyConfigSources(Config& c, const std::string& appName,
                        const std::vector<std::string>& exeNames, uint64_t generation,
                        const std::string& path) {
  std::ifstream file(path);
  if (file) {
    std::stringstream ss;
    ss << file.rdbuf();
    std::vector<IniSection> sections = parseIni(ss.str());
    const std::string app = toLower(appName);
    std::vector<std::string> exes;
    for (const std::string& e : exeNames) exes.push_back(toLower(e));
    for (size_t pass = 0; pass < 3; ++pass) {
      for (const auto& sec : sections) {
        std::string name = toLower(sec.name);
        bool applies = false;
        std::string origin = path + " [" + sec.name + "]";
        if (pass == 0) {
          applies = name == "global";
        } else if (pass == 1) {
          applies = name.rfind("exe:", 0) == 0 && !exes.empty() &&
                    std::find(exes.begin(), exes.end(), name.substr(4)) != exes.end();
        } else {
          applies = name.rfind("app:", 0) == 0 && !app.empty() && name.substr(4) == app;
        }
        if (!applies) continue;
        for (const auto& kv : sec.entries) applyConfigKey(c, kv.first, kv.second, origin);
      }
    }
  }
  c = loadConfigFromEnv(c, generation);
  log::setLevelFromString(c.logLevel.c_str());
}

Config loadConfigInternal(const std::string& appName, const std::vector<std::string>& exeNames,
                          uint64_t generation, const std::string& path) {
  Config c;
  applyConfigSources(c, appName, exeNames, generation, path);
  if (c.preset != "lo") {
    // Re-read everything on top of the preset's base, so explicit radii/rates
    // (file or env) still win, exactly like Valve's FOVE_LEVEL vs FDM_DEBUG.
    Config base = presetBase(c.preset);
    applyConfigSources(base, appName, exeNames, generation, path);
    c = base;
  }
  return c;
}

std::string configSummary(const Config& c) {
  char buf[640];
  snprintf(buf, sizeof(buf),
           "enabled=%d eye_tracking=%d standalone=%d eye_size=%s radii eye %.2f/%.2f fixed "
           "%.2f/%.2f rates %s/%s/%s hscale %.2f offset (%.2f,%.2f) smoothing %.2f texel %u "
           "debug %d rings %s x%u stale %s gaze_yflip=%d preset %s",
           (int)c.enabled, (int)c.eyeTracking, (int)c.standalone,
           c.eyeWidth ? (std::to_string(c.eyeWidth) + "x" + std::to_string(c.eyeHeight)).c_str()
                      : "auto",
           c.innerRadius, c.outerRadius, c.fixedInnerRadius, c.fixedOuterRadius,
           rateToString(c.innerRate).c_str(), rateToString(c.middleRate).c_str(),
           rateToString(c.outerRate).c_str(), c.horizontalScale, c.fixedOffsetX, c.fixedOffsetY,
           c.smoothing, c.texelSize, c.debugMode, drawRadiiName(c.drawRadii), c.ringWidth,
           c.staleBehavior.c_str(), (int)c.gazeYFlip, c.preset.c_str());
  return buf;
}

}  // namespace framefov
