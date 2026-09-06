#pragma once
#include "city/math.hpp"
namespace cb {
using namespace inf::city;

// Reversed Z: near maps to depth 1, far to 0. With a float depth buffer the
// precision then follows the distance (about 1e-7 relative), instead of
// collapsing to centimetres a few hundred metres out as standard Z does.
inline Mat4 perspective_reversed(float fov_y, float aspect, float zn, float zf) {
  const float f = 1.0f / std::tan(fov_y * 0.5f);
  Mat4 r;
  r.at(0, 0) = f / aspect;
  r.at(1, 1) = f;
  r.at(2, 2) = zn / (zf - zn);
  r.at(2, 3) = zn * zf / (zf - zn);
  r.at(3, 2) = -1.0f;
  return r;
}
}  // namespace cb
