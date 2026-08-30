#pragma once

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

}  // namespace inf::det
