#include <doctest/doctest.h>

#include "core/det/fixed64.hpp"

using inf::det::Fixed64;

namespace {
Fixed64 fx(double v) { return Fixed64::from_double(v); }
}  // namespace

TEST_CASE("fixed64: basic arithmetic") {
  CHECK((Fixed64::from_int(2) + Fixed64::from_int(3)) == Fixed64::from_int(5));
  CHECK((Fixed64::from_int(2) - Fixed64::from_int(3)) == Fixed64::from_int(-1));
  CHECK((Fixed64::from_int(6) * Fixed64::from_int(7)) == Fixed64::from_int(42));
  CHECK((Fixed64::from_int(42) / Fixed64::from_int(6)) == Fixed64::from_int(7));
  CHECK((-Fixed64::from_int(5)) == Fixed64::from_int(-5));
}

TEST_CASE("fixed64: fractional math") {
  const Fixed64 half = Fixed64::from_int(1) / Fixed64::from_int(2);
  CHECK(half.raw() == Fixed64::kOne / 2);
  CHECK((half * Fixed64::from_int(10)) == Fixed64::from_int(5));
  CHECK((half + half) == Fixed64::from_int(1));

  // Signed rounding of mul/div is toward zero.
  const Fixed64 minus_half = Fixed64::from_int(-1) / Fixed64::from_int(2);
  CHECK(minus_half.raw() == -(Fixed64::kOne / 2));
  CHECK((minus_half * Fixed64::from_int(3)).to_double() == doctest::Approx(-1.5));
}

TEST_CASE("fixed64: floor/ceil/frac/abs/clamp/lerp") {
  CHECK(inf::det::floor(fx(2.75)) == Fixed64::from_int(2));
  CHECK(inf::det::floor(fx(-2.25)) == Fixed64::from_int(-3));
  CHECK(inf::det::ceil(fx(2.25)) == Fixed64::from_int(3));
  CHECK(inf::det::ceil(Fixed64::from_int(2)) == Fixed64::from_int(2));
  CHECK(inf::det::frac(fx(2.75)).to_double() == doctest::Approx(0.75));
  CHECK(inf::det::frac(fx(-2.25)).to_double() == doctest::Approx(0.75));
  CHECK(inf::det::abs(fx(-3.5)) == fx(3.5));
  CHECK(inf::det::clamp(fx(5.0), fx(0.0), fx(2.0)) == fx(2.0));
  CHECK(inf::det::lerp(Fixed64::from_int(0), Fixed64::from_int(10), fx(0.25)) ==
        fx(2.5));
}

TEST_CASE("fixed64: sqrt exactness") {
  CHECK(inf::det::sqrt(Fixed64::from_int(4)) == Fixed64::from_int(2));
  CHECK(inf::det::sqrt(Fixed64::from_int(0)) == Fixed64::from_int(0));
  CHECK(inf::det::sqrt(Fixed64::from_int(1)) == Fixed64::from_int(1));
  // Non-square: floor semantics on the raw grid, within one ulp of ideal.
  const double got = inf::det::sqrt(Fixed64::from_int(2)).to_double();
  CHECK(got == doctest::Approx(1.4142135623).epsilon(1e-9));
  // sqrt(x)^2 <= x for the floor-root on the raw lattice.
  const Fixed64 root = inf::det::sqrt(fx(1234.5678));
  CHECK(root * root <= fx(1234.5678));

  // Large values near the range edge stay exact.
  const Fixed64 big = Fixed64::from_int(2000000000);
  const Fixed64 big_root = inf::det::sqrt(big);
  CHECK(big_root.to_double() == doctest::Approx(44721.359549996).epsilon(1e-9));
}

TEST_CASE("fixed64: saturation at the range edges (release semantics)") {
#ifdef NDEBUG
  const Fixed64 maxv = Fixed64::max();
  CHECK((maxv + Fixed64::from_int(1)) == Fixed64::max());
  CHECK((Fixed64::min() - Fixed64::from_int(1)) == Fixed64::min());
  CHECK((maxv * Fixed64::from_int(2)) == Fixed64::max());
#endif
}

TEST_CASE("fixed64: fixed op sequence is bit-stable") {
  // Frozen fingerprint of a mixed op sequence; any change to fixed64
  // semantics must show up here (and in hash-core goldens).
  Fixed64 acc = Fixed64::from_int(3);
  acc = acc / Fixed64::from_int(7);
  acc = acc * fx(123.456);
  acc = acc + inf::det::sqrt(fx(9876.5));
  acc = inf::det::lerp(acc, fx(-42.42), fx(0.125));
  acc = acc - inf::det::floor(acc);
  CHECK(acc.raw() == 0x00000000f3961619LL);
}
