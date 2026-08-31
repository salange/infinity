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
  Map = 4,  // system map (design/map-mode.md): controls suspended
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

// The closest planet this frame (anchor-local frame), fed by the app.
// Every planet is treated the same: whichever is closest governs the
// speed limit, the flight zone, and whether E may land. When the app
// never feeds one (headless tests), the anchor body is the nearest body.
struct NearestBody {
  Vec3 center;             // planet center, anchor-local meters
  double radius_m = 0.0;
  double atmosphere_m = 0.0;  // 0 = airless
  bool is_anchor = true;      // only the anchor has terrain to land on
};

class Player {
 public:
  static constexpr double kEyeHeight = 1.80;
  static constexpr double kShipClearance = 2.5;
  static constexpr double kBeamSpeed = 500.0;
  static constexpr double kMachSix = 2058.0;     // atmosphere hard cap (Mach 6)
  static constexpr double kLightSpeed = 299'792'458.0;  // space hard cap (c)
  static constexpr double kBeamMaxDistance = 1500.0;
  static constexpr double kReticleMax = 0.42;  // NDC-vertical units
  // Landing window on airless worlds (no atmosphere band to gate E by).
  static constexpr double kAirlessLandingBand = 8'000.0;

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
  // Space vs Atmosphere (M5): inside the NEAREST planet's atmosphere band
  // the speed is hard-capped at Mach 6; outside at light speed. Airless
  // planets are Space everywhere.
  FlightZone zone() const;

  // Per-frame nearest-planet feed (see NearestBody). The nearest planet
  // governs the altitude speed governor, zone(), and the landing gate.
  void set_nearest_body(const NearestBody& body) { nearest_ = body; has_nearest_ = true; }
  // E may land: the nearest planet is the anchor (has terrain) and the
  // ship is inside its atmosphere band (a fixed window on airless worlds).
  bool can_land() const;

  // Map mode (design/map-mode.md section 1/4): pushes the current mode
  // and freezes the player (position/orientation stay PLANET-LOCAL, so
  // the return spot rides along with the planet's orbital motion).
  // exit_map restores the pushed mode at the exact planet-local pose with
  // velocity zero; walking re-grounds via the normal radial snap on the
  // next update. Landing/takeoff transitions complete as their pushed
  // target mode. The app owns the map CAMERA — the player is simply
  // suspended while mode() == Map.
  void enter_map();
  void exit_map();

  // Interplanetary flight: swap the effective field (and coordinate
  // frame) to another anchor body. position is the player's pose in the
  // TARGET body's planet-local frame; attitude and speed carry over,
  // beams are dropped (they were in the old frame).
  void rebase(const gen::EffectiveField& field, const Vec3& position);

  // Sphere keep-out for bodies WITHOUT a terrain field (gas giants,
  // moons, the star): if the player is inside (center, min_dist), push
  // radially back to the boundary. The anchor body's real ground uses the
  // effective-field clamp instead.
  void push_out(const Vec3& center, double min_dist);

 private:
  void update_flight(const InputFrame& input);
  void update_on_foot(const InputFrame& input);
  void update_transition(const InputFrame& input);
  void update_beams(double dt);
  void try_fire(const InputFrame& input);
  double ground_radius(const Vec3& dir) const;
  void clamp_to_ground_flight();
  NearestBody nearest_or_anchor() const;

  const gen::EffectiveField* field_;  // current anchor body (never null)
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

  NearestBody nearest_;
  bool has_nearest_ = false;

  PlayerMode map_pushed_mode_ = PlayerMode::Flight;
};

}  // namespace inf::sim
