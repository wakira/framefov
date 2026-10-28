// Minimal vec3/quat helpers. Header only, no dependencies, used by the gaze
// projection and by the unit tests.
#pragma once

#include <cmath>

namespace framefov {

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a) {
  float l = length(a);
  return l > 0.f ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{0.f, 0.f, 0.f};
}

struct Quat {
  float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

inline Quat quatMul(const Quat& a, const Quat& b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

inline Quat quatConjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

inline Vec3 quatRotate(const Quat& q, const Vec3& v) {
  // v' = q * v * q^-1, expanded to avoid building an extra quaternion.
  const Vec3 qv{q.x, q.y, q.z};
  const Vec3 t = cross(qv, v) * 2.f;
  return v + t * q.w + cross(qv, t);
}

inline Quat quatFromAxisAngle(const Vec3& axis, float radians) {
  Vec3 n = normalize(axis);
  float half = radians * 0.5f;
  float s = std::sin(half);
  return {n.x * s, n.y * s, n.z * s, std::cos(half)};
}

// Yaw (about +Y) then pitch (about +X), both in radians: the usual way to build
// a test gaze direction.
inline Quat quatFromYawPitch(float yaw, float pitch) {
  return quatMul(quatFromAxisAngle({0.f, 1.f, 0.f}, yaw),
                 quatFromAxisAngle({1.f, 0.f, 0.f}, pitch));
}

}  // namespace framefov
