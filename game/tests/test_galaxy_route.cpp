#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "../app/src/galaxy_flythrough.hpp"
#include "../app/src/stellar_stream.hpp"
#include "gen/universe.hpp"
using namespace inf;
namespace {
auto parameters() { return gen::home_galaxy_params(*core::parse_seed("83")); }
app::GalaxyPose starting_pose() {
  const auto p = gen::home_system_position_m(parameters());
  return {{p.x.to_double(), p.y.to_double(), p.z.to_double()},
          {-1, 0, 0},
          {0, 0, 1},
          {-100, 0, 0}};
}
}  // namespace
TEST_CASE(
    "galaxy flight starts from the existing pose and ends beyond the center") {
  app::GalaxyFlight flight;
  const auto start = starting_pose();
  flight.start(parameters(), start);
  const auto first = flight.sample(0);
  CHECK(sim::length(first.position - start.position) == 0);
  CHECK(sim::length(first.forward - start.forward) < 1e-14);
  CHECK(sim::length(first.up - start.up) < 1e-14);
  CHECK(sim::length(first.velocity - start.velocity) < 1e-8);
  CHECK(sim::length(flight.sample(flight.center_time()).position) < 1024);
  const auto end = flight.sample(120);
  CHECK(sim::length(end.position) ==
        doctest::Approx(.55 * parameters().diameter_ly.to_double() *
                        gen::kLightYearM));
  CHECK(sim::length(end.velocity) < 1e-4);
  CHECK(flight.finished(120));
}
TEST_CASE("galaxy flight keeps roll and handles opposite and polar headings") {
  for (const sim::Vec3 forward :
       {sim::Vec3{1, 0, 0}, sim::Vec3{0, 0, 1}, sim::Vec3{0, -1, 0}}) {
    auto start = starting_pose();
    start.forward = forward;
    start.up = sim::normalize(sim::cross(forward, sim::Vec3{0.2, 1, 0.1}));
    app::GalaxyFlight flight;
    flight.start(parameters(), start);
    sim::Vec3 previous = forward;
    for (int frame = 0; frame <= 7200; ++frame) {
      const auto pose = flight.sample(frame / 60.0);
      CHECK(std::isfinite(pose.position.x));
      CHECK(sim::dot(pose.forward, previous) > .995);
      CHECK(sim::dot(pose.forward, pose.up) ==
            doctest::Approx(0).epsilon(1e-12));
      previous = pose.forward;
    }
  }
}
TEST_CASE(
    "cancellation preserves pose and velocity and stops at the reached "
    "location") {
  for (double t : {0.0, .2, .7, 1.5, 30.0, 60.0, 119.8}) {
    app::GalaxyFlight flight;
    flight.start(parameters(), starting_pose());
    const auto before = flight.sample(t);
    flight.stop(t);
    const auto after = flight.sample(t);
    CHECK(sim::length(after.position - before.position) == 0);
    CHECK(sim::length(after.velocity - before.velocity) == 0);
    const auto stopped = flight.sample(t + 1);
    CHECK(sim::length(stopped.velocity) == 0);
    CHECK(sim::length(stopped.position -
                      (before.position + before.velocity * .5)) < 1024);
    flight.stop(t + .2);
    CHECK(flight.end_time() == t + 1);
  }
}
TEST_CASE("galaxy flight speed agrees with its continuous trajectory") {
  app::GalaxyFlight flight;
  flight.start(parameters(), starting_pose());
  for (double t : {.1, .5, .99, 1.01, 3.0, 10.0, 70.0, 119.2, 119.8}) {
    constexpr double h = 1e-4;
    const auto velocity =
        (flight.displacement(t + h) - flight.displacement(t - h)) * (0.5 / h);
    const auto expected = flight.sample(t).velocity;
    CHECK(sim::length(velocity - expected) /
              std::max(1e8, sim::length(expected)) <
          .002);
  }
}
TEST_CASE("spatial sky generation does not depend on a route or observer") {
  const auto seed = *core::parse_seed("83");
  const auto first = app::build_galaxy_volume(seed, parameters(), 24);
  const auto second = app::build_galaxy_volume(seed, parameters(), 24);
  CHECK(first.rgba_half == second.rgba_half);
  CHECK(first.rgba_half.size() == 24 * 24 * 24 * 4);
  for (auto half : first.rgba_half) CHECK((half & 0x7c00U) != 0x7c00U);
}

TEST_CASE("stellar exposure culling never reseeds surviving points") {
  const auto seed = *core::parse_seed("83");
  const auto p = parameters();
  const gen::GalaxyOctree octree(gen::home_galaxy_key(seed), p);
  const auto start = starting_pose();
  const auto bright =
      app::build_stellar_catalog(octree, start.position, {}, nullptr, 4.0);
  const auto faint =
      app::build_stellar_catalog(octree, start.position, {}, nullptr, 5.0);
  REQUIRE(!bright.vertices.empty());
  std::set<std::array<float, 10>> complete;
  for (std::size_t i = 0; i < faint.vertices.size(); i += 60) {
    std::array<float, 10> star;
    std::copy_n(faint.vertices.data() + i, 10, star.begin());
    complete.insert(star);
  }
  for (std::size_t i = 0; i < bright.vertices.size(); i += 60) {
    std::array<float, 10> star;
    std::copy_n(bright.vertices.data() + i, 10, star.begin());
    CHECK(complete.contains(star));
  }
  const auto repeat =
      app::build_stellar_catalog(octree, start.position, {}, nullptr, 4.0);
  CHECK(repeat.vertices == bright.vertices);
  const std::atomic<bool> cancelled{true};
  CHECK(app::build_stellar_catalog(octree, start.position, {}, &cancelled, 5.0)
            .vertices.empty());
}
