#include "../app/src/deep_sky_render.hpp"
#include "../app/src/galaxy_flythrough.hpp"
#include "gen/universe.hpp"
#include <doctest/doctest.h>
using namespace inf;
TEST_CASE("galaxy route crosses the authoritative center in 120 seconds") {
  for (const char *text : {"83", "123", "ffffffff"}) {
    const auto seed = core::parse_seed(text);
    const auto params = gen::home_galaxy_params(*seed);
    const app::GalaxyRoute route(params);
    CHECK(sim::length(route.position(60)) == 0.0);
    CHECK(sim::length(route.position(0)) ==
          doctest::Approx(0.55 * params.diameter_ly.to_double() *
                          gen::kLightYearM));
    CHECK(sim::length(route.position(0) + route.position(120)) == 0.0);
    CHECK(sim::length(route.position(-10) - route.position(0)) == 0.0);
    CHECK(sim::length(route.position(130) - route.position(120)) == 0.0);
    for (int t = 1; t <= 120; ++t) {
      CHECK(sim::length(route.position(t) - route.position(t - 1)) ==
            doctest::Approx(route.speed_mps()));
    }
  }
}
TEST_CASE("moving star catalogs preserve deterministic stationary photometry") {
  const auto seed = core::parse_seed("83");
  const auto params = gen::home_galaxy_params(*seed);
  const gen::GalaxyOctree octree(gen::home_galaxy_key(*seed), params);
  const auto eye = gen::home_system_position_m(params);
  const auto fixed = app::build_star_field_mesh(octree, eye, 8.3, 64);
  const auto moving =
      app::build_star_field_mesh(octree, eye, 8.3, 64, nullptr, true);
  REQUIRE(fixed.size() == moving.size());
  REQUIRE(!fixed.empty());
  CHECK(moving ==
        app::build_star_field_mesh(octree, eye, 8.3, 64, nullptr, true));
  for (std::size_t i = 0; i < fixed.size(); i += 10) {
    const sim::Vec3 pos{moving[i], moving[i + 1], moving[i + 2]};
    const auto dir = sim::normalize(pos);
    CHECK(dir.x == doctest::Approx(fixed[i]));
    CHECK(dir.y == doctest::Approx(fixed[i + 1]));
    CHECK(dir.z == doctest::Approx(fixed[i + 2]));
    for (std::size_t j = 3; j < 10; ++j)
      CHECK(moving[i + j] == fixed[i + j]);
  }
}
