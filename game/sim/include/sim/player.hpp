#pragma once

#include <cstdint>
#include <vector>

#include "sim/vec3.hpp"
#include "gen/effective_field.hpp"

namespace inf::sim {

// Player controller (prototype-v0 spec section 9, flight model per
// Sascha 2026-08-30):
//
// Flight: the ship is the camera, full 6-DOF attitude. A reticle (moved
// by the mouse, clamped to a max screen deviation) steers the ship —
// centered reticle = straight flight; deviation maps NON-linearly to
// pitch/yaw rate. W accelerates, S decelerates to zero (never reverse),
// A/D roll. Speed is capped by an altitude governor. The ship cannot
// penetrate the ground: the inward radial component stops at a clearance
// above the surface.
//
// E lands (eased descent to eye height on the local surface) and takes
// off again (eased climb to a hover altitude). On foot: shooter mouse
// (yaw free, pitch clamped to the 180-degree arc), WASD walk (shift =
// run), camera glued to 1.80 m above the ground — falling through is
// impossible by construction.
//
// Fire spawns a beam projectile toward the crosshair in both modes;
// beams die after a fixed travel distance.

enum class FlightZone : std::uint8_t {
  Space = 0,
  Atmosphere = 1,
};

enum class PlayerMode : std::uint8_t {
  Flight = 0,
  Landing = 1,
  OnFoot = 2,
  Takeoff = 3,
};

struct InputFrame {
  double dt = 0.0;
  double mouse_dx = 0.0;  // pixels
  double mouse_dy = 0.0;
  bool forward = false;   // W
  bool back = false;      // S
  bool left = false;      // A (strafe on foot, roll in flight)
  bool right = false;     // D
  bool run = false;       // shift
  bool fire = false;      // held
  bool interact_pressed = false;  // E, edge-triggered
  // Projection info for reticle ray construction.
  double aspect = 16.0 / 9.0;
  double fov_y = 1.1;
};

struct Beam {
  Vec3 position;   // planet-local meters
  Vec3 velocity;   // m/s
  double traveled = 0.0;
};

class Player {
 public:
  static constexpr double kEyeHeight = 1.80;
  static constexpr double kShipClearance = 2.5;
  static constexpr double kBeamSpeed = 500.0;
  static constexpr double kMachSix = 2058.0;     // atmosphere hard cap (Mach 6)
  static constexpr double kTenthC = 29'979'245.8;  // space hard cap
  static constexpr double kBeamMaxDistance = 1500.0;
  static constexpr double kReticleMax = 0.42;  // NDC-vertical units

  Player(const gen::EffectiveField& field, Vec3 spawn_position);

  void update(const InputFrame& input);

  PlayerMode mode() const { return mode_; }
  const Vec3& position() const { return position_; }
  const Vec3& forward() const { return forward_; }
  const Vec3& up() const { return up_; }
  Vec3 right() const { return cross(forward_, up_); }
  double speed() const { return speed_; }
  double reticle_x() const { return reticle_x_; }  // NDC-ish, x scaled by 1/aspect at use
  double reticle_y() const { return reticle_y_; }
  const std::vector<Beam>& beams() const { return beams_; }
  double altitude() const;
  // Space vs Atmosphere (M5): inside the planet's atmosphere band the
  // speed is hard-capped at Mach 2; outside at 0.1c. Airless planets are
  // Space everywhere.
  FlightZone zone() const;

 private:
  void update_flight(const InputFrame& input);
  void update_on_foot(const InputFrame& input);
  void update_transition(const InputFrame& input);
  void update_beams(double dt);
  void try_fire(const InputFrame& input);
  double ground_radius(const Vec3& dir) const;
  void clamp_to_ground_flight();

  const gen::EffectiveField& field_;
  PlayerMode mode_ = PlayerMode::Flight;

  Vec3 position_;
  Vec3 forward_{0.0, 0.0, 1.0};
  Vec3 up_{1.0, 0.0, 0.0};
  double speed_ = 0.0;
  double reticle_x_ = 0.0;
  double reticle_y_ = 0.0;

  // Transition (landing/takeoff) state.
  double transition_t_ = 0.0;
  double transition_duration_ = 1.0;
  Vec3 transition_from_pos_, transition_to_pos_;
  Vec3 transition_from_fwd_, transition_to_fwd_;
  Vec3 transition_from_up_, transition_to_up_;

  std::vector<Beam> beams_;
  double fire_cooldown_ = 0.0;
};

}  // namespace inf::sim
