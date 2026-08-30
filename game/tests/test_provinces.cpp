#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

using namespace inf;
using det::Real;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

gen::Dir3 lerp_dir(const gen::Dir3& a, const gen::Dir3& b, double t) {
  const Real rt(t);
  return gen::normalize(gen::Dir3{det::lerp(a.x, b.x, rt), det::lerp(a.y, b.y, rt),
                                  det::lerp(a.z, b.z, rt)});
}

}  // namespace

TEST_CASE("provinces: cell params deterministic and within archetype ranges") {
  const gen::BodyHandle body = body_for(11);
  const gen::PlanetParams planet =
      gen::derive_planet_params(body.params, gen::PlanetType::EarthLike);
  const gen::ProvinceField field(body.entity, planet);

  for (const gen::CellId& cell : field.all_cells()) {
    const gen::ProvinceParams a = field.cell_params(cell);
    const gen::ProvinceParams b = field.cell_params(cell);
    REQUIRE(a.archetype == b.archetype);
    REQUIRE(a.relief_amplitude_m == b.relief_amplitude_m);
    REQUIRE(a.relief_amplitude_m.to_double() >= 50.0);
    REQUIRE(a.relief_amplitude_m.to_double() <= 3500.0);
    REQUIRE(a.ruggedness.to_double() >= 0.0);
    REQUIRE(a.ruggedness.to_double() <= 1.0);
    REQUIRE(a.carving.to_double() >= 0.0);
    REQUIRE(a.carving.to_double() <= 1.0);
  }
}

TEST_CASE("provinces: blended field is continuous across cube edges and corners") {
  const gen::BodyHandle body = body_for(23);
  const gen::PlanetParams planet =
      gen::derive_planet_params(body.params, gen::PlanetType::EarthLike);
  const gen::ProvinceField field(body.entity, planet);

  // Paths crossing all 12 cube edges + through 2 corners. Each path is a
  // dense spherical walk; the max relief step between consecutive samples
  // must scale with the step size (no jumps).
  const std::array<std::pair<gen::Dir3, gen::Dir3>, 14> paths = {{
      // Edge crossings: pairs of adjacent face centers.
      {{Real(1), Real(0.4), Real(0)}, {Real(0.4), Real(1), Real(0)}},    // +X/+Y
      {{Real(1), Real(-0.4), Real(0)}, {Real(0.4), Real(-1), Real(0)}},  // +X/-Y
      {{Real(1), Real(0), Real(0.4)}, {Real(0.4), Real(0), Real(1)}},    // +X/+Z
      {{Real(1), Real(0), Real(-0.4)}, {Real(0.4), Real(0), Real(-1)}},  // +X/-Z
      {{Real(-1), Real(0.4), Real(0)}, {Real(-0.4), Real(1), Real(0)}},  // -X/+Y
      {{Real(-1), Real(-0.4), Real(0)}, {Real(-0.4), Real(-1), Real(0)}},
      {{Real(-1), Real(0), Real(0.4)}, {Real(-0.4), Real(0), Real(1)}},
      {{Real(-1), Real(0), Real(-0.4)}, {Real(-0.4), Real(0), Real(-1)}},
      {{Real(0), Real(1), Real(0.4)}, {Real(0), Real(0.4), Real(1)}},    // +Y/+Z
      {{Real(0), Real(1), Real(-0.4)}, {Real(0), Real(0.4), Real(-1)}},
      {{Real(0), Real(-1), Real(0.4)}, {Real(0), Real(-0.4), Real(1)}},
      {{Real(0), Real(-1), Real(-0.4)}, {Real(0), Real(-0.4), Real(-1)}},
      // Corner passes.
      {{Real(1), Real(0.8), Real(0.8)}, {Real(0.8), Real(0.8), Real(1)}},
      {{Real(-1), Real(-0.8), Real(0.8)}, {Real(-0.8), Real(-0.8), Real(1)}},
  }};

  constexpr int kSteps = 400;
  for (std::size_t path_index = 0; path_index < paths.size(); ++path_index) {
    CAPTURE(path_index);
    const gen::Dir3 start = gen::normalize(paths[path_index].first);
    const gen::Dir3 end = gen::normalize(paths[path_index].second);
    double previous = field.sample(start).relief_amplitude_m.to_double();
    double max_step = 0.0;
    for (int i = 1; i <= kSteps; ++i) {
      const gen::Dir3 dir = lerp_dir(start, end, static_cast<double>(i) / kSteps);
      const double current = field.sample(dir).relief_amplitude_m.to_double();
      max_step = std::max(max_step, std::abs(current - previous));
      previous = current;
    }
    // Whole-path relief span is up to ~2000 m; a continuous field sampled
    // at 400 steps must move in small increments. A hard cutoff of 40 m
    // per step catches any face-seam jump (a discontinuity shows up as a
    // several-hundred-meter single-step change).
    CHECK(max_step < 40.0);
  }
}

TEST_CASE("provinces: EarthLike shows >=3 distinct archetypes (100 seeds)") {
  int ok = 0;
  for (std::uint64_t seed = 0; seed < 100; ++seed) {
    const gen::BodyHandle body = body_for(seed);
    const gen::PlanetParams planet =
        gen::derive_planet_params(body.params, gen::PlanetType::EarthLike);
    const gen::ProvinceField field(body.entity, planet);
    std::set<gen::Archetype> archetypes;
    for (const gen::CellId& cell : field.all_cells()) {
      archetypes.insert(field.cell_params(cell).archetype);
    }
    if (archetypes.size() >= 3) {
      ++ok;
    }
  }
  CHECK(ok >= 95);  // exit criterion 4 feeder (T0004)
}

TEST_CASE("provinces: type tables produce type-correct archetypes") {
  const gen::BodyHandle body = body_for(5);
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    const auto type = static_cast<gen::PlanetType>(type_index);
    const gen::PlanetParams planet = gen::derive_planet_params(body.params, type);
    const gen::ProvinceField field(body.entity, planet);
    for (const gen::CellId& cell : field.all_cells()) {
      const auto value = static_cast<std::uint8_t>(field.cell_params(cell).archetype);
      switch (type) {
        case gen::PlanetType::EarthLike: REQUIRE(value <= 4); break;
        case gen::PlanetType::Barren: REQUIRE((value >= 5 && value <= 7)); break;
        case gen::PlanetType::Desert: REQUIRE((value >= 8 && value <= 10)); break;
        case gen::PlanetType::Ice: REQUIRE((value >= 11 && value <= 13)); break;
      }
    }
  }
}
