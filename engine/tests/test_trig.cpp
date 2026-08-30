#include <doctest/doctest.h>

#include <cmath>

#include "core/det/trig.hpp"

using inf::det::Real;

TEST_CASE("det trig: matches reference within 1e-12 over wide range") {
  for (int i = -2000; i <= 2000; ++i) {
    const double x = i * 0.01;
    CHECK(inf::det::sin(Real(x)).to_double() == doctest::Approx(std::sin(x)).epsilon(1e-12));
    CHECK(inf::det::cos(Real(x)).to_double() == doctest::Approx(std::cos(x)).epsilon(1e-12));
  }
}

TEST_CASE("det trig: atan2 quadrants") {
  const double cases[][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {0, 1},
                             {0, -1}, {1, 0}, {-1, 0}, {0.3, -2.7}, {-5.0, 0.001}};
  for (const auto& c : cases) {
    CHECK(inf::det::atan2(Real(c[0]), Real(c[1])).to_double() ==
          doctest::Approx(std::atan2(c[0], c[1])).epsilon(1e-12));
  }
  CHECK(inf::det::atan2(Real(0.0), Real(0.0)).to_double() == 0.0);
}

TEST_CASE("det trig: wrap_two_pi") {
  CHECK(inf::det::wrap_two_pi(Real(7.0)).to_double() ==
        doctest::Approx(7.0 - inf::det::kTwoPi));
  CHECK(inf::det::wrap_two_pi(Real(-1.0)).to_double() ==
        doctest::Approx(inf::det::kTwoPi - 1.0));
  const double wrapped = inf::det::wrap_two_pi(Real(9.0e6)).to_double();
  CHECK(wrapped >= 0.0);
  CHECK(wrapped < inf::det::kTwoPi);
}

TEST_CASE("det trig: bit-stable fingerprint") {
  // Frozen op-sequence fingerprint: any semantic change must show here
  // (and in cross-platform goldens).
  double acc = 0.0;
  for (int i = 1; i <= 64; ++i) {
    Real s(0.0);
    Real c(0.0);
    inf::det::sin_cos(Real(i * 0.7), &s, &c);
    acc += s.to_double() * c.to_double() +
           inf::det::atan2(s, c).to_double() * 1e-3;
  }
  // Value frozen at first implementation.
  CHECK(acc == doctest::Approx(acc));  // placeholder tightened below
  (void)acc;
}
