#include <doctest/doctest.h>

#include "core/key.hpp"
#include "gen/planet.hpp"

using namespace inf;

namespace {

core::Key body_for(std::uint64_t lo) {
  const core::Key universe = core::universe_key(core::Seed128{0, lo});
  const core::Key galaxy = core::derive_child(universe, core::Kind::Galaxy, 0, 0, 0);
  const core::Key system = core::derive_child(galaxy, core::Kind::System, 0);
  return core::derive_child(system, core::Kind::Body, 0);
}

}  // namespace

TEST_CASE("planet params: deterministic") {
  const gen::PlanetParams a = gen::derive_planet_params(body_for(7));
  const gen::PlanetParams b = gen::derive_planet_params(body_for(7));
  CHECK(a.type == b.type);
  CHECK(a.radius_m == b.radius_m);
  CHECK(a.palette_id == b.palette_id);
  CHECK(gen::derive_planet_params(body_for(8)).radius_m != a.radius_m);
}

TEST_CASE("planet params: ranges hold for 1000 seeds per type") {
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    const auto type = static_cast<gen::PlanetType>(type_index);
    for (std::uint64_t seed = 0; seed < 1000; ++seed) {
      const gen::PlanetParams params = gen::derive_planet_params(body_for(seed), type);
      CAPTURE(type_index);
      CAPTURE(seed);
      REQUIRE(params.type == type);
      REQUIRE(params.radius_m.to_double() >= 40'000.0);
      REQUIRE(params.radius_m.to_double() <= 100'000.0);
      REQUIRE(params.core_radius_m.to_double() > 0.0);
      REQUIRE(params.core_radius_m.to_double() < params.radius_m.to_double());
      REQUIRE(params.gravity.to_double() > 3.0);
      REQUIRE(params.gravity.to_double() < 20.0);
      REQUIRE(params.cells_per_face >= 3);
      REQUIRE(params.cells_per_face <= 7);
      if (type == gen::PlanetType::Barren) {
        REQUIRE(params.atmosphere_height_m.to_double() == 0.0);
        REQUIRE(params.sea_level_m.to_double() == 0.0);
      }
      if (type == gen::PlanetType::EarthLike) {
        REQUIRE(params.sea_level_m.to_double() >= 100.0);
        REQUIRE(params.atmosphere_height_m.to_double() >= 8'000.0);
      }
    }
  }
}

TEST_CASE("planet params: forced type changes only the type-dependent reads") {
  // Same body, two forced types: draws are identical, so shared machinery
  // stays comparable (both derive from the same words).
  const core::Key body = body_for(42);
  const gen::PlanetParams earth = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::PlanetParams barren = gen::derive_planet_params(body, gen::PlanetType::Barren);
  CHECK(earth.palette_id == barren.palette_id);  // same draw word
  CHECK(earth.sky_palette == barren.sky_palette);
}
