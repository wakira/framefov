#include "common/shared_state.h"

#include <ctime>

namespace framefov {

SharedState::SharedState() {
  auto s = std::make_shared<Snapshot>();
  s->config = std::make_shared<const Config>();
  s->publishTimeNs = nowNs();
  snap_ = s;
}

SharedState& SharedState::get() {
  static SharedState instance;
  return instance;
}

int64_t nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

const Config& effectiveConfig(const std::shared_ptr<const Snapshot>& snap) {
  static const Config kDefault{};
  if (snap && snap->config) return *snap->config;
  return kDefault;
}

}  // namespace framefov
