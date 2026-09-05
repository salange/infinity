#pragma once
// Free-fly camera: mouse look, WASD + QE (down/up), shift = fast,
// ctrl = slow, scroll = base speed.
#include "math.hpp"

namespace cb {

struct Camera {
  Vec3 position{0, 40, 160};
  float yaw{0.0f};    // radians, 0 = looking down -Z
  float pitch{0.0f};  // radians, positive = up
  float fov_y{radians(55.0f)};
  float speed{18.0f};  // m/s base

  Vec3 forward() const {
    return normalize(Vec3{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)});
  }
  Vec3 right() const { return normalize(cross(forward(), Vec3{0, 1, 0})); }
  Mat4 view() const { return look_at(position, position + forward(), Vec3{0, 1, 0}); }
  void look(float dx_pixels, float dy_pixels) {
    const float s = 0.0025f;
    yaw -= dx_pixels * s;
    pitch = clampf(pitch - dy_pixels * s, -1.55f, 1.55f);
  }
  void move(float fwd, float strafe, float up, float dt, float mult) {
    const Vec3 f = forward();
    const Vec3 r = right();
    position += (f * fwd + r * strafe + Vec3{0, 1, 0} * up) * (speed * mult * dt);
  }
  void look_at_point(Vec3 target) {
    const Vec3 d = normalize(target - position);
    pitch = std::asin(clampf(d.y, -1.0f, 1.0f));
    yaw = std::atan2(-d.x, -d.z);
  }
};

}  // namespace cb
