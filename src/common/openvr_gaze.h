// Optional SteamVR-native gaze source (FRAMEFOV_OPENVR_GAZE=1).
//
// Some desktop runtimes are inert on the OpenXR eye-gaze path even though the
// driver does have eye tracking: SteamVR with the Frame's vrlink driver reports
// supportsEyeGazeInteraction, accepts and attaches our gaze action set, and then
// never activates the action. The same runtime does expose eye tracking through
// OpenVR's own API (IVRSystem::GetEyeTrackedFoveationCenter), which we can reach
// from inside the app process - where this layer lives - by loading the
// libopenvr_api.so that sits next to the vrclient.so the OpenXR loader has
// already mapped in.
//
// This is opt-in and off by default: it creates a second, background SteamVR
// client inside the game's process.
#pragma once

#include <string>

namespace framefov {

// Loads libopenvr_api.so, initialises a background client and probes the runtime
// once, filling `report` with everything that was found. Returns false when the
// library or the interface is unavailable.
bool openvrGazePrepare(std::string& report);

// Per-eye eye-tracked foveation centre as eye-image UV. False when the runtime
// has no centre for this client (eye tracking off, or not the VR app).
bool openvrGazeRead(float& u0, float& v0, float& u1, float& v1);

void openvrGazeShutdown();

}  // namespace framefov
