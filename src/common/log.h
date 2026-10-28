// Leveled logging for both halves of the layer.
//
// Every macro expands to a level check followed by a single printf-style call
// into log::write(). The tag ([xr] / [vk]) is a compile-time constant so that
// the two halves never disagree; CMake sets FRAMEFOV_LOG_TAG per source file.
#pragma once

#include <cstdarg>

#ifndef FRAMEFOV_LOG_TAG
#define FRAMEFOV_LOG_TAG "framefov"
#endif

namespace framefov {
namespace log {

enum Level {
  LEVEL_ERROR = 0,
  LEVEL_WARN = 1,
  LEVEL_INFO = 2,
  LEVEL_DEBUG = 3,
  LEVEL_TRACE = 4,
};

// Opens $XDG_STATE_HOME/framefov/framefov.log (truncating it) and installs
// the default level. Safe to call more than once.
void init();
void shutdown();
void flush();

void setLevel(Level l);
void setLevelFromString(const char* s);
Level level();
inline bool enabled(Level l) { return l <= level(); }

// Not for direct use; always go through the LOG_* macros.
void write(Level l, const char* tag, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

// Returns true the first time a given key is seen (process lifetime).
bool once(const char* key);

// Absolute path of the log file, or "" when the file could not be opened.
const char* path();

}  // namespace log
}  // namespace framefov

#define FRAMEFOV_LOG_AT(lvl, ...)                          \
  do {                                                  \
    if (::framefov::log::enabled(lvl)) {                   \
      ::framefov::log::write(lvl, FRAMEFOV_LOG_TAG, __FILE__, __LINE__, __VA_ARGS__); \
    }                                                   \
  } while (0)

#define LOG_ERROR(...) FRAMEFOV_LOG_AT(::framefov::log::LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(...) FRAMEFOV_LOG_AT(::framefov::log::LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO(...) FRAMEFOV_LOG_AT(::framefov::log::LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) FRAMEFOV_LOG_AT(::framefov::log::LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) FRAMEFOV_LOG_AT(::framefov::log::LEVEL_TRACE, __VA_ARGS__)

// LOG_ONCE(INFO, "render target 1024x1024", "...%ux%u...", w, h);
#define LOG_ONCE(lvl, key, ...)                                       \
  do {                                                                \
    if (::framefov::log::enabled(::framefov::log::LEVEL_##lvl) &&           \
        ::framefov::log::once(key)) {                                    \
      ::framefov::log::write(::framefov::log::LEVEL_##lvl, FRAMEFOV_LOG_TAG, __FILE__, __LINE__, __VA_ARGS__); \
    }                                                                 \
  } while (0)
