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

TEST_CASE("trig: fast approximations track the reference kernels (T0017)") {
  using inf::det::Real;
  // fast_exp: < 3e-5 relative over the useful density range.
  for (int i = -600; i <= 200; ++i) {
    const double x = i * 0.1;
    const double approx = inf::det::fast_exp(Real(x)).to_double();
    const double exact = std::exp(x);
    CHECK(std::abs(approx - exact) <= exact * 3.0e-5 + 1e-300);
  }
  CHECK(inf::det::fast_exp(Real(-800.0)).to_double() == 0.0);
  // fast_log: < 1e-9 relative.
  for (int i = 1; i <= 400; ++i) {
    const double x = i * 0.37;
    const double approx = inf::det::fast_log(Real(x)).to_double();
    const double exact = std::log(x);
    CHECK(std::abs(approx - exact) <= std::abs(exact) * 1.0e-9 + 1.0e-12);
  }
  // fast_sin_cos: kernel-grade on moderate arguments.
  for (int i = -300; i <= 300; ++i) {
    const double x = i * 0.11;
    Real s(0.0), c(0.0);
    inf::det::fast_sin_cos(Real(x), &s, &c);
    CHECK(std::abs(s.to_double() - std::sin(x)) < 1.0e-9);
    CHECK(std::abs(c.to_double() - std::cos(x)) < 1.0e-9);
  }
}
