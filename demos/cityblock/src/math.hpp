#pragma once
// Small float math for the demo: y-up right-handed world, column-major
// mat4 matching WGSL. Rendering only — nothing here feeds generation.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cb {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
  float x{0}, y{0};
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
};
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline Vec2 normalize(Vec2 v) {
  const float l = length(v);
  return l > 1e-12f ? v * (1.0f / l) : Vec2{1, 0};
}
inline Vec2 perp(Vec2 v) { return {-v.y, v.x}; }  // left normal

struct Vec3 {
  float x{0}, y{0}, z{0};
  Vec3() = default;
  Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
  Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) {
  const float l = length(v);
  return l > 1e-12f ? v * (1.0f / l) : Vec3{0, 1, 0};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline Vec3 vmin(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Vec4 {
  float x{0}, y{0}, z{0}, w{0};
  Vec4() = default;
  Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  Vec4(Vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  Vec3 xyz() const { return {x, y, z}; }
};

struct Mat4 {
  float m[16]{};  // column-major: m[col*4 + row]
  static Mat4 identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }
};

inline Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return r;
}
inline Vec4 mul(const Mat4& a, Vec4 v) {
  Vec4 r;
  r.x = a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w;
  r.y = a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w;
  r.z = a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w;
  r.w = a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w;
  return r;
}
inline Mat4 translate(Vec3 t) {
  Mat4 r = Mat4::identity();
  r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
  return r;
}
// Right-handed view matrix (camera looks down -Z in view space).
inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) {
  const Vec3 f = normalize(target - eye);
  const Vec3 s = normalize(cross(f, up));
  const Vec3 u = cross(s, f);
  Mat4 r = Mat4::identity();
  r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
  r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
  r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
  r.at(0, 3) = -dot(s, eye);
  r.at(1, 3) = -dot(u, eye);
  r.at(2, 3) = dot(f, eye);
  return r;
}
// WebGPU clip space: z in [0, 1], y up.
inline Mat4 perspective(float fov_y, float aspect, float zn, float zf) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  Mat4 r;
  r.at(0, 0) = f / aspect;
  r.at(1, 1) = f;
  r.at(2, 2) = zf / (zn - zf);
  r.at(2, 3) = zn * zf / (zn - zf);
  r.at(3, 2) = -1.0f;
  return r;
}
inline Mat4 ortho(float l, float r_, float b, float t, float zn, float zf) {
  Mat4 r = Mat4::identity();
  r.at(0, 0) = 2.0f / (r_ - l);
  r.at(1, 1) = 2.0f / (t - b);
  r.at(2, 2) = 1.0f / (zn - zf);
  r.at(0, 3) = -(r_ + l) / (r_ - l);
  r.at(1, 3) = -(t + b) / (t - b);
  r.at(2, 3) = zn / (zn - zf);
  return r;
}
// General 4x4 inverse (Cramer); fine for per-frame use.
inline Mat4 inverse(const Mat4& a) {
  const float* m = a.m;
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  Mat4 r;
  if (std::fabs(det) < 1e-30f) return Mat4::identity();
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
  return r;
}

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float smoothstep(float e0, float e1, float x) {
  const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline float radians(float deg) { return deg * kPi / 180.0f; }

// Orthonormal basis from a normal (Frisvad / Duff et al.).
inline void basis(Vec3 n, Vec3& t, Vec3& b) {
  const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (sign + n.z);
  const float bb = n.x * n.y * a;
  t = Vec3{1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x};
  b = Vec3{bb, sign + n.y * n.y * a, -n.y};
}

}  // namespace cb
