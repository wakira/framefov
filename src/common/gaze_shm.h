// Shared-memory channel between the out-of-process gaze publisher and the layer.
//
// Why out of process: VR_InitInternal() inside the game's process blocks (and
// competes with Proton's own OpenVR/OpenXR client), which stalled the render
// thread and left the headset black while audio played. The publisher is a
// separate process; the layer only ever reads memory.
//
// Single writer (publisher), many readers (one layer instance per process).
// Seqlock: seq is odd while the record is being written.
#pragma once

#include <cstdint>
#include <cstring>

namespace framefov {

struct GazeShm {
  uint32_t magic;
  uint32_t version;
  uint64_t seq;       // odd = write in progress
  int64_t monoNs;     // CLOCK_MONOTONIC of the sample
  float u0, v0;       // left eye, UV in the eye image
  float u1, v1;       // right eye
  uint32_t valid;     // 1 = this sample is usable
  uint32_t everValid; // 1 once any sample has been written
  uint32_t clientType;
};

constexpr uint32_t kGazeMagic = 0x5a475646;  // 'FVGZ'
constexpr uint32_t kGazeVersion = 1;
constexpr const char *kGazeShmName = "/framefov-gaze";
// Written by the layer whenever a game is rendering with gaze enabled. The
// publisher connects to SteamVR only while this is fresh, so it never becomes a
// VR client at boot (gamescope's OpenVR backend asserts if it becomes session
// manager after another VR app has already connected).
constexpr const char *kGazeWantPath = "/dev/shm/framefov-gaze.want";
constexpr int64_t kGazeStaleNs = 250LL * 1000 * 1000;

inline void gazeShmPublish(GazeShm *s, const GazeShm &v) {
  s->seq += 1;  // odd: writing
  __atomic_thread_fence(__ATOMIC_RELEASE);
  s->monoNs = v.monoNs;
  s->u0 = v.u0;
  s->v0 = v.v0;
  s->u1 = v.u1;
  s->v1 = v.v1;
  s->valid = v.valid;
  s->everValid = v.everValid;
  s->clientType = v.clientType;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  s->seq += 1;  // even: stable
}

inline bool gazeShmRead(const GazeShm *s, GazeShm &out) {
  if (!s) return false;
  const uint64_t before = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
  if (before == 0 || (before & 1u)) return false;
  std::memcpy(&out, s, sizeof(out));
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const uint64_t after = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
  return before == after && out.magic == kGazeMagic && out.version == kGazeVersion;
}

}  // namespace framefov
