#pragma once

// Small double-precision vector math for the simulation layer (gameplay
// motion is cosmetic-class for now; identity-critical placement decisions
// go through det:: when they become gameplay-visible state).

#include <cmath>

namespace inf::sim {

struct Vec3 {
  double x{0.0}, y{0.0}, z{0.0};

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  const double len = length(v);
  return len > 0.0 ? v * (1.0 / len) : Vec3{0.0, 0.0, 1.0};
}

// Rodrigues rotation of v about unit axis by angle (radians).
inline Vec3 rotate(const Vec3& v, const Vec3& axis, double angle) {
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

}  // namespace inf::sim
