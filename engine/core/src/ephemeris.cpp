#include "core/ephem/ephemeris.hpp"

#include "core/det/trig.hpp"

namespace inf::core {

using det::Real;

Real Ephemeris::solve_kepler(Real mean_anomaly, Real eccentricity) {
  const Real e = det::clamp(eccentricity, Real(0.0), Real(kMaxEccentricity));
  const Real m = det::wrap_two_pi(mean_anomaly);
  // Standard starter; fixed Newton iteration count (determinism: the
  // iteration count must never depend on the data).
  Real ecc_anomaly = m + e * det::sin(m);
  for (int i = 0; i < kKeplerIterations; ++i) {
    Real s(0.0);
    Real c(0.0);
    det::sin_cos(ecc_anomaly, &s, &c);
    const Real f = ecc_anomaly - e * s - m;
    const Real fp = Real(1.0) - e * c;
    ecc_anomaly = ecc_anomaly - f / fp;
  }
  return ecc_anomaly;
}

PosVel Ephemeris::evaluate(const OrbitalElements& elements, WorldTime t) {
  const Real a = elements.a_m;
  const Real e = det::clamp(elements.e, Real(0.0), Real(kMaxEccentricity));
  const Real mu = elements.mu_parent;

  // Mean motion n = sqrt(mu / a^3); M = M0 + n * dt.
  const Real n = det::sqrt(mu / (a * a * a));
  const Real dt = t.seconds();
  const Real mean_anomaly = det::wrap_two_pi(elements.mean_anom_0_rad + n * dt);

  const Real ecc_anomaly = solve_kepler(mean_anomaly, e);
  Real sin_e(0.0);
  Real cos_e(0.0);
  det::sin_cos(ecc_anomaly, &sin_e, &cos_e);

  const Real one_minus_ecos = Real(1.0) - e * cos_e;
  const Real r = a * one_minus_ecos;
  const Real root = det::sqrt(Real(1.0) - e * e);

  // Perifocal (orbital-plane) position and velocity.
  const Real x_p = a * (cos_e - e);
  const Real y_p = a * root * sin_e;
  const Real vel_scale = det::sqrt(mu * a) / r;
  const Real vx_p = -vel_scale * sin_e;
  const Real vy_p = vel_scale * root * cos_e;

  // Rotate perifocal -> parent frame: Rz(raan) * Rx(i) * Rz(argp).
  Real sin_o(0.0), cos_o(0.0), sin_i(0.0), cos_i(0.0), sin_w(0.0), cos_w(0.0);
  det::sin_cos(elements.raan_rad, &sin_o, &cos_o);
  det::sin_cos(elements.i_rad, &sin_i, &cos_i);
  det::sin_cos(elements.argp_rad, &sin_w, &cos_w);

  const Real r11 = cos_o * cos_w - sin_o * sin_w * cos_i;
  const Real r12 = -cos_o * sin_w - sin_o * cos_w * cos_i;
  const Real r21 = sin_o * cos_w + cos_o * sin_w * cos_i;
  const Real r22 = -sin_o * sin_w + cos_o * cos_w * cos_i;
  const Real r31 = sin_w * sin_i;
  const Real r32 = cos_w * sin_i;

  PosVel out{};
  out.x = r11 * x_p + r12 * y_p;
  out.y = r21 * x_p + r22 * y_p;
  out.z = r31 * x_p + r32 * y_p;
  out.vx = r11 * vx_p + r12 * vy_p;
  out.vy = r21 * vx_p + r22 * vy_p;
  out.vz = r31 * vx_p + r32 * vy_p;
  return out;
}

Real Ephemeris::rotation_angle(const SpinState& spin, WorldTime t) {
  return det::wrap_two_pi(spin.spin_phase_0_rad + spin.spin_rate_rad_s * t.seconds());
}

}  // namespace inf::core
