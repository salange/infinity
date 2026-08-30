#include "core/det/fixed64.hpp"

#include <cmath>

namespace inf::det {

namespace {

struct U128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};
};

U128 mul_abs(std::uint64_t a, std::uint64_t b) {
  const Mul128 product = mul_64x64(a, b);
  return U128{product.hi, product.lo};
}

std::uint64_t abs_raw(std::int64_t v) {
  // Two's complement safe |v| as unsigned (works for INT64_MIN).
  return v < 0 ? ~static_cast<std::uint64_t>(v) + 1U : static_cast<std::uint64_t>(v);
}

Fixed64 saturate(bool negative) { return negative ? Fixed64::min() : Fixed64::max(); }

// magnitude is guaranteed <= 2^63 (negative) or < 2^63 (positive) here;
// the 2^63 magnitude maps exactly to INT64_MIN.
std::int64_t apply_sign(std::uint64_t magnitude, bool negative) {
  if (negative) {
    if (magnitude == 0x8000000000000000ULL) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }
  return static_cast<std::int64_t>(magnitude);
}

// Exact floor(sqrt(hi*2^64 + lo)). A float seed is used only as a starting
// estimate; the exact integer adjustment below makes the result independent
// of the seed's rounding, hence bit-exact on every platform.
std::uint64_t isqrt128(std::uint64_t hi, std::uint64_t lo) {
  const double approx =
      std::sqrt(static_cast<double>(hi) * 18446744073709551616.0 + static_cast<double>(lo));
  std::uint64_t estimate = 0;
  if (approx >= 18446744073709551615.0) {
    estimate = 0xFFFFFFFFFFFFFFFFULL;
  } else {
    estimate = static_cast<std::uint64_t>(approx);
  }

  const auto squared_leq = [&](std::uint64_t candidate) {
    const U128 square = mul_abs(candidate, candidate);
    return square.hi < hi || (square.hi == hi && square.lo <= lo);
  };

  // Pull the estimate down until estimate^2 <= N, then push it up while
  // (estimate+1)^2 <= N. The float seed is within a few ulps, so both
  // loops run at most a handful of iterations.
  while (estimate > 0 && !squared_leq(estimate)) {
    --estimate;
  }
  while (estimate < 0xFFFFFFFFFFFFFFFFULL && squared_leq(estimate + 1U)) {
    ++estimate;
  }
  return estimate;
}

}  // namespace

Fixed64 operator*(Fixed64 a, Fixed64 b) {
  const bool negative = (a.raw() < 0) != (b.raw() < 0);
  const U128 product = mul_abs(abs_raw(a.raw()), abs_raw(b.raw()));
  // (hi:lo) >> 32, rounding toward zero on the magnitude.
  const std::uint64_t high_part = product.hi >> 32U;
  const std::uint64_t magnitude = (product.hi << 32U) | (product.lo >> 32U);
  const std::uint64_t limit =
      negative ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL;
  if (high_part != 0 || magnitude > limit) {
    assert(false && "Fixed64 multiplication overflow");
    return saturate(negative);
  }
  return Fixed64::from_raw(apply_sign(magnitude, negative));
}

Fixed64 operator/(Fixed64 a, Fixed64 b) {
  assert(b.raw() != 0 && "Fixed64 division by zero");
  if (b.raw() == 0) {
    return saturate(a.raw() < 0);
  }
  const bool negative = (a.raw() < 0) != (b.raw() < 0);
  const std::uint64_t dividend = abs_raw(a.raw());
  const std::uint64_t divisor = abs_raw(b.raw());
  // numerator = dividend << 32 as a 128-bit value.
  const std::uint64_t num_hi = dividend >> 32U;
  const std::uint64_t num_lo = dividend << 32U;
  if (num_hi >= divisor) {
    // Quotient would need >= 64 bits.
    assert(false && "Fixed64 division overflow");
    return saturate(negative);
  }
#if defined(_MSC_VER) && !defined(__clang__)
  std::uint64_t remainder = 0;
  const std::uint64_t magnitude = _udiv128(num_hi, num_lo, divisor, &remainder);
#else
  const unsigned __int128 numerator =
      (static_cast<unsigned __int128>(num_hi) << 64U) | num_lo;
  const std::uint64_t magnitude = static_cast<std::uint64_t>(numerator / divisor);
#endif
  const std::uint64_t limit =
      negative ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL;
  if (magnitude > limit) {
    assert(false && "Fixed64 division overflow");
    return saturate(negative);
  }
  return Fixed64::from_raw(apply_sign(magnitude, negative));
}

Fixed64 abs(Fixed64 v) { return v.raw() < 0 ? -v : v; }

Fixed64 floor(Fixed64 v) {
  return Fixed64::from_raw(v.raw() & ~(Fixed64::kOne - 1));
}

Fixed64 ceil(Fixed64 v) {
  const std::int64_t mask = Fixed64::kOne - 1;
  if ((v.raw() & mask) == 0) {
    return v;
  }
  return floor(v) + kFixedOne;
}

Fixed64 frac(Fixed64 v) { return v - floor(v); }

Fixed64 min(Fixed64 a, Fixed64 b) { return a < b ? a : b; }
Fixed64 max(Fixed64 a, Fixed64 b) { return a < b ? b : a; }

Fixed64 clamp(Fixed64 v, Fixed64 lo, Fixed64 hi) { return min(max(v, lo), hi); }

Fixed64 lerp(Fixed64 a, Fixed64 b, Fixed64 t) { return a + (b - a) * t; }

Fixed64 sqrt(Fixed64 v) {
  assert(v.raw() >= 0 && "Fixed64 sqrt of negative value");
  if (v.raw() <= 0) {
    return Fixed64::from_raw(0);
  }
  // sqrt(raw * 2^-32) = isqrt(raw << 32) * 2^-32.
  const auto raw = static_cast<std::uint64_t>(v.raw());
  const std::uint64_t result = isqrt128(raw >> 32U, raw << 32U);
  return Fixed64::from_raw(static_cast<std::int64_t>(result));
}

}  // namespace inf::det
