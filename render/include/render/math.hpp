#pragma once

// Minimal render-side matrix/vector math (floats/doubles are fine here —
// rendering is cosmetic; nothing flows back into generation). Column-major
// mat4, matching WGSL's mat4x4<f32> memory layout.

#include <cmath>

namespace inf::render {

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

struct Mat4 {
  float m[16]{};  // column-major

  static Mat4 identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
};

inline Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[k * 4 + row] * b.m[col * 4 + k];
      }
      r.m[col * 4 + row] = sum;
    }
  }
  return r;
}

// Right-handed look-at; eye at origin (camera-relative rendering — the
// caller subtracts the camera position in doubles before this).
inline Mat4 look_dir(const Vec3& forward, const Vec3& up) {
  const Vec3 f = normalize(forward);
  const Vec3 s = normalize(cross(f, up));
  const Vec3 u = cross(s, f);
  Mat4 r = Mat4::identity();
  r.m[0] = static_cast<float>(s.x);
  r.m[4] = static_cast<float>(s.y);
  r.m[8] = static_cast<float>(s.z);
  r.m[1] = static_cast<float>(u.x);
  r.m[5] = static_cast<float>(u.y);
  r.m[9] = static_cast<float>(u.z);
  r.m[2] = static_cast<float>(-f.x);
  r.m[6] = static_cast<float>(-f.y);
  r.m[10] = static_cast<float>(-f.z);
  return r;
}

// Perspective with WebGPU clip space (z in [0, 1]).
inline Mat4 perspective(double fov_y_radians, double aspect, double near_z, double far_z) {
  const double f = 1.0 / std::tan(fov_y_radians * 0.5);
  Mat4 r;
  r.m[0] = static_cast<float>(f / aspect);
  r.m[5] = static_cast<float>(f);
  r.m[10] = static_cast<float>(far_z / (near_z - far_z));
  r.m[11] = -1.0f;
  r.m[14] = static_cast<float>(near_z * far_z / (near_z - far_z));
  return r;
}

inline Mat4 translate(const Vec3& t) {
  Mat4 r = Mat4::identity();
  r.m[12] = static_cast<float>(t.x);
  r.m[13] = static_cast<float>(t.y);
  r.m[14] = static_cast<float>(t.z);
  return r;
}

// Model matrix from a (scaled) basis and translation: columns are the
// images of the x/y/z unit axes.
inline Mat4 from_basis(const Vec3& x_axis, const Vec3& y_axis, const Vec3& z_axis,
                       const Vec3& t) {
  Mat4 r = Mat4::identity();
  r.m[0] = static_cast<float>(x_axis.x);
  r.m[1] = static_cast<float>(x_axis.y);
  r.m[2] = static_cast<float>(x_axis.z);
  r.m[4] = static_cast<float>(y_axis.x);
  r.m[5] = static_cast<float>(y_axis.y);
  r.m[6] = static_cast<float>(y_axis.z);
  r.m[8] = static_cast<float>(z_axis.x);
  r.m[9] = static_cast<float>(z_axis.y);
  r.m[10] = static_cast<float>(z_axis.z);
  r.m[12] = static_cast<float>(t.x);
  r.m[13] = static_cast<float>(t.y);
  r.m[14] = static_cast<float>(t.z);
  return r;
}

}  // namespace inf::render
