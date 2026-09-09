#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include "sim/hyperdrive.hpp"

using namespace inf::sim;

TEST_CASE("hyperdrive: charge, measured FTL travel and frame-rate independent acceleration") {
  for (const double dt : {0.01, 0.02, 0.05, 0.1}) {
    Hyperdrive drive;
    double distance = 0;
    for (int i = 0; i < static_cast<int>(4.0 / dt); ++i) {
      distance += drive.update(dt, i == 0, false, false, false, {}, {1, 0, 0}, {});
      if ((i + 1) * dt < 0.8) CHECK(distance == 0);
    }
    CHECK(drive.speed() == doctest::Approx(2 * Hyperdrive::kLightSpeed));
    // 0.8 s charge + 0.5 s ramp (mean c) + 2.7 s at 2c.
    CHECK(distance == doctest::Approx(5.9 * Hyperdrive::kLightSpeed).epsilon(1e-10));
    CHECK(distance / 4.0 > Hyperdrive::kLightSpeed);
    CHECK(drive.effect() == doctest::Approx(1));
  }
}

TEST_CASE("hyperdrive: throttle bounds and immediate cancellation with optical fade") {
  Hyperdrive drive;
  for (int i = 0; i < 100; ++i)
    drive.update(.1, i == 0, false, true, false, {}, {1, 0, 0}, {});
  CHECK(drive.factor() == 10);
  CHECK(drive.speed() == 10 * Hyperdrive::kLightSpeed);
  for (int i = 0; i < 100; ++i)
    drive.update(.1, false, false, false, true, {}, {1, 0, 0}, {});
  CHECK(drive.factor() == 2);
  CHECK(drive.speed() == 2 * Hyperdrive::kLightSpeed);
  CHECK(drive.update(.1, false, true, true, false, {}, {1, 0, 0}, {}) == 0);
  CHECK_FALSE(drive.active());
  CHECK(drive.speed() == 0);
  CHECK(drive.effect() > 0);
  for (int i = 0; i < 4; ++i) drive.update(.1, false, false, false, false, {}, {1, 0, 0}, {});
  CHECK(drive.effect() == 0);
}

TEST_CASE("hyperdrive: swept checks stop at the first body, even beyond the endpoint") {
  for (const double radius : {200'000.0, 700'000.0, 8'000'000.0, 100'000'000.0}) {
    Hyperdrive drive;
    for (int i = 0; i < 100; ++i)
      drive.update(.1, i == 0, false, true, false, {}, {1, 0, 0}, {});
    const std::array<HyperObstacle, 3> bodies{{
        {{radius + 10'000'000, 0, 0}, radius},
        {{radius + 1'000'000, 0, 0}, radius},
        {{0, radius + 500'000, 0}, radius}}};
    const double distance = drive.update(.1, false, false, false, false, {}, {1, 0, 0}, bodies);
    CHECK(distance == doctest::Approx(999'999));
    CHECK(drive.state() == Hyperdrive::State::Proximity);
    CHECK(drive.speed() == 0);
    // Attempts from a city/atmosphere shell never charge or move.
    CHECK(drive.update(.1, true, false, false, false, bodies[0].center, {1, 0, 0}, bodies) == 0);
    CHECK_FALSE(drive.active());
  }
}

TEST_CASE("hyperdrive: off-axis misses, tangency and large-coordinate rebasing") {
  for (const Vec3 origin : {Vec3{}, Vec3{1e15, -2e15, 3e15}}) {
    Hyperdrive drive;
    for (int i = 0; i < 100; ++i)
      drive.update(.1, i == 0, false, true, false, origin, {1, 0, 0}, {});
    const std::array<HyperObstacle, 2> miss{{{origin + Vec3{1e6, 2001, 0}, 2000},
                                           {origin + Vec3{-1e6, 0, 0}, 2000}}};
    CHECK(drive.update(.1, false, false, false, false, origin, {1, 0, 0}, miss) > 1e8);
    const std::array<HyperObstacle, 1> tangent{{{origin + Vec3{1e6, 2000, 0}, 2000}}};
    CHECK(drive.update(.1, false, false, false, false, origin, {1, 0, 0}, tangent) == doctest::Approx(999'999));
    CHECK_FALSE(drive.active());
  }
  CHECK(Hyperdrive::body_clearance(637'100, 15'000) == 752'100);
  CHECK(Hyperdrive::body_clearance(8e6, 0) == 8.4e6);
}
