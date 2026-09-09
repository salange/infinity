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
  float speed{72.0f};   // m/s cruise (4x the first version); Shift 5x, Ctrl 0.2x
  Vec3 velocity;        // smoothed: accelerates toward the wanted velocity

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
  // Accelerating flight: the wanted velocity is reached in ~0.25 s, and
  // releasing the keys brakes in the same time. Yaw is unbounded (spin as
  // long as you like); only pitch is clamped short of the poles.
  void update(float fwd, float strafe, float up, float dt, float mult) {
    const Vec3 f = forward();
    const Vec3 r = right();
    Vec3 wanted = f * fwd + r * strafe + Vec3{0, 1, 0} * up;
    if (length(wanted) > 1e-4f) wanted = normalize(wanted) * (speed * mult);
    const float k = 1.0f - std::exp(-dt / 0.25f);
    velocity = velocity + (wanted - velocity) * k;
    position += velocity * dt;
  }
  void look_at_point(Vec3 target) {
    const Vec3 d = normalize(target - position);
    pitch = std::asin(clampf(d.y, -1.0f, 1.0f));
    yaw = std::atan2(-d.x, -d.z);
  }
};

}  // namespace cb
