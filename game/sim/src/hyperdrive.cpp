#include "sim/hyperdrive.hpp"

#include <algorithm>
#include <cmath>

namespace inf::sim {
double Hyperdrive::body_clearance(double radius, double atmosphere) {
  return radius + std::max({100'000.0, atmosphere + 100'000.0, radius * 0.05});
}

void Hyperdrive::stop(bool clear_effect) {
  if (clear_effect) effect_ = 0;
  state_ = State::Off;
  speed_ = charge_ = 0;
}

double Hyperdrive::update(double dt, bool toggle, bool cancel, bool faster,
                         bool slower, Vec3 position, Vec3 forward,
                         std::span<const HyperObstacle> obstacles) {
  if (!std::isfinite(dt) || dt <= 0) return 0;
  dt = std::min(dt, 0.1);  // same simulation time budget as ordinary flight
  phase_ = std::fmod(phase_ + dt, 100.0);
  if (cancel || (toggle && active())) {
    stop();
  } else if (toggle) {
    state_ = State::Charging;
    charge_ = 0;
  }
  if (active()) {
    for (const auto& body : obstacles) {
      if (length(position - body.center) <= body.radius_m) {
        stop();
        state_ = State::Proximity;
        break;
      }
    }
  }
  const double wanted_effect = active() ? 1.0 : 0.0;
  effect_ += std::clamp(wanted_effect - effect_, -dt / 0.4, dt / kChargeSeconds);
  if (!active()) return 0;
  factor_ = std::clamp(factor_ + (static_cast<int>(faster) - static_cast<int>(slower)) * dt * 2.0,
                       2.0, 10.0);
  if (state_ == State::Charging) {
    const double consumed = std::min(dt, std::max(0.0, kChargeSeconds - charge_));
    charge_ += consumed;
    dt -= consumed;
    if (charge_ + 1e-12 < kChargeSeconds) return 0;
    state_ = State::Cruise;
  }
  const double target = factor_ * kLightSpeed;
  const double reach = std::min(dt, std::abs(target - speed_) / kAcceleration);
  const double next = speed_ + std::clamp(target - speed_, -kAcceleration * dt, kAcceleration * dt);
  double distance = (speed_ + next) * 0.5 * reach + next * (dt - reach);
  speed_ = next;
  // Ray/sphere entry rather than endpoint overlap: a frame may cross an entire
  // planet. Projection avoids subtracting two enormous squared distances.
  for (const auto& body : obstacles) {
    const Vec3 relative = body.center - position;
    const double along = dot(relative, forward);
    if (along < 0 || along > distance + body.radius_m) continue;
    const Vec3 perpendicular = relative - forward * along;
    const double gap2 = body.radius_m * body.radius_m - dot(perpendicular, perpendicular);
    if (gap2 < 0) continue;
    const double entry = along - std::sqrt(gap2);
    if (entry <= distance) {
      distance = std::max(0.0, entry - 1.0);
      stop();
      state_ = State::Proximity;
    }
  }
  return distance;
}
}  // namespace inf::sim
