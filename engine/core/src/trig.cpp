#include "core/det/trig.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace inf::det {

namespace {

// sin kernel on [-pi/4, pi/4] (fdlibm __kernel_sin coefficients).
double sin_kernel(double x) {
  constexpr double s1 = -1.66666666666666324348e-01;
  constexpr double s2 = 8.33333333332248946124e-03;
  constexpr double s3 = -1.98412698298579493134e-04;
  constexpr double s4 = 2.75573137070700676789e-06;
  constexpr double s5 = -2.50507602534068634195e-08;
  constexpr double s6 = 1.58969099521155010221e-10;
  const double z = x * x;
  const double r = s2 + z * (s3 + z * (s4 + z * (s5 + z * s6)));
  return x + x * z * (s1 + z * r);
}

// cos kernel on [-pi/4, pi/4] (fdlibm __kernel_cos coefficients).
double cos_kernel(double x) {
  constexpr double c1 = 4.16666666666666019037e-02;
  constexpr double c2 = -1.38888888888741095749e-03;
  constexpr double c3 = 2.48015872894767294178e-05;
  constexpr double c4 = -2.75573143513906633035e-07;
  constexpr double c5 = 2.08757232129817482790e-09;
  constexpr double c6 = -1.13596475577881948265e-11;
  const double z = x * x;
  const double r = z * z * (c1 + z * (c2 + z * (c3 + z * (c4 + z * (c5 + z * c6)))));
  return 1.0 - 0.5 * z + r;
}

// atan on [0, 7/16] extended by range reduction (fdlibm aT series).
double atan_kernel(double x) {
  constexpr double a0 = 3.33333333333329318027e-01;
  constexpr double a1 = -1.99999999998764832476e-01;
  constexpr double a2 = 1.42857142725034663711e-01;
  constexpr double a3 = -1.11111104054623557880e-01;
  constexpr double a4 = 9.09088713343650656196e-02;
  constexpr double a5 = -7.69187620504482999495e-02;
  constexpr double a6 = 6.66107313738753120669e-02;
  constexpr double a7 = -5.83357013379057348645e-02;
  constexpr double a8 = 4.97687799461593236017e-02;
  constexpr double a9 = -3.65315727442169155270e-02;
  constexpr double a10 = 1.62858201153657823623e-02;
  const double z = x * x;
  const double w = z * z;
  const double s1 = z * (a0 + w * (a2 + w * (a4 + w * (a6 + w * (a8 + w * a10)))));
  const double s2 = w * (a1 + w * (a3 + w * (a5 + w * (a7 + w * a9))));
  return x - x * (s1 + s2);
}

double atan_positive(double x) {  // x >= 0
  constexpr double half_pi = 1.57079632679489661923;
  constexpr double quarter_pi = 0.78539816339744830962;
  if (x > 1.0) {
    return half_pi - atan_positive(1.0 / x);
  }
  if (x > 0.4375) {
    // atan(x) = pi/4 + atan((x-1)/(1+x)); the argument falls back into
    // the kernel's accurate range.
    return quarter_pi + atan_kernel((x - 1.0) / (1.0 + x));
  }
  return atan_kernel(x);
}

}  // namespace

Real wrap_two_pi(Real x) {
  const Real turns = det::floor(x / Real(kTwoPi));
  Real wrapped = x - turns * Real(kTwoPi);
  // Guard the boundary (floor rounding can leave exactly 2*pi).
  if (wrapped.to_double() >= kTwoPi) {
    wrapped = wrapped - Real(kTwoPi);
  }
  if (wrapped.to_double() < 0.0) {
    wrapped = wrapped + Real(kTwoPi);
  }
  return wrapped;
}

void sin_cos(Real x, Real* sine, Real* cosine) {
  // Quadrant reduction: x = k*(pi/2) + r, r in [-pi/4, pi/4].
  constexpr double half_pi = 1.57079632679489661923;
  constexpr double inv_half_pi = 0.63661977236758134308;
  const double xd = x.to_double();
  const double kd = det::floor(Real(xd * inv_half_pi + 0.5)).to_double();
  const double r = xd - kd * half_pi;
  const auto quadrant = static_cast<long long>(kd) & 3LL;
  const double s = sin_kernel(r);
  const double c = cos_kernel(r);
  switch (quadrant) {
    case 0: *sine = Real(s); *cosine = Real(c); break;
    case 1: *sine = Real(c); *cosine = Real(-s); break;
    case 2: *sine = Real(-s); *cosine = Real(-c); break;
    default: *sine = Real(-c); *cosine = Real(s); break;
  }
}

Real sin(Real x) {
  Real s(0.0);
  Real c(0.0);
  sin_cos(x, &s, &c);
  return s;
}

Real cos(Real x) {
  Real s(0.0);
  Real c(0.0);
  sin_cos(x, &s, &c);
  return c;
}

Real atan2(Real y, Real x) {
  const double yd = y.to_double();
  const double xd = x.to_double();
  if (yd == 0.0 && xd == 0.0) {
    return Real(0.0);
  }
  const double ay = yd < 0.0 ? -yd : yd;
  const double ax = xd < 0.0 ? -xd : xd;
  double angle = 0.0;
  if (ax >= ay) {
    angle = atan_positive(ay / ax);          // [0, pi/4]-ish
  } else {
    angle = 1.57079632679489661923 - atan_positive(ax / ay);
  }
  if (xd < 0.0) {
    angle = kPi - angle;
  }
  return Real(yd < 0.0 ? -angle : angle);
}

// --- fast deterministic approximations (T0017) ---------------------------

void fast_sin_cos(Real x, Real* sine, Real* cosine) {
  // Quadrant reduction by multiplication (exact floor), then the fdlibm
  // kernels on [-pi/4, pi/4].
  const double v = x.to_double();
  const double q = v * 0.6366197723675814;  // 2/pi
  const double kf = std::floor(q + 0.5);
  const double r = v - kf * 1.5707963267948966;
  const double rs = sin_kernel(r);
  const double rc = cos_kernel(r);
  const auto quadrant = static_cast<std::int64_t>(kf) & 3;
  switch (quadrant) {
    case 0: *sine = Real(rs); *cosine = Real(rc); break;
    case 1: *sine = Real(rc); *cosine = Real(-rs); break;
    case 2: *sine = Real(-rs); *cosine = Real(-rc); break;
    default: *sine = Real(-rc); *cosine = Real(rs); break;
  }
}

}  // namespace inf::det
