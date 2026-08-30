#include <doctest/doctest.h>

#include <cmath>

#include "core/det/trig.hpp"
#include "core/ephem/ephemeris.hpp"
#include "core/time/world_clock.hpp"

using inf::core::Ephemeris;
using inf::core::ManualClock;
using inf::core::OrbitalElements;
using inf::core::SpinState;
using inf::core::WorldTime;
using inf::det::Real;

namespace {

// Earth-analogue at game scale (1:10): a = 15 Gm, mu_sun/10.
OrbitalElements earth_like() {
  OrbitalElements el{};
  el.a_m = Real(1.495978707e10);       // 149.6 Gm / 10
  el.e = Real(0.0167);
  el.i_rad = Real(0.0);
  el.raan_rad = Real(0.0);
  el.argp_rad = Real(1.99);
  el.mean_anom_0_rad = Real(6.24);
  el.mu_parent = Real(1.32712440018e19);  // GM_sun / 10
  return el;
}

}  // namespace

TEST_CASE("kepler solver: satisfies the equation after 6 fixed steps") {
  for (double e : {0.0, 0.1, 0.3, 0.6, 0.9, 0.95}) {
    for (double m = 0.1; m < 6.2; m += 0.5) {
      const Real ecc_anom = Ephemeris::solve_kepler(Real(m), Real(e));
      const double residual =
          ecc_anom.to_double() - e * std::sin(ecc_anom.to_double()) - m;
      CHECK(std::abs(residual) < 1e-9);
    }
  }
}

TEST_CASE("ephemeris: period and radius match Kepler's third law") {
  const OrbitalElements el = earth_like();
  // Period = 2*pi*sqrt(a^3/mu): with the 1:10 rule this is a real year /10.
  const double a = el.a_m.to_double();
  const double period =
      2.0 * 3.14159265358979323846 * std::sqrt(a * a * a / el.mu_parent.to_double());
  CHECK(period == doctest::Approx(365.25 * 86400.0 / 10.0).epsilon(0.01));

  ManualClock clock(WorldTime::epoch());
  const auto p0 = Ephemeris::evaluate(el, clock.now());
  // One full period later: same position (closed-form, no drift).
  clock.set(WorldTime::from_seconds(period));
  const auto p1 = Ephemeris::evaluate(el, clock.now());
  CHECK(p1.x.to_double() == doctest::Approx(p0.x.to_double()).epsilon(1e-6));
  CHECK(p1.y.to_double() == doctest::Approx(p0.y.to_double()).epsilon(1e-6));

  // Radius stays within [a(1-e), a(1+e)]; speed near the vis-viva value.
  const double r = std::hypot(p0.x.to_double(), p0.y.to_double(), p0.z.to_double());
  CHECK(r >= a * (1.0 - el.e.to_double()) * 0.999);
  CHECK(r <= a * (1.0 + el.e.to_double()) * 1.001);
  const double v = std::hypot(p0.vx.to_double(), p0.vy.to_double(), p0.vz.to_double());
  const double vis_viva = std::sqrt(el.mu_parent.to_double() * (2.0 / r - 1.0 / a));
  CHECK(v == doctest::Approx(vis_viva).epsilon(1e-9));
  // 1:10 rule keeps orbital speeds REAL: ~29.8 km/s for the Earth-analogue.
  CHECK(v == doctest::Approx(29'780.0).epsilon(0.05));
}

TEST_CASE("ephemeris: evaluation is a pure function of (elements, t)") {
  const OrbitalElements el = earth_like();
  const WorldTime t = WorldTime::from_seconds(123456.789);
  const auto p1 = Ephemeris::evaluate(el, t);
  const auto p2 = Ephemeris::evaluate(el, t);
  CHECK(p1.x == p2.x);
  CHECK(p1.vy == p2.vy);
  // Negative time (before Epoch Zero) works identically.
  const auto past = Ephemeris::evaluate(el, WorldTime::from_seconds(-987654.3));
  CHECK(std::isfinite(past.x.to_double()));
}

TEST_CASE("spin: linear phase, wraps, tidally-locked convention") {
  SpinState spin{};
  spin.spin_rate_rad_s = Real(inf::det::kTwoPi / 8640.0);  // 2.4 h day
  spin.spin_phase_0_rad = Real(1.0);
  const Real angle = Ephemeris::rotation_angle(spin, WorldTime::from_seconds(8640.0));
  CHECK(angle.to_double() == doctest::Approx(1.0).epsilon(1e-9));
  const Real half = Ephemeris::rotation_angle(spin, WorldTime::from_seconds(4320.0));
  CHECK(half.to_double() == doctest::Approx(1.0 + 3.14159265).epsilon(1e-6));
}
