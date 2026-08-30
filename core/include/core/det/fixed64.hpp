#pragma once

#include <cassert>
#include <compare>
#include <cstdint>
#include <limits>

#include "core/det/int128.hpp"

namespace inf::det {

// Signed Q32.32 fixed point (DECISIONS 2026-08-30: DET-FIXED class).
// Range ±2^31 (±2.1e9) in whole units, resolution 2^-32 (~2.3e-10).
// Positions and identity-critical continuous math use this type; all
// multiply/divide paths go through 128-bit intermediates, so the result
// is bit-exact on every platform by construction.
//
// Overflow policy: debug builds assert; release builds saturate to the
// representable extremes (documented in T0003).
class Fixed64 {
 public:
  static constexpr std::int32_t kFractionBits = 32;
  static constexpr std::int64_t kOne = std::int64_t{1} << kFractionBits;

  constexpr Fixed64() = default;

  static constexpr Fixed64 from_raw(std::int64_t raw) { return Fixed64(raw); }
  static constexpr Fixed64 from_int(std::int32_t value) {
    return Fixed64(static_cast<std::int64_t>(value) << kFractionBits);
  }
  // Boundary-only conversion (declared conversion sites, spec section 8).
  static Fixed64 from_double(double value) {
    return Fixed64(static_cast<std::int64_t>(value * static_cast<double>(kOne)));
  }

  constexpr std::int64_t raw() const { return raw_; }
  double to_double() const { return static_cast<double>(raw_) / static_cast<double>(kOne); }

  static constexpr Fixed64 max() { return Fixed64(std::numeric_limits<std::int64_t>::max()); }
  static constexpr Fixed64 min() { return Fixed64(std::numeric_limits<std::int64_t>::min()); }

  friend constexpr Fixed64 operator+(Fixed64 a, Fixed64 b) {
    std::int64_t result = 0;
    if (add_overflows(a.raw_, b.raw_, &result)) {
      assert(false && "Fixed64 addition overflow");
      return b.raw_ > 0 ? max() : min();
    }
    return Fixed64(result);
  }
  friend constexpr Fixed64 operator-(Fixed64 a, Fixed64 b) {
    std::int64_t result = 0;
    if (sub_overflows(a.raw_, b.raw_, &result)) {
      assert(false && "Fixed64 subtraction overflow");
      return b.raw_ < 0 ? max() : min();
    }
    return Fixed64(result);
  }
  friend constexpr Fixed64 operator-(Fixed64 a) {
    if (a.raw_ == std::numeric_limits<std::int64_t>::min()) {
      assert(false && "Fixed64 negation overflow");
      return max();
    }
    return Fixed64(-a.raw_);
  }

  // (a * b) >> 32 with a full 128-bit intermediate.
  friend Fixed64 operator*(Fixed64 a, Fixed64 b);
  // (a << 32) / b with a full 128-bit intermediate. b must be nonzero.
  friend Fixed64 operator/(Fixed64 a, Fixed64 b);

  friend constexpr bool operator==(Fixed64 a, Fixed64 b) = default;
  friend constexpr auto operator<=>(Fixed64 a, Fixed64 b) { return a.raw_ <=> b.raw_; }

 private:
  constexpr explicit Fixed64(std::int64_t raw) : raw_(raw) {}

  static constexpr bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t* out) {
#if defined(_MSC_VER) && !defined(__clang__)
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    const auto ur = ua + ub;
    *out = static_cast<std::int64_t>(ur);
    return ((~(ua ^ ub)) & (ua ^ ur) & 0x8000000000000000ULL) != 0;
#else
    return __builtin_add_overflow(a, b, out);
#endif
  }
  static constexpr bool sub_overflows(std::int64_t a, std::int64_t b, std::int64_t* out) {
#if defined(_MSC_VER) && !defined(__clang__)
    const auto ua = static_cast<std::uint64_t>(a);
    const auto ub = static_cast<std::uint64_t>(b);
    const auto ur = ua - ub;
    *out = static_cast<std::int64_t>(ur);
    return (((ua ^ ub)) & (ua ^ ur) & 0x8000000000000000ULL) != 0;
#else
    return __builtin_sub_overflow(a, b, out);
#endif
  }

  std::int64_t raw_{0};
};

inline constexpr Fixed64 kFixedOne = Fixed64::from_int(1);

Fixed64 abs(Fixed64 v);
Fixed64 floor(Fixed64 v);
Fixed64 ceil(Fixed64 v);
Fixed64 frac(Fixed64 v);  // v - floor(v), in [0, 1)
Fixed64 min(Fixed64 a, Fixed64 b);
Fixed64 max(Fixed64 a, Fixed64 b);
Fixed64 clamp(Fixed64 v, Fixed64 lo, Fixed64 hi);
// a + (b - a) * t
Fixed64 lerp(Fixed64 a, Fixed64 b, Fixed64 t);
// Integer Newton square root; v must be >= 0.
Fixed64 sqrt(Fixed64 v);

}  // namespace inf::det
