#include "galaxy_flythrough.hpp"

#include <algorithm>
#include <cmath>

namespace inf::app {
namespace {
using sim::Vec3;
// Beta(12,3) cumulative profile: very gentle local departure, then increasing
// interstellar acceleration, with zero first/second derivatives at both ends.
double ramp(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return std::pow(x, 12) * (91.0 - 168.0 * x + 78.0 * x * x);
}
double integral(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return std::pow(x, 13) * (7.0 - 12.0 * x + 5.2 * x * x);
}
double ease(double x) {
  x = std::clamp(x, 0.0, 1.0);
  return x * x * x * (10.0 + x * (-15.0 + 6.0 * x));
}
}  // namespace

void GalaxyFlight::start(const gen::GalaxyParams& galaxy,
                         const GalaxyPose& pose) {
  initial_ = pose;
  initial_.forward = sim::normalize(pose.forward);
  initial_.up = sim::normalize(
      pose.up - initial_.forward * sim::dot(pose.up, initial_.forward));
  const Vec3 inward = sim::length(pose.position) > 1.0
                          ? sim::normalize(pose.position * -1.0)
                          : initial_.forward;
  const double angle =
      std::acos(std::clamp(sim::dot(inward, initial_.forward), -1.0, 1.0));
  alignment_ = angle > 0.174532925 ? std::max(1.0, angle) : 0.0;
  // Retain initial momentum during alignment and release it continuously as
  // the first-second interstellar ramp takes over (integral of 1-ramp = .8).
  origin_ = pose.position + pose.velocity * (alignment_ + 0.8);
  axis_ = sim::length(origin_) > 1.0 ? sim::normalize(origin_ * -1.0)
                                     : initial_.forward;
  const double outskirts =
      0.55 * galaxy.diameter_ly.to_double() * gen::kLightYearM;
  cruise_ =
      (sim::length(origin_) + outskirts) / (duration_s - alignment_ - 1.6);
  cancelled_ = false;
  clearance_ = {};
}

void GalaxyFlight::clear_departure_body(Vec3 from_body, double radius_m) {
  const double distance = sim::length(from_body);
  if (distance < 1 || radius_m <= 0) return;
  const double along = sim::dot(from_body, axis_);
  Vec3 side = from_body - axis_ * along;
  if (along >= 0 || sim::length(side) > radius_m * 1.1) return;
  const Vec3 radial = from_body * (1.0 / distance);
  if (sim::length(side) < distance * 1e-6) {
    side = initial_.up - axis_ * sim::dot(initial_.up, axis_);
  }
  clearance_ = radial * 2.0 + sim::normalize(side) * 4.0;
  clearance_scale_ = 4.0 * distance;
}

GalaxyPose GalaxyFlight::sample(double elapsed) const {
  const double t = std::clamp(elapsed, 0.0, end_time());
  if (cancelled_ && t >= stop_time_) {
    const double u = std::clamp(t - stop_time_, 0.0, 1.0);
    GalaxyPose pose = stopped_;
    // Integral of 1-smoothstep5: finite braking distance, no position/velocity
    // cut.
    const double distance =
        u - 2.5 * std::pow(u, 4) + 3.0 * std::pow(u, 5) - std::pow(u, 6);
    pose.position = pose.position + stopped_.velocity * distance;
    pose.velocity = stopped_.velocity * (1.0 - ease(u));
    return pose;
  }
  const double a = std::clamp(t - alignment_, 0.0, 1.0);
  double distance = integral(a);
  double speed = ramp(a);
  if (t > alignment_ + 1.0)
    distance += std::min(t, duration_s - 1.0) - alignment_ - 1.0;
  if (t > duration_s - 1.0) {
    const double remaining = duration_s - t;
    distance += 0.2 - integral(remaining);
    speed = ramp(remaining);
  }
  const double initial_time = std::min(t, alignment_) + a - integral(a);
  GalaxyPose pose = initial_;
  // This form preserves the original position exactly at t=0.
  pose.position = initial_.position + initial_.velocity * initial_time +
                  axis_ * (distance * cruise_);
  pose.velocity = initial_.velocity * (t <= alignment_ ? 1.0 : 1.0 - ramp(a)) +
                  axis_ * (speed * cruise_);
  // A smooth local detour starts outward if the center lies behind the
  // departure body. It vanishes once clear, leaving the galactic route intact.
  const double d = distance * cruise_;
  const double q = d / clearance_scale_;
  const double falloff = std::exp(-q);
  pose.position = pose.position + clearance_ * (d * falloff);
  pose.velocity =
      pose.velocity + clearance_ * (speed * cruise_ * falloff * (1.0 - q));
  Vec3 turn_axis = sim::cross(initial_.forward, axis_);
  const double turn_length = sim::length(turn_axis);
  turn_axis =
      turn_length > 1e-8 ? turn_axis * (1.0 / turn_length) : initial_.up;
  const double angle =
      std::acos(std::clamp(sim::dot(initial_.forward, axis_), -1.0, 1.0));
  const double turn = angle * ease(t / std::max(1.0, alignment_));
  pose.forward = sim::normalize(sim::rotate(initial_.forward, turn_axis, turn));
  pose.up = sim::normalize(sim::rotate(initial_.up, turn_axis, turn));
  return pose;
}

sim::Vec3 GalaxyFlight::displacement(double elapsed) const {
  GalaxyFlight relative = *this;
  relative.initial_.position = {};
  relative.stopped_.position = stopped_displacement_;
  return relative.sample(elapsed).position;
}

void GalaxyFlight::stop(double elapsed) {
  if (cancelled_ || elapsed >= duration_s) return;
  stopped_ = sample(elapsed);
  stopped_displacement_ = displacement(elapsed);
  stop_time_ = std::max(0.0, elapsed);
  cancelled_ = true;
}
bool GalaxyFlight::finished(double elapsed) const {
  return elapsed >= end_time();
}
double GalaxyFlight::center_time() const {
  return alignment_ + 0.8 + sim::length(origin_) / cruise_;
}
}  // namespace inf::app
