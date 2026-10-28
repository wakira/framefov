// Process-wide state published by the layer's control path and read by the
// Vulkan half. On Steam Frame the control path is the eye-tracking/standalone
// setup inside the same shared object, so a function-local static singleton is
// enough; log its address at init to verify.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "common/config.h"

namespace framefov {

struct EyeView {  // per eye, per frame
  bool valid = false;
  float u = 0.5f, v = 0.5f;  // gaze in [0,1] of the eye image (v down, Vulkan)
  bool eyeTracked = false;   // false => fixed foveation radii are used
};

// Immutable once published.
struct Snapshot {
  uint64_t frameIndex = 0;
  bool active = false;                // a usable gaze/geometry source exists
  uint32_t eyeW = 0, eyeH = 0;        // expected per-eye render size (0 = any)
  EyeView eyes[2];
  std::shared_ptr<const Config> config;
  int64_t publishTimeNs = 0;  // CLOCK_MONOTONIC
  // True when the eye data is synthesized (fixed foveation) rather than
  // published by eye tracking: it never goes stale.
  bool synthetic = false;
  // True for synthesized snapshots: they describe the whole process, so every
  // VkDevice in it counts (a game and an in-process vrclient have separate devices).
  bool anyDevice = false;
};

// Foveation statistics, incremented by the Vulkan half.
struct Stats {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> passes{0};
  std::atomic<uint64_t> foveatedPasses{0};
  std::atomic<uint64_t> neutralPasses{0};
  std::atomic<int> eyeTracked{0};
  std::atomic<float> gazeU{0.5f};
  std::atomic<float> gazeV{0.5f};
};

class SharedState {
 public:
  static SharedState& get();
  std::shared_ptr<const Snapshot> snapshot() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
  }
  void publish(std::shared_ptr<const Snapshot> s) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    std::atomic_store_explicit(&snap_, std::move(s), std::memory_order_release);
  }
  // Copy the current snapshot (or a default one), mutate the copy, publish.
  template <class F>
  void update(F&& mutate) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    auto cur = std::atomic_load_explicit(&snap_, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot>(cur ? *cur : Snapshot{});
    mutate(*next);
    std::atomic_store_explicit(&snap_, std::shared_ptr<const Snapshot>(next), std::memory_order_release);
  }

  Stats stats;

 private:
  SharedState();
  mutable std::mutex writerMutex_;
  std::shared_ptr<const Snapshot> snap_;
};

int64_t nowNs();  // CLOCK_MONOTONIC

// The config published by the control path, or a default-constructed one when no
// snapshot has been published yet.
const Config& effectiveConfig(const std::shared_ptr<const Snapshot>& snap);

}  // namespace framefov
