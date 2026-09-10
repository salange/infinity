#include "window_floor_filter.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
namespace wf = cb::window_floor;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
double error(wf::Fields a, wf::Fields b) {
  return std::max({std::abs(a.lit - b.lit), std::abs(a.gradient - b.gradient),
                   std::abs(a.joint - b.joint)});
}
} // namespace
int main() {
  try {
    auto active = [](long long row) { return (row % 7 + 7) % 7 < 3; };
    std::size_t integrated = 0;
    double maximum_quadrature_error = 0;
    for (double width : {1e-6, .02, .2, .666666})
      for (int index = -40; index <= 40; ++index)
        for (double offset :
             {-.999999, -.5, -.000001, 0., .000001, .499, .999999}) {
          const double position = index * .77 + offset;
          const auto actual = wf::integrate(position, width, active);
          wf::Fields reference;
          constexpr int samples = 4096;
          // Independently sample the original pointwise floor distribution and
          // ceiling polynomial. No closed-form integration or blend is reused.
          for (int i = 0; i < samples; ++i) {
            const double y = position + width * ((i + .5) / samples - .5);
            const auto floor = static_cast<long long>(std::floor(y));
            const double t = y - std::floor(y);
            const double lit = .725 * (active(floor) ? .85 : .10);
            const double gradient = .55 + .75 * t * t;
            reference.lit += lit / samples;
            reference.gradient += gradient / samples;
            reference.joint += lit * gradient / samples;
          }
          const double difference = error(actual, reference);
          maximum_quadrature_error =
              std::max(maximum_quadrature_error, difference);
          require(
              difference < .0005,
              "floor footprint disagrees with original pointwise quadrature");
          ++integrated;
        }
    const auto crossing = wf::integrate(0., .2, active);
    require(std::abs(crossing.joint - crossing.lit * crossing.gradient) > .03,
            "lit/gradient correlation was incorrectly separated");
    for (double probability : {.045, .19, .5}) {
      const double q = probability * .78;
      const double mean = .725 * (.10 + .75 * q);
      for (double y : {-12.7, -.000001, 0., .12, .999999, 123.}) {
        const auto unresolved = wf::filter(y, .75, 1.2, q, .413, active);
        require(error(unresolved, {mean, .8, mean * .8}) < 1e-15,
                "fully unresolved21 expectation changed");
        const auto resolved = wf::filter(y, 24., 24., q, .413, active);
        const double phase = y - std::floor(y),
                     gradient = .55 + .75 * phase * phase;
        require(error(resolved, {.413, gradient, .413 * gradient}) < 1e-15,
                "resolved room pattern changed");
      }
      // Exact q counts over a periodic bank of10000 floors let us check mean
      // energy without mistaking random finite-sample variation for drift.
      constexpr int period = 10000, samples_per_floor = 64;
      const int count = int(std::round(q * period));
      auto rows = [=](long long row) {
        return (row % period + period) % period < count;
      };
      for (double px : {.8, 2.5, 10.})
        for (double py : {1.2, 2., 8.}) {
          wf::Fields average;
          for (int i = 0; i < period * samples_per_floor; ++i) {
            const double position = (i + .5) / samples_per_floor;
            const double original_lit =
                .725 * (rows(static_cast<long long>(position)) ? .85 : .10);
            const auto value =
                wf::filter(position, px, py, q, original_lit, rows);
            average.lit += value.lit / (period * samples_per_floor);
            average.gradient += value.gradient / (period * samples_per_floor);
            average.joint += value.joint / (period * samples_per_floor);
          }
          require(error(average, {mean, .8, mean * .8}) < .000016,
                  "separable blending changed full-bank average energy");
        }
    }
    std::cout << "PASS " << integrated
              << " independent floor integrals; max quadrature error "
              << maximum_quadrature_error
              << "; joint correlation,21 endpoint contracts "
                 "and27 probability/footprint energy banks\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
