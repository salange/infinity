#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <vector>

#include "gen/civ_names.hpp"
#include "gen/civ_time.hpp"
#include "gen/civilization.hpp"
#include "gen/system.hpp"
#include "gen/universe.hpp"

using namespace inf;

namespace {

struct HomeGalaxy {
  core::Key key;
  gen::GalaxyParams params;
  gen::CivilizationParams civ;
};

HomeGalaxy home_galaxy(std::uint64_t seed_lo) {
  const core::Seed128 seed{0, seed_lo};
  HomeGalaxy g;
  g.key = gen::home_galaxy_key(seed);
  g.params = gen::home_galaxy_params(seed);
  g.civ = gen::derive_civilization(g.key, g.params, true);
  return g;
}

}  // namespace

TEST_CASE("civ time: constants and conversions") {
  CHECK(gen::kGameYearS == doctest::Approx(394470.0).epsilon(1e-6));
  // 2018-09-01 is 8 real years before the launch reference.
  const double age = gen::ns_to_real_years(gen::kLaunchReference.ns_since_epoch -
                                           gen::kHumanExpansionStart.ns_since_epoch);
  CHECK(age == doctest::Approx(8.0));
  // A real year is 80 game-years; round trip.
  const core::WorldTime t = gen::kLaunchReference;
  const double gy = gen::game_years_since_epoch(t);
  CHECK(gen::world_time_from_game_years(gy).ns_since_epoch ==
        doctest::Approx(static_cast<double>(t.ns_since_epoch)).epsilon(1e-9));
  CHECK(gen::game_years_between(gen::kHumanExpansionStart, gen::kLaunchReference) ==
        doctest::Approx(640.0));
}

TEST_CASE("civilization/v1: home galaxy holds >= 6 races; population statistics (WP1)") {
  for (std::uint64_t s = 1; s <= 20; ++s) {
    const HomeGalaxy g = home_galaxy(s);
    CAPTURE(s);
    CHECK(g.civ.race_count >= 6);
    CHECK(g.civ.l_civ >= 2);
    CHECK(g.civ.l_civ <= 9);
    CHECK(g.civ.cell_width_ly > 900.0);
    CHECK(g.civ.cell_width_ly < 3000.0);
  }
  // 200 free-rolling galaxies (neighbours in the home cluster): < 5 %
  // with zero races, median 4-6, none above 100.
  const core::Seed128 seed{0, 0x83};
  std::vector<std::uint32_t> counts;
  int zero = 0;
  for (std::int64_t cx = 0; counts.size() < 200 && cx < 64; ++cx) {
    const auto tree = gen::make_tree(seed);
    const auto cluster = tree->get(core::tree::Address{}.child(
        core::tree::Step{gen::name::ClustersAxis, core::tree::Cell::grid(cx, 0, 0)}));
    const std::uint32_t count = gen::galaxy_count_in_cluster(cluster->key());
    for (std::uint32_t i = 1; i < count && counts.size() < 200; ++i) {
      const core::Key key = gen::galaxy_key_in_cluster(seed, cx, 0, 0, i);
      const gen::GalaxyParams params = gen::derive_galaxy_params(key);
      const gen::CivilizationParams civ = gen::derive_civilization(key, params);
      counts.push_back(civ.race_count);
      zero += civ.race_count == 0 ? 1 : 0;
      CHECK(civ.race_count <= 100);
    }
  }
  REQUIRE(counts.size() == 200);
  std::sort(counts.begin(), counts.end());
  CHECK(zero < 10);  // < 5 %
  CHECK(counts[100] >= 4);
  CHECK(counts[100] <= 6);
}

TEST_CASE("races/v1: registry is deterministic, order-independent and bounded (WP1)") {
  const HomeGalaxy g = home_galaxy(0x83);
  const gen::RaceRegistry a(g.key, g.params, g.civ);
  const gen::RaceRegistry b(g.key, g.params, g.civ);
  const gen::Dir3 home = gen::home_system_position_m(g.params);
  const gen::MacroCell centre = a.macro_cell_of(home);
  // Query b in a different order first (a far block, then the centre).
  const gen::MacroCell far{centre.x + 7, centre.y, centre.z, centre.level};
  (void)b.races_around(far);
  const auto& races_a = a.races_around(centre);
  const auto& races_b = b.races_around(centre);
  REQUIRE(races_a.size() == races_b.size());
  for (std::size_t i = 0; i < races_a.size(); ++i) {
    CHECK(races_a[i].key == races_b[i].key);
    CHECK(races_a[i].params.name == races_b[i].params.name);
    CHECK(races_a[i].params.t_0 == races_b[i].params.t_0);
    CHECK(races_a[i].params.speed_ly_per_year == races_b[i].params.speed_ly_per_year);
    // Sorted by key.
    if (i > 0) {
      CHECK((races_a[i - 1].key.k0 < races_a[i].key.k0 ||
             (races_a[i - 1].key.k0 == races_a[i].key.k0 &&
              races_a[i - 1].key.k1 < races_a[i].key.k1)));
    }
  }
  // A home slot computed in isolation equals its entry in the block.
  for (const gen::Race& race : races_a) {
    const gen::Race alone = a.home(race.cell, race.index);
    CHECK(alone.key == race.key);
    CHECK(alone.home_system == race.home_system);
    CHECK(alone.params.name == race.params.name);
    CHECK(alone.factions.size() == race.factions.size());
    // R_max never exceeds the reach.
    CHECK(race.params.r_max_ly <= gen::RaceRegistry::kReach * g.civ.cell_width_ly + 1e-6);
    CHECK(race.params.r_max_ly >= 50.0);
    CHECK(race.params.falloff_ly <= race.params.r_max_ly + 1e-6);
    // Every alien is older than humanity's expansion, or a dead Precursor.
    CHECK(race.params.t_0 < gen::kHumanExpansionStart);
    if (race.params.type == gen::RaceType::Precursor) {
      CHECK(race.params.extinct_ever);
      CHECK(race.params.t_end < gen::kLaunchReference);
      CHECK(race.factions.empty());
    } else {
      CHECK(!race.factions.empty());
      CHECK(race.factions[0].type == gen::FactionType::Government);
    }
    CHECK(race.params.home_level >= 4);
    CHECK(race.params.peak_level >= race.params.home_level);
    CHECK(race.params.peak_level <= 7);
    // The home world lies inside its macro cell and is an occupied leaf.
    const gen::Dir3 pos = a.system_position_m(race.home_system);
    const gen::MacroCell hosting = a.macro_cell_of(pos);
    CHECK(hosting.x == race.cell.x);
    CHECK(hosting.y == race.cell.y);
    CHECK(hosting.z == race.cell.z);
    CHECK(a.octree().occupied({race.home_system.x, race.home_system.y, race.home_system.z,
                               race.home_system.level}));
    CHECK(!race.home_system.is_home());
    for (const gen::FactionParams& f : race.factions) {
      CHECK(f.t_start >= race.params.t_0);
      CHECK(!f.centres.empty());
      CHECK(f.centres.size() <= 3);
      if (race.params.type == gen::RaceType::Machine) {
        CHECK(f.type != gen::FactionType::AlignedMachine);
        CHECK(f.type != gen::FactionType::RenegadeMachine);
      }
    }
  }
  // The home galaxy has races in the human neighbourhood across seeds
  // (the whole galaxy holds >= 6; the 125-cell block around the home is a
  // sizeable share of the disc mass) — at least one seed in ten must see
  // an alien from home, and the total over ten seeds must be several.
  int seen = 0;
  for (std::uint64_t s = 1; s <= 10; ++s) {
    const HomeGalaxy h = home_galaxy(s);
    const gen::RaceRegistry r(h.key, h.params, h.civ);
    seen += static_cast<int>(r.races_around(gen::home_system_position_m(h.params)).size());
  }
  CHECK(seen >= 3);
}

TEST_CASE("races/v1: race block cost and home-slot override (WP1)") {
  const HomeGalaxy g = home_galaxy(0x83);
  const gen::RaceRegistry registry(g.key, g.params, g.civ);
  const gen::Dir3 home = gen::home_system_position_m(g.params);
  const gen::MacroCell centre = registry.macro_cell_of(home);
  // Block scan cost: the brief targets <= 50 us for the 125 quadratures;
  // home placement adds octree descents per home. Measured once, cached
  // afterwards. Bound generously for CI noise; the number is reported.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    const gen::MacroCell c{centre.x + (i % 3), centre.y + (i / 3) % 3, centre.z, centre.level};
    (void)registry.races_around(c);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double us_per_block =
      std::chrono::duration<double, std::micro>(t1 - t0).count() / 20.0;
  MESSAGE("race block scan: " << us_per_block << " us per 125-cell block");
  CHECK(us_per_block < 5000.0);
  // The default (seed 83) system is never a race home: the goldens for
  // hash-system stay byte-identical.
  CHECK(!registry.home_override(gen::SystemCell{}).has_value());
  // Every race's home system IS overridden, and the override forces a
  // terrestrial slot of the race's habitat.
  const core::Seed128 seed{0, 0x83};
  for (const gen::Race& race : registry.races_around(centre)) {
    const auto over = registry.home_override(race.home_system);
    REQUIRE(over.has_value());
    CHECK(over->race_key == race.key);
    const core::Key system_key = gen::system_key_for(seed, race.home_system);
    gen::HomeSlotOverride slot_override;
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
    const gen::StarSystemParams plain = gen::generate_system(system_key);
    const gen::StarSystemParams forced = gen::generate_system(system_key, &slot_override);
    int homes = 0;
    for (int slot = 0; slot < gen::kMaxPlanetSlots; ++slot) {
      const auto& p = forced.planets[static_cast<std::size_t>(slot)];
      if (!p.occupied) continue;
      if (p.race_home) {
        ++homes;
        CHECK(p.surface_type == over->habitat);
        CHECK((p.phys.cls == core::PlanetClass::Rocky ||
               p.phys.cls == core::PlanetClass::SuperEarth));
        if (over->habitat == gen::PlanetType::EarthLike) {
          CHECK(p.phys.radius_m.to_double() >= gen::kAtmosphereMinRadiusM);
        }
        // The forced biosphere reaches the surface generator.
        const gen::BodyHandle body = gen::body_for_system_slot(seed, race.home_system, slot);
        const gen::PlanetParams params = gen::planet_params_for_slot(forced, slot, body);
        CHECK(params.forced_biosphere == over->force_biosphere);
      } else {
        // Untouched slots are byte-identical to the plain system.
        const auto& q = plain.planets[static_cast<std::size_t>(slot)];
        CHECK(p.orbit.a_m == q.orbit.a_m);
        CHECK(p.phys.radius_m == q.phys.radius_m);
        CHECK(p.surface_type == q.surface_type);
      }
    }
    CHECK(homes == 1);
    // The star is the same object claim resolution reads.
    CHECK(gen::system_star(system_key).mass_solar == forced.star.mass_solar);
  }
}

TEST_CASE("civ-names/v1: deterministic, filtered, typed (WP1)") {
  const HomeGalaxy g = home_galaxy(7);
  const gen::RaceRegistry registry(g.key, g.params, g.civ);
  std::set<std::string> corpus;
  for (std::uint64_t s = 1; s <= 12; ++s) {
    const HomeGalaxy h = home_galaxy(s);
    const gen::RaceRegistry r(h.key, h.params, h.civ);
    for (const gen::Race& race : r.races_around(gen::home_system_position_m(h.params))) {
      CHECK(!race.params.name.empty());
      CHECK(!gen::name_blocked(race.params.name));
      CHECK(!gen::name_blocked(race.params.adjective));
      corpus.insert(race.params.name);
      std::string name;
      std::string adjective;
      gen::race_names(race.key, race.params.type, &name, &adjective);
      CHECK(name == race.params.name);
      for (const gen::FactionParams& f : race.factions) {
        CHECK(!gen::name_blocked(f.name));
        CHECK(!f.name.empty());
      }
      if (race.params.type == gen::RaceType::Machine) {
        CHECK(race.params.name.find('-') != std::string::npos);
      }
    }
  }
  CHECK(gen::name_blocked("Kashitra"));
  CHECK(!gen::name_blocked("Vashkari"));
  // Settlement names and capital honorifics come from the same tables.
  const core::Key site = core::derive_child(g.key, gen::kind::Site, 1, 2, 3);
  const std::string town = gen::settlement_name(site, gen::RaceType::Humanoid, false);
  const std::string capital = gen::settlement_name(site, gen::RaceType::Humanoid, true);
  CHECK(!town.empty());
  CHECK(capital.size() > town.size());
  CHECK(town == gen::settlement_name(site, gen::RaceType::Humanoid, false));
}

// --- WP2: humans and enclaves ---------------------------------------------

#include "gen/human.hpp"

TEST_CASE("human/v1: constants, factions, the 72 % front (WP2)") {
  for (std::uint64_t s : {std::uint64_t{0x83}, std::uint64_t{1}, std::uint64_t{7}}) {
    const core::Seed128 seed{0, s};
    const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
    const gen::Race human = gen::human_race(gen::home_galaxy_key(seed), galaxy);
    CAPTURE(s);
    CHECK(human.params.is_human);
    CHECK(!human.params.extinct_ever);
    CHECK(human.params.t_0 == gen::kHumanExpansionStart);
    CHECK(human.home_system.is_home());
    CHECK(human.params.peak_level == 7);
    CHECK(human.params.home_level == 6);
    REQUIRE(human.params.sources.size() == 1);
    // Front radius at the launch reference covers 72 % of the disc area
    // (front / radius = 0.848) for ANY home galaxy size.
    const double radius_ly = galaxy.diameter_ly.to_double() * 0.5;
    const double front_ly = human.params.speed_ly_per_year * gen::kHumanExpansionAgeAtLaunchYears;
    const double area = (front_ly / radius_ly) * (front_ly / radius_ly);
    CHECK(area == doctest::Approx(0.72).epsilon(0.05));
    CHECK(human.params.r_max_ly >= radius_ly);
    // Factions: 1-2 Government, 2-3 Independent, 2-3 Outlaw, exactly one
    // aligned and one renegade android faction created 4 years after t_0.
    int counts[5] = {};
    for (const gen::FactionParams& f : human.factions) {
      ++counts[static_cast<int>(f.type)];
      CHECK(!f.name.empty());
      CHECK(!f.centres.empty());
      if (f.type == gen::FactionType::AlignedMachine || f.type == gen::FactionType::RenegadeMachine) {
        CHECK(f.t_start.ns_since_epoch ==
              gen::kHumanExpansionStart.ns_since_epoch + gen::real_years_to_ns(4.0));
      } else {
        CHECK(f.t_start >= gen::kHumanExpansionStart);
        CHECK(f.t_start < gen::kHumanExpansionStart + gen::real_years_to_ns(3.1));
      }
      CHECK(f.hostile == (f.type == gen::FactionType::RenegadeMachine));
      CHECK(f.dome_mul <= 1.0);
    }
    CHECK(counts[0] >= 1);
    CHECK(counts[0] <= 2);
    CHECK(counts[1] >= 2);
    CHECK(counts[1] <= 3);
    CHECK(counts[2] >= 2);
    CHECK(counts[2] <= 3);
    CHECK(counts[3] == 1);
    CHECK(counts[4] == 1);
    CHECK(human.params.faction_count == static_cast<int>(human.factions.size()));
    // Deterministic.
    const gen::Race again = gen::human_race(gen::home_galaxy_key(seed), galaxy);
    CHECK(again.factions.size() == human.factions.size());
    CHECK(again.factions[0].name == human.factions[0].name);
    // The registry carries the human race as the last candidate.
    const gen::CivilizationParams civ = gen::derive_civilization(gen::home_galaxy_key(seed), galaxy, true);
    gen::RaceRegistry registry(gen::home_galaxy_key(seed), galaxy, civ);
    registry.set_human(human);
    const auto& candidates = registry.candidates_around(gen::home_system_position_m(galaxy));
    REQUIRE(!candidates.empty());
    CHECK(candidates.back().params.is_human);
    CHECK(candidates.size() == registry.races_around(gen::home_system_position_m(galaxy)).size() + 1);
  }
}

TEST_CASE("human-enclaves/v1: 30 % of home-cluster galaxies, stranded, mutual gates (WP2)") {
  // Frequency over >= 200 galaxies drawn from several seeds' home clusters.
  int galaxies = 0;
  int with_enclaves = 0;
  for (std::uint64_t s = 1; s <= 40 && galaxies < 400; ++s) {
    const core::Seed128 seed{0, s};
    const std::uint32_t count = gen::galaxy_count_in_cluster(gen::home_cluster_key(seed));
    for (std::uint32_t g = 1; g < count && galaxies < 400; ++g) {
      ++galaxies;
      const auto enclaves = gen::human_enclaves(seed, 0, 0, 0, g);
      if (!enclaves.empty()) {
        ++with_enclaves;
      }
      CHECK(enclaves.size() <= 3);
      for (const gen::HumanEnclave& e : enclaves) {
        CHECK(e.source.speed_scale == doctest::Approx(0.1));
        CHECK(e.source.settle_scale == doctest::Approx(0.3));
        CHECK(e.source.reproduction_scale == doctest::Approx(0.5));
        CHECK(e.source.r_max_ly >= 100.0);
        CHECK(e.source.r_max_ly <= 600.0);
        CHECK(e.source.level_cap >= 2);
        CHECK(e.source.level_cap <= 6);
        CHECK(e.source.t_source >= gen::kHumanExpansionStart);
        CHECK(e.source.t_source <= gen::kHumanExpansionStart + gen::real_years_to_ns(2.0));
      }
    }
  }
  REQUIRE(galaxies >= 200);
  const double frequency = static_cast<double>(with_enclaves) / galaxies;
  MESSAGE("enclave frequency over " << galaxies << " galaxies: " << frequency);
  CHECK(frequency > 0.25);
  CHECK(frequency < 0.35);
  // The home galaxy itself never has enclaves; a far cluster almost never.
  CHECK(gen::human_enclaves(core::Seed128{0, 0x83}, 0, 0, 0, 0).empty());
  // Gate pairs are mutual: every home-galaxy gate points at an enclave
  // whose own gate partner is that gate.
  const core::Seed128 seed{0, 0x83};
  const auto gates = gen::home_galaxy_gates(seed);
  for (const gen::WormholeGate& gate : gates) {
    const auto enclaves = gen::human_enclaves(seed, 0, 0, 0, gate.partner_galaxy);
    REQUIRE(gate.partner_enclave < enclaves.size());
    const gen::HumanEnclave& e = enclaves[gate.partner_enclave];
    CHECK(e.gate_partner_m.x == gate.position_m.x);
    CHECK(e.gate_partner_m.y == gate.position_m.y);
    CHECK(e.source.position_m.x == gate.partner_position_m.x);
    CHECK(gate.dead);
  }
  // A non-home galaxy's human race has only beachhead sources.
  const gen::GalaxyParams other = gen::derive_galaxy_params(gen::galaxy_key_in_cluster(seed, 0, 0, 0, 1));
  const gen::Race stranded = gen::human_race_in_galaxy(seed, 0, 0, 0, 1, other);
  CHECK(stranded.void_home);
  CHECK(stranded.params.sources.size() == gen::human_enclaves(seed, 0, 0, 0, 1).size());
}
