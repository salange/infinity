#include <doctest/doctest.h>

#include <cmath>

#include "core/ephem/ephemeris.hpp"
#include "gen/system.hpp"
#include "gen/universe.hpp"

using namespace inf;

namespace {

gen::StarSystemParams system_for(std::uint64_t lo) {
  const auto tree = gen::make_tree(core::Seed128{0, lo});
  const auto node = tree->get(gen::default_system_address());
  return gen::generate_system(node->key());
}

}  // namespace

TEST_CASE("system: deterministic and structurally sane") {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const gen::StarSystemParams a = system_for(seed);
    const gen::StarSystemParams b = system_for(seed);
    CAPTURE(seed);
    REQUIRE(a.archetype == b.archetype);
    REQUIRE(a.star.mass_solar == b.star.mass_solar);

    int occupied = 0;
    bool landable = false;
    double prev_a = 0.0;
    for (const auto& planet : a.planets) {
      if (!planet.occupied) continue;
      ++occupied;
      landable = landable || planet.landable;
      // Orbits strictly ordered outward, eccentricity generation-clamped.
      REQUIRE(planet.orbit.a_m.to_double() > prev_a);
      prev_a = planet.orbit.a_m.to_double();
      REQUIRE(planet.orbit.e.to_double() < 0.95);
      // 1:10 radius rule: the body sits inside ITS OWN class's range from
      // the single shared table (gen/planet.hpp), giants included —
      // Rocky ~223-892 km through GasGiant ~4778-7964 km.
      const gen::RadiusRange range = gen::radius_range_m(planet.phys.cls);
      REQUIRE(planet.phys.radius_m.to_double() >= range.lo_m);
      REQUIRE(planet.phys.radius_m.to_double() <= range.hi_m);
      // Surface gravity stays in the playable band (spec section 4).
      REQUIRE(planet.phys.g_surface.to_double() >= 1.0);
      REQUIRE(planet.phys.g_surface.to_double() <= 25.0);
      // Moons live inside a third of the Hill sphere by construction.
      for (const auto& moon : planet.moons) {
        REQUIRE(moon.orbit.mu_parent == planet.phys.mu);
        REQUIRE(moon.spin.tidally_locked);
      }
    }
    REQUIRE(occupied >= 1);
    REQUIRE(landable);  // default-spawn contract
    REQUIRE(!a.belts.empty());
  }
}

TEST_CASE("system: Hill spacing keeps neighbors >= 10 mutual Hill radii") {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    const gen::StarSystemParams system = system_for(seed);
    const gen::SystemPlanet* prev = nullptr;
    for (const auto& planet : system.planets) {
      if (!planet.occupied) continue;
      if (prev != nullptr) {
        const double m_sum =
            (prev->phys.mass_earth.to_double() + planet.phys.mass_earth.to_double()) *
            3.003e-6;
        const double mean_a =
            0.5 * (prev->orbit.a_m.to_double() + planet.orbit.a_m.to_double());
        const double hill =
            mean_a * std::cbrt(m_sum / (3.0 * system.star.mass_solar.to_double()));
        CAPTURE(seed);
        CHECK(planet.orbit.a_m.to_double() - prev->orbit.a_m.to_double() >=
              10.0 * hill * 0.999);
      }
      prev = &planet;
    }
  }
}

TEST_CASE("system: default-seed contract (seed 83)") {
  // Contract (2026-08-31): the app's DEFAULT seed must yield a system
  // with >= 5 planets, at least one of them EarthLike with >= 1 moon.
  // If a generation change trips this test, scan for a new qualifying
  // seed (infinity-cli dump-system) and update BOTH the app default
  // (game/app/src/main.cpp) and this test. Seed text is HEX: "83" = 0x83.
  const gen::StarSystemParams system = system_for(0x83);
  int occupied = 0;
  bool earthlike_with_moon = false;
  for (const auto& planet : system.planets) {
    if (!planet.occupied) continue;
    ++occupied;
    if (planet.surface_type == gen::PlanetType::EarthLike && !planet.moons.empty()) {
      earthlike_with_moon = true;
    }
  }
  CHECK(occupied >= 5);
  CHECK(earthlike_with_moon);
}

TEST_CASE("system: every planet is landable; companions orbit far out") {
  int with_companions = 0;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const gen::StarSystemParams system = system_for(seed);
    CAPTURE(seed);
    double outermost = 0.0;
    for (const auto& planet : system.planets) {
      if (!planet.occupied) continue;
      REQUIRE(planet.landable);  // uniform-planet rule
      outermost = std::max(outermost, planet.orbit.a_m.to_double());
    }
    REQUIRE(system.companions.size() <= 2);
    with_companions += system.companions.empty() ? 0 : 1;
    for (const auto& companion : system.companions) {
      // Wide S-type orbits: far beyond the outer planet, so the planet
      // architecture is untouched by the multistar layer.
      REQUIRE(companion.orbit.a_m.to_double() > outermost * 2.0);
      REQUIRE(companion.orbit.mu_parent.to_double() > 0.0);
      REQUIRE(companion.phys.temperature_k.to_double() > 1000.0);
    }
  }
  // ~1/3 of systems should be multiples; loose band for 40 samples.
  CHECK(with_companions >= 4);
  CHECK(with_companions <= 25);
}

TEST_CASE("system: forever-state — ephemeris returns, never drifts") {
  const gen::StarSystemParams system = system_for(7);
  const gen::SystemPlanet& planet =
      system.planets[static_cast<std::size_t>(gen::default_landable_slot(system))];
  const double a = planet.orbit.a_m.to_double();
  const double period = 2.0 * 3.14159265358979323846 *
                        std::sqrt(a * a * a / planet.orbit.mu_parent.to_double());
  const auto p0 = core::Ephemeris::evaluate(planet.orbit, core::WorldTime::epoch());
  // 100 periods later (decades of WorldTime — which spans +-292 years by
  // design): identical position to within meters. Closed form — an
  // integrator would have drifted long ago.
  const auto p100 = core::Ephemeris::evaluate(
      planet.orbit, core::WorldTime::from_seconds(100.0 * period));
  CHECK(std::abs(p100.x.to_double() - p0.x.to_double()) < 5.0);
  CHECK(std::abs(p100.y.to_double() - p0.y.to_double()) < 5.0);
}

TEST_CASE("system: dump payloads are reproducible") {
  const gen::StarSystemParams system = system_for(3);
  CHECK(gen::system_to_json(system) == gen::system_to_json(system));
  const auto table = gen::ephemeris_table_json(system, core::WorldTime::epoch(),
                                               86'400'000'000'000LL, 4);
  CHECK(table == gen::ephemeris_table_json(system, core::WorldTime::epoch(),
                                           86'400'000'000'000LL, 4));
}
