#pragma once

// The library is built with -fvisibility=hidden; only the two loader
// negotiation entry points are exported (exports.map lists them too).
#if defined(_WIN32)
#define FRAMEFOV_EXPORT __declspec(dllexport)
#else
#define FRAMEFOV_EXPORT __attribute__((visibility("default")))
#endif
