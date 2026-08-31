#include <doctest/doctest.h>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"

using namespace inf;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

}  // namespace

TEST_CASE("planet params: deterministic") {
  const gen::PlanetParams a = gen::derive_planet_params(body_for(7).params);
  const gen::PlanetParams b = gen::derive_planet_params(body_for(7).params);
  CHECK(a.type == b.type);
  CHECK(a.radius_m == b.radius_m);
  CHECK(a.palette_id == b.palette_id);
  CHECK(gen::derive_planet_params(body_for(8).params).radius_m != a.radius_m);
}

TEST_CASE("planet params: ranges hold for 1000 seeds per type") {
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    const auto type = static_cast<gen::PlanetType>(type_index);
    for (std::uint64_t seed = 0; seed < 1000; ++seed) {
      const gen::PlanetParams params = gen::derive_planet_params(body_for(seed).params, type);
      CAPTURE(type_index);
      CAPTURE(seed);
      REQUIRE(params.type == type);
      // Radius must come from the shared class table (gen/planet.hpp) —
      // no second range lives in this test either.
      const gen::RadiusRange rocky = gen::radius_range_m(core::PlanetClass::Rocky);
      const gen::RadiusRange super = gen::radius_range_m(core::PlanetClass::SuperEarth);
      REQUIRE(params.radius_m.to_double() >= rocky.lo_m);
      REQUIRE(params.radius_m.to_double() <= super.hi_m);
      if (type == gen::PlanetType::EarthLike) {
        REQUIRE(params.radius_m.to_double() >= gen::kAtmosphereMinRadiusM);
      }
      REQUIRE(params.core_radius_m.to_double() > 0.0);
      REQUIRE(params.core_radius_m.to_double() < params.radius_m.to_double());
      REQUIRE(params.gravity.to_double() >= 1.0);
      REQUIRE(params.gravity.to_double() <= 25.0);
      REQUIRE(params.cells_per_face >= 3);
      REQUIRE(params.cells_per_face <= 24);
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
  const gen::BodyHandle body = body_for(42);
  const gen::PlanetParams earth = gen::derive_planet_params(body.params, gen::PlanetType::EarthLike);
  const gen::PlanetParams barren = gen::derive_planet_params(body.params, gen::PlanetType::Barren);
  CHECK(earth.palette_id == barren.palette_id);  // same draw word
  CHECK(earth.sky_palette == barren.sky_palette);
}
