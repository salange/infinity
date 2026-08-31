#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/det/real.hpp"

namespace inf::det {

// Deterministic transcendentals for the ephemeris path (DECISIONS
// 2026-08-30; planetary-systems spec section 6). Fixed-order minimax
// polynomial kernels with fixed coefficients evaluated in a fixed Horner
// order — only correctly-rounded IEEE basic ops, so results are bit-exact
// on every platform under the DET-REAL build rules. Never call libm's
// sin/cos/atan2 in identity-critical code; call these.
//
// Kernel coefficients after fdlibm (Sun Microsystems; use freely granted
// provided the notice is preserved). Accuracy ~1e-13 over the reduced
// range; argument reduction is exact-order double math (deterministic;
// absolute error grows benignly for astronomically large angles — wrap
// angles with wrap_two_pi first where they accumulate).

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;

// x reduced into [0, 2*pi) via floor — deterministic for any magnitude.
Real wrap_two_pi(Real x);

Real sin(Real x);
Real cos(Real x);
// Both at once (the ephemeris hot path).
void sin_cos(Real x, Real* sine, Real* cosine);
// Four-quadrant arctangent, result in (-pi, pi]; atan2(0, 0) == 0.
Real atan2(Real y, Real x);

// --- fast deterministic approximations (T0017) ---------------------------
// Pure IEEE-754 arithmetic + bit assembly, no libm: bit-identical on
// every platform, a handful of ns per call. Built for the galaxy density
// model, which is evaluated ~1e8 times per sky bake. Accuracy: fast_exp
// relative error < 3e-5, fast_log < 1e-9 — plenty for density fields,
// NOT a replacement for the fdlibm-grade kernels above.
// Inline on purpose: the density hot path makes several calls per
// evaluation and cross-TU call overhead would dominate the arithmetic.
inline Real fast_exp(Real x) {
  double v = x.to_double();
  if (v < -700.0) {
    return Real(0.0);
  }
  if (v > 700.0) {
    v = 700.0;
  }
  // exp(x) = 2^k * 2^f, k integer, f in [0, 1): degree-6 Taylor of 2^f in
  // ln2 powers, then exact power-of-two scaling by bit assembly.
  const double y = v * 1.4426950408889634;  // log2(e)
  const double kf = std::floor(y);
  const double f = y - kf;
  const double p =
      1.0 +
      f * (0.6931471805599453 +
           f * (0.2402265069591007 +
                f * (0.05550410866482158 +
                     f * (0.009618129107628477 +
                          f * (0.0013333558146428443 + f * 0.00015403530393381608)))));
  const auto ki = static_cast<std::int64_t>(kf);
  const std::uint64_t bits = static_cast<std::uint64_t>(ki + 1023) << 52U;
  double scale;
  std::memcpy(&scale, &bits, sizeof(scale));
  return Real(p * scale);
}

inline Real fast_log(Real x) {
  const double v = x.to_double();
  std::uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  std::int64_t exponent = static_cast<std::int64_t>((bits >> 52U) & 0x7FFU) - 1023;
  std::uint64_t mant_bits = (bits & 0xFFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
  double m;
  std::memcpy(&m, &mant_bits, sizeof(m));  // [1, 2)
  if (m > 1.4142135623730951) {
    m *= 0.5;
    exponent += 1;
  }
  // atanh series: ln m = 2s(1 + s^2/3 + s^4/5 + ...), |s| <= 0.1716.
  const double s = (m - 1.0) / (m + 1.0);
  const double s2 = s * s;
  const double p =
      1.0 + s2 * (0.3333333333333333 +
                  s2 * (0.2 + s2 * (0.14285714285714285 + s2 * 0.1111111111111111)));
  return Real(static_cast<double>(exponent) * 0.6931471805599453 + 2.0 * s * p);
}

void fast_sin_cos(Real x, Real* sine, Real* cosine);  // any finite x

}  // namespace inf::det
