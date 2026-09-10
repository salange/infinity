#pragma once
// CPU counterpart of the occupied-glass vertical pixel-footprint integral.
// The row-state callback is the pre-existing world-anchored floor occupancy.
#include <algorithm>
#include <cmath>

namespace cb::window_floor {
struct Fields {
  double lit{}, gradient{}, joint{};
};
template <class FloorActive>
inline Fields integrate(double position, double width, FloorActive active) {
  width = std::max(width, 1e-6);
  const double local = position - std::floor(position);
  auto first = static_cast<long long>(std::floor(position));
  double first_length = width, second_length = 0, u = local - width * .5,
         v = local + width * .5;
  if (local < width * .5) {
    --first;
    first_length = width * .5 - local;
    second_length = width - first_length;
    u = 1 - first_length;
    v = 1;
  } else if (1 - local < width * .5) {
    second_length = width * .5 - (1 - local);
    first_length = width - second_length;
    u = 1 - first_length;
    v = 1;
  }
  Fields result;
  for (int i = 0; i < 2; ++i) {
    const double length = i ? second_length : first_length;
    if (length <= 0)
      continue;
    const double a = i ? 0 : u, b = i ? second_length : v;
    const double gradient = .55 + .25 * (a * a + a * b + b * b);
    const double lit = .725 * (active(first + i) ? .85 : .10);
    result.lit += lit * length / width;
    result.gradient += gradient * length / width;
    result.joint += lit * gradient * length / width;
  }
  return result;
}
inline double fade(double pixels) {
  return std::clamp((pixels - 1.5) / 4., 0., 1.);
}
inline Fields blend(Fields a, Fields b, double weight) {
  return {a.lit + (b.lit - a.lit) * weight,
          a.gradient + (b.gradient - a.gradient) * weight,
          a.joint + (b.joint - a.joint) * weight};
}
template <class FloorActive>
inline Fields filter(double position, double pixels_x, double pixels_y,
                     double floor_probability, double resolved_lit,
                     FloorActive active) {
  const double mean = .725 * (.10 + .75 * floor_probability);
  Fields fields{mean, .8, mean * .8};
  const double vertical = std::clamp((pixels_y - 1.5) / .5, 0., 1.);
  if (vertical > 0)
    fields =
        blend(fields, integrate(position, 1. / pixels_y, active), vertical);
  const double local = position - std::floor(position);
  const double gradient = .55 + .75 * local * local;
  return blend(fields, {resolved_lit, gradient, resolved_lit * gradient},
               std::min(fade(pixels_x), fade(pixels_y)));
}
} // namespace cb::window_floor
