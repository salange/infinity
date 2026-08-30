#pragma once

#include <cmath>

#include "core/det/fixed64.hpp"

namespace inf::det {

// DET-REAL façade (DECISIONS 2026-08-30): a controlled scalar for
// generation math whose determinism class is not yet settled. Wraps an
// IEEE-754 double but exposes ONLY the correctly-rounded basic operations
// (+, -, *, /, sqrt — bit-exact across platforms with contraction and
// fast-math disabled, which the build pins) plus exact operations (floor,
// abs, min/max). NO libm transcendentals — sin/exp/pow differ between
// platform libms; when a transcendental is needed it gets our own
// deterministic implementation.
//
// Every use is greppable ("det::Real"); the M8 golden-hash harness decides
// per call-site whether it stays Real or migrates to Fixed64.
class Real {
 public:
  constexpr Real() = default;
  constexpr explicit Real(double value) : value_(value) {}
  static constexpr Real from_int(int value) { return Real(static_cast<double>(value)); }
  static Real from_fixed(Fixed64 value) { return Real(value.to_double()); }

  constexpr double to_double() const { return value_; }
  Fixed64 to_fixed() const { return Fixed64::from_double(value_); }

  friend constexpr Real operator+(Real a, Real b) { return Real(a.value_ + b.value_); }
  friend constexpr Real operator-(Real a, Real b) { return Real(a.value_ - b.value_); }
  friend constexpr Real operator*(Real a, Real b) { return Real(a.value_ * b.value_); }
  friend constexpr Real operator/(Real a, Real b) { return Real(a.value_ / b.value_); }
  friend constexpr Real operator-(Real a) { return Real(-a.value_); }

  Real& operator+=(Real other) {
    value_ += other.value_;
    return *this;
  }
  Real& operator-=(Real other) {
    value_ -= other.value_;
    return *this;
  }
  Real& operator*=(Real other) {
    value_ *= other.value_;
    return *this;
  }

  friend constexpr bool operator==(Real a, Real b) { return a.value_ == b.value_; }
  friend constexpr auto operator<=>(Real a, Real b) { return a.value_ <=> b.value_; }

 private:
  double value_{0.0};
};

// IEEE-754 requires sqrt to be correctly rounded: portable.
inline Real sqrt(Real v) { return Real(std::sqrt(v.to_double())); }
// Exact operations: portable.
inline Real floor(Real v) { return Real(std::floor(v.to_double())); }
inline Real abs(Real v) { return Real(std::fabs(v.to_double())); }
inline Real min(Real a, Real b) { return b < a ? b : a; }
inline Real max(Real a, Real b) { return a < b ? b : a; }
inline Real clamp(Real v, Real lo, Real hi) { return min(max(v, lo), hi); }
inline Real lerp(Real a, Real b, Real t) { return a + (b - a) * t; }

}  // namespace inf::det
