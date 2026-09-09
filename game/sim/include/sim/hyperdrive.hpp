#pragma once

#include <span>
#include "sim/vec3.hpp"

namespace inf::sim {

struct HyperObstacle {
  Vec3 center;
  double radius_m;
};

// Camera motion only; never affects procedural generation or world time.
class Hyperdrive {
 public:
  enum class State { Off, Charging, Cruise, Proximity };
  static constexpr double kLightSpeed = 299'792'458.0;
  static constexpr double kChargeSeconds = 0.8;
  static constexpr double kAcceleration = 4.0 * kLightSpeed;
  // Conservative shell outside terrain, cities, atmospheres and gas giants.
  static double body_clearance(double radius, double atmosphere);

  bool active() const { return state_ == State::Charging || state_ == State::Cruise; }
  State state() const { return state_; }
  double factor() const { return factor_; }
  double speed() const { return speed_; }
  double effect() const { return effect_; }
  double phase() const { return phase_; }
  void stop(bool clear_effect = false);
  // Returns swept-safe distance this frame. All obstacles use the player's frame.
  double update(double dt, bool toggle, bool cancel, bool faster, bool slower,
                Vec3 position, Vec3 forward, std::span<const HyperObstacle> obstacles);

 private:
  State state_{State::Off};
  double factor_{2.0}, speed_{0}, charge_{0}, effect_{0}, phase_{0};
};
}  // namespace inf::sim
