#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "core/det/mix.hpp"
#include "world/noise.hpp"

using namespace inf;
using det::Real;
using world::FbmParams;
using world::NoiseD;

namespace {

// Deterministic test points from the sanctioned mixer (no platform RNG).
double u01(std::uint64_t word) {
  return static_cast<double>(word >> 11U) * 0x1.0p-53;
}

struct Point {
  Real x, y, z;
};

Point test_point(std::uint64_t index) {
  const std::uint64_t a = det::mix64(0x7E57B0D5ULL + index * 3U);
  const std::uint64_t b = det::mix64(0x7E57B0D5ULL + index * 3U + 1U);
  const std::uint64_t c = det::mix64(0x7E57B0D5ULL + index * 3U + 2U);
  // Span several lattice cells, avoid exact integers.
  return Point{Real(u01(a) * 37.0 - 18.5), Real(u01(b) * 37.0 - 18.5),
               Real(u01(c) * 37.0 - 18.5)};
}

// Verifies the analytic gradient of `field` against central differences at
// `count` deterministic points. Non-smooth points (the ridge blend's |n|
// crease) are detected by disagreeing one-sided differences and skipped —
// the derivative is genuinely undefined there.
template <typename Field>
void check_gradient(const Field& field, int count) {
  const double h = 1e-5;
  int checked = 0;
  int skipped = 0;
  for (int i = 0; i < count; ++i) {
    const Point p = test_point(static_cast<std::uint64_t>(i));
    const NoiseD analytic = field.eval_d(p.x, p.y, p.z);
    CHECK(analytic.value.to_double() ==
          doctest::Approx(field.eval(p.x, p.y, p.z).to_double()).epsilon(1e-12));
    const double partials[3] = {analytic.dx.to_double(), analytic.dy.to_double(),
                                analytic.dz.to_double()};
    for (int axis = 0; axis < 3; ++axis) {
      Point lo = p;
      Point hi = p;
      Real* lo_c = axis == 0 ? &lo.x : axis == 1 ? &lo.y : &lo.z;
      Real* hi_c = axis == 0 ? &hi.x : axis == 1 ? &hi.y : &hi.z;
      *lo_c = *lo_c - Real(h);
      *hi_c = *hi_c + Real(h);
      const double f_lo = field.eval(lo.x, lo.y, lo.z).to_double();
      const double f_mid = field.eval(p.x, p.y, p.z).to_double();
      const double f_hi = field.eval(hi.x, hi.y, hi.z).to_double();
      const double forward = (f_hi - f_mid) / h;
      const double backward = (f_mid - f_lo) / h;
      if (std::abs(forward - backward) > 1e-2 * (1.0 + std::abs(forward))) {
        ++skipped;  // crease of the ridge blend: not differentiable here
        continue;
      }
      const double central = (f_hi - f_lo) / (2.0 * h);
      const double tolerance = 1e-3 * (1.0 > std::abs(central) ? 1.0 : std::abs(central));
      CAPTURE(i);
      CAPTURE(axis);
      REQUIRE(std::abs(partials[axis] - central) < tolerance);
      ++checked;
    }
  }
  // The crease filter must not have eaten the test.
  CHECK(checked > count * 2);
  CHECK(skipped < count / 4);
}

constexpr std::uint64_t kKey = 0xD00DFEEDFACE42ULL;

}  // namespace

TEST_CASE("noise: analytic gradient matches central difference — gradient noise") {
  struct {
    Real eval(Real x, Real y, Real z) const { return world::gradient_noise3(kKey, x, y, z); }
    NoiseD eval_d(Real x, Real y, Real z) const {
      return world::gradient_noise3_d(kKey, x, y, z);
    }
  } field;
  check_gradient(field, 2500);
}

TEST_CASE("noise: analytic gradient matches central difference — smooth fBm") {
  struct {
    FbmParams params;
    Real eval(Real x, Real y, Real z) const { return world::fbm3(kKey, x, y, z, params); }
    NoiseD eval_d(Real x, Real y, Real z) const {
      return world::fbm3_d(kKey, x, y, z, params);
    }
  } field;
  field.params.octaves = 6;
  check_gradient(field, 2500);
}

TEST_CASE("noise: analytic gradient matches central difference — ridged fBm") {
  struct {
    FbmParams params;
    Real eval(Real x, Real y, Real z) const { return world::fbm3(kKey, x, y, z, params); }
    NoiseD eval_d(Real x, Real y, Real z) const {
      return world::fbm3_d(kKey, x, y, z, params);
    }
  } field;
  field.params.octaves = 6;
  field.params.sharpness = Real(0.8);
  check_gradient(field, 2500);
}

TEST_CASE("noise: analytic gradient matches central difference — warped fBm") {
  struct {
    FbmParams params;
    Real eval(Real x, Real y, Real z) const {
      return world::warped_fbm3(kKey, x, y, z, params, Real(0.9));
    }
    NoiseD eval_d(Real x, Real y, Real z) const {
      return world::warped_fbm3_d(kKey, x, y, z, params, Real(0.9));
    }
  } field;
  field.params.octaves = 6;
  field.params.sharpness = Real(0.5);
  check_gradient(field, 2500);
}
