#pragma once
#include "gen/galaxy.hpp"
#include "sim/vec3.hpp"

namespace inf::app {

struct GalaxyPose {
  sim::Vec3 position, forward, up, velocity;
};

// A camera controller only. The ordinary scene renderer owns every frame.
// Positions are galactocentric game metres; the generator is never modified.
class GalaxyFlight {
 public:
  static constexpr double duration_s = 120.0;
  void start(const gen::GalaxyParams& galaxy, const GalaxyPose& pose);
  void clear_departure_body(sim::Vec3 from_body, double radius_m);
  GalaxyPose sample(double elapsed) const;
  sim::Vec3 displacement(double elapsed) const;
  void stop(double elapsed);
  bool finished(double elapsed) const;
  double end_time() const { return cancelled_ ? stop_time_ + 1.0 : duration_s; }
  double center_time() const;
  bool cancelled() const { return cancelled_; }
  double cruise_speed() const { return cruise_; }

 private:
  GalaxyPose initial_{}, stopped_{};
  sim::Vec3 axis_{1, 0, 0}, origin_{}, stopped_displacement_{};
  double alignment_{0}, cruise_{0}, stop_time_{0};
  bool cancelled_{false};
  sim::Vec3 clearance_{};
  double clearance_scale_{1};
};

}  // namespace inf::app
