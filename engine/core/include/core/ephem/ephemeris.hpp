#pragma once

#include "core/ephem/elements.hpp"
#include "core/time/world_time.hpp"

namespace inf::core {

// Pure closed-form ephemerides (planetary-systems spec section 6):
// position/velocity from Epoch-Zero elements at ANY absolute time — past
// or future — with zero accumulated state. det:: math throughout
// (identity-critical: docking/landing/collision read these). The Kepler
// solve runs a FIXED iteration count (never iterate-to-tolerance), so the
// bit pattern is platform-independent by construction.
struct PosVel {
  det::Real x, y, z;     // parent frame, game-scale meters
  det::Real vx, vy, vz;  // m/s
};

struct Ephemeris {
  static constexpr int kKeplerIterations = 6;
  static constexpr double kMaxEccentricity = 0.95;

  // Solve E - e*sin(E) = M for the eccentric anomaly (fixed 6 Newton
  // steps; e defensively clamped). Exposed for tests.
  static det::Real solve_kepler(det::Real mean_anomaly, det::Real eccentricity);

  // Position/velocity in the parent frame at absolute time t.
  static PosVel evaluate(const OrbitalElements& elements, WorldTime t);

  // Spin rotation angle about the body axis at absolute time t, in
  // [0, 2*pi).
  static det::Real rotation_angle(const SpinState& spin, WorldTime t);
};

}  // namespace inf::core
