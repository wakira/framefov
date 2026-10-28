#include "common/openvr_gaze.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <openvr/openvr.h>

#include "common/gaze_math.h"
#include "common/log.h"

namespace framefov {
namespace {

using InitInternalFn = void (*)(vr::EVRInitError*, vr::EVRApplicationType);
using ShutdownInternalFn = void (*)();
using GetGenericInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
using ErrorDescriptionFn = const char* (*)(vr::EVRInitError);

void* g_library = nullptr;
InitInternalFn g_init = nullptr;
ShutdownInternalFn g_shutdown = nullptr;
GetGenericInterfaceFn g_getInterface = nullptr;
ErrorDescriptionFn g_errorDescription = nullptr;  // optional, for readable logs
const char* g_mode = "2";
vr::IVRSystem* g_system = nullptr;
bool g_prepared = false;
bool g_available = false;
std::string g_report;

struct ShutdownAtExit {
  ~ShutdownAtExit() { openvrGazeShutdown(); }
} g_shutdownGuard;

// Directory of the SteamVR install, taken from the vrclient.so already mapped into
// this process (the OpenXR loader loaded it because SteamVR is the active
// runtime). libopenvr_api.so belongs to the same install.
std::string steamVrDirFromMaps() {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return {};
  char line[4096];
  std::string found;
  const char* needle = "vrclient.so";
  const size_t needleLen = strlen(needle);
  while (fgets(line, sizeof(line), f)) {
    char* path = strchr(line, '/');
    if (!path) continue;
    std::string p(path);
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) p.pop_back();
    const size_t cut = p.find(" (deleted)");
    if (cut != std::string::npos) p.resize(cut);
    if (p.size() >= needleLen && p.compare(p.size() - needleLen, needleLen, needle) == 0) {
      found = p;
      break;
    }
  }
  fclose(f);
  if (found.empty()) return {};
  const size_t slash = found.find_last_of('/');
  return slash == std::string::npos ? std::string() : found.substr(0, slash);
}

std::string initErrorText(vr::EVRInitError e) {
  const char* s = g_errorDescription ? g_errorDescription(e) : nullptr;
  return std::string(s ? s : "unknown error") + " (" + std::to_string((int)e) + ")";
}

}  // namespace

bool openvrGazePrepare(std::string& report) {
  if (g_prepared) {
    report = g_report;
    return g_available;
  }
  g_prepared = true;

  std::string dir = steamVrDirFromMaps();
  if (dir.empty()) {
    // Steam Frame: the game runs under Proton/FEX, so vrclient.so may not be
    // mapped in this process even though SteamVR is running. libopenvr_api.so is
    // a stable part of the runtime install; fall back to it.
    dir = "/opt/steamvr/bin/linuxarm64";
  }
  const std::string libPath = dir + "/libopenvr_api.so";
  g_library = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!g_library) {
    g_report = "cannot load " + libPath + ": " + (dlerror() ? dlerror() : "?");
    report = g_report;
    return false;
  }
  g_init = reinterpret_cast<InitInternalFn>(dlsym(g_library, "VR_InitInternal"));
  g_shutdown = reinterpret_cast<ShutdownInternalFn>(dlsym(g_library, "VR_ShutdownInternal"));
  g_getInterface =
      reinterpret_cast<GetGenericInterfaceFn>(dlsym(g_library, "VR_GetGenericInterface"));
  g_errorDescription = reinterpret_cast<ErrorDescriptionFn>(
      dlsym(g_library, "VR_GetVRInitErrorAsEnglishDescription"));
  if (!g_init || !g_shutdown || !g_getInterface) {
    g_report = "libopenvr_api.so has no VR_InitInternal/VR_GetGenericInterface";
    report = g_report;
    return false;
  }

  // Client type. A background client (the polite default: this process already
  // belongs to the VR application) is *refused* gaze data by SteamVR, so the
  // "Other" type - which does not claim to render a scene, so it does not disturb
  // the running application - is what actually gets the eye-tracked centre.
  // 1/background, 2/other (default), 3/overlay, 5/utility. VRApplication_OpenXRScene
  // is deliberately NOT offered: it is reserved for OpenXR itself, and asking for it
  // here (the frame after this was written) was followed by the headset dropping off
  // the link - one reserved client type too far.
  // Client type. FRAMEFOV_OPENVR_GAZE is the on/off switch; the type has its own
  // variable, because "1" used to double as VRApplication_Background and SteamVR
  // refuses eye-tracked data to background clients.
  const char* mode = getenv("FRAMEFOV_OPENVR_CLIENT");
  g_mode = mode && *mode ? mode : "2";
  vr::EVRApplicationType appType = vr::VRApplication_Other;
  if (!strcmp(g_mode, "1") || !strcmp(g_mode, "background")) {
    appType = vr::VRApplication_Background;
  } else if (!strcmp(g_mode, "3") || !strcmp(g_mode, "overlay")) {
    appType = vr::VRApplication_Overlay;
  } else if (!strcmp(g_mode, "5") || !strcmp(g_mode, "utility")) {
    appType = vr::VRApplication_Utility;
  }
  vr::EVRInitError err = vr::VRInitError_None;
  g_init(&err, appType);
  if (err != vr::VRInitError_None) {
    g_report = "VR_InitInternal(Background) failed: " + initErrorText(err);
    report = g_report;
    return false;
  }

  vr::EVRInitError ifaceErr = vr::VRInitError_None;
  void* iface = g_getInterface(vr::IVRSystem_Version, &ifaceErr);
  if (!iface) {
    g_report = std::string("the runtime does not provide ") + vr::IVRSystem_Version + " (" +
               initErrorText(ifaceErr) + ")";
    report = g_report;
    return false;
  }
  g_system = static_cast<vr::IVRSystem*>(iface);

  char model[256] = {0};
  vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
  g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                           vr::Prop_ModelNumber_String, model, sizeof(model), &pe);
  pe = vr::TrackedProp_Success;
  const bool supports = g_system->GetBoolTrackedDeviceProperty(
      vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SupportsXrEyeGazeInteraction_Bool, &pe);

  vr::HmdVector2_t l{}, r{};
  const bool haveCentre = g_system->GetEyeTrackedFoveationCenter(&l, &r);

  g_report = std::string("client type ") + g_mode + "; runtime " + (g_system->GetRuntimeVersion() ? g_system->GetRuntimeVersion() : "?") +
             ", HMD '" + (model[0] ? model : "?") + "', " + vr::IVRSystem_Version +
             ", Prop_SupportsXrEyeGazeInteraction=" + (supports ? "true" : "false") +
             (pe == vr::TrackedProp_Success ? "" : " (property unsupported)") +
             (haveCentre ? ", GetEyeTrackedFoveationCenter=ok" : ", GetEyeTrackedFoveationCenter=no data");
  g_available = true;
  report = g_report;
  return true;
}

bool openvrGazeRead(float& u0, float& v0, float& u1, float& v1) {
  if (!g_available || !g_system) return false;
  vr::HmdVector2_t l{}, r{};
  if (!g_system->GetEyeTrackedFoveationCenter(&l, &r)) return false;
  openvrNdcToUv(l.v[0], l.v[1], u0, v0);
  openvrNdcToUv(r.v[0], r.v[1], u1, v1);
  return true;
}

void openvrGazeShutdown() {
  g_system = nullptr;
  if (g_shutdown) {
    g_shutdown();
    g_shutdown = nullptr;
  }
  g_init = nullptr;
  g_getInterface = nullptr;
  g_errorDescription = nullptr;
  if (g_library) {
    dlclose(g_library);
    g_library = nullptr;
  }
  g_available = false;
  g_prepared = false;  // allow a later reconnect (publisher is on-demand)
}

}  // namespace framefov
