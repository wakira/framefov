// Exception guards for C entry points. No exception may escape a hook.
#pragma once

#include <exception>

#include "common/log.h"

#define FRAMEFOV_GUARD_BEGIN try {

#define FRAMEFOV_GUARD_END(errval)                                                     \
  }                                                                                 \
  catch (const std::exception& framefov_e) {                                           \
    LOG_ERROR("unhandled exception in %s: %s", __func__, framefov_e.what());           \
    return (errval);                                                                \
  }                                                                                 \
  catch (...) {                                                                     \
    LOG_ERROR("unhandled exception in %s", __func__);                               \
    return (errval);                                                                \
  }

// For void entry points: log once and let the call continue as a passthrough.
#define FRAMEFOV_GUARD_END_VOID                                                        \
  }                                                                                 \
  catch (const std::exception& framefov_e) {                                           \
    LOG_ERROR("unhandled exception in %s: %s", __func__, framefov_e.what());           \
  }                                                                                 \
  catch (...) {                                                                     \
    LOG_ERROR("unhandled exception in %s", __func__);                               \
  }
