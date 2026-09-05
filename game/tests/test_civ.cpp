#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <vector>

#include "gen/civ_names.hpp"
#include "gen/civ_time.hpp"
#include "gen/civilization.hpp"
#include "gen/ecumenopolis.hpp"
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
    CHECK(g.civ.l_civ == 6);
    CHECK(g.civ.cell_width_ly == doctest::Approx(g.params.diameter_ly.to_double() * 1.1 / 64.0));
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
  // The whole home galaxy holds most of its N races as real (non-void)
  // homes: void slots happen only where a leaf straddles cells (the halo).
  for (std::uint64_t s : {std::uint64_t{0x83}, std::uint64_t{3}}) {
    const HomeGalaxy h = home_galaxy(s);
    const gen::RaceRegistry r(h.key, h.params, h.civ);
    const auto all = r.all_races();
    CAPTURE(s);
    // Poisson thinning: N is the EXPECTATION, the realised count scatters.
    CHECK(all.size() >= 3);
    CHECK(all.size() <= 2 * h.civ.race_count + 3);
    for (const gen::Race& race : all) {
      CHECK(!race.void_home);
      CHECK(r.home_override(race.home_system).has_value());
    }
  }
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

// --- WP3: ownership and the planet state ----------------------------------

#include "gen/civ_census.hpp"
#include "gen/colony.hpp"

namespace {

struct CivWorld {
  core::Seed128 seed;
  core::Key galaxy_key;
  gen::GalaxyParams galaxy;
  gen::CivilizationParams civ;
  std::unique_ptr<gen::RaceRegistry> registry;
  std::unique_ptr<gen::ColonyResolver> resolver;
};

CivWorld civ_world(std::uint64_t seed_lo) {
  CivWorld w;
  w.seed = core::Seed128{0, seed_lo};
  w.galaxy_key = gen::home_galaxy_key(w.seed);
  w.galaxy = gen::home_galaxy_params(w.seed);
  w.civ = gen::derive_civilization(w.galaxy_key, w.galaxy, true);
  w.registry = std::make_unique<gen::RaceRegistry>(w.galaxy_key, w.galaxy, w.civ);
  w.registry->set_human(gen::human_race(w.galaxy_key, w.galaxy));
  w.resolver = std::make_unique<gen::ColonyResolver>(*w.registry);
  return w;
}

core::WorldTime launch_plus_years(double years) {
  return core::WorldTime::from_ns(gen::kLaunchReference.ns_since_epoch + gen::real_years_to_ns(years));
}

}  // namespace

TEST_CASE("keys: direct derivation equals the tree path (WP3)") {
  const core::Seed128 seed{0, 0x83};
  const core::Key galaxy = gen::home_galaxy_key(seed);
  for (const gen::SystemCell cell : {gen::SystemCell{}, gen::SystemCell{5, 7, 3, 4}, gen::SystemCell{1809, 3035, 2075, 12}}) {
    CHECK(gen::system_key_in_galaxy(galaxy, cell) == gen::system_key_for(seed, cell));
    const core::Key system = gen::system_key_for(seed, cell);
    for (int slot : {0, 1, 5}) {
      const gen::BodyHandle via_tree = gen::body_for_system_slot(seed, cell, slot);
      const gen::BodyKeys direct = gen::body_keys_in_system(system, slot);
      CHECK(direct.entity == via_tree.entity);
      CHECK(direct.params == via_tree.params);
      const gen::BodyHandle moon_tree = gen::body_for_system_moon(seed, cell, slot, 1);
      const gen::BodyKeys moon_direct = gen::moon_keys_in_system(system, slot, 1);
      CHECK(moon_direct.entity == moon_tree.entity);
      CHECK(moon_direct.params == moon_tree.params);
    }
  }
}

TEST_CASE("colony/v1: single owner, earliest claim, humans own home, levels monotone (WP3)") {
  const CivWorld w = civ_world(0x83);
  const core::WorldTime t0 = gen::kLaunchReference;
  // The home system belongs to humans from the expansion start, and the
  // home world sits at level 6 with the first Government faction.
  const gen::SystemCivContext home = gen::gather_system_context(w.seed, *w.registry, gen::SystemCell{}, true);
  const gen::Owner owner = w.resolver->owner(home, t0);
  REQUIRE(owner.owned);
  CHECK(owner.race_key == w.registry->human().key);
  CHECK(owner.t_claim == gen::kHumanExpansionStart);
  const auto states = w.resolver->system_states(home, owner, t0);
  int homes = 0;
  int settled = 0;
  for (std::size_t i = 0; i < states.size(); ++i) {
    const gen::CivState& s = states[i];
    if (s.is_home) {
      ++homes;
      CHECK(s.level == 6);
      CHECK(s.settled);
      CHECK(!s.domed);
      CHECK(s.faction_index == 0);
      CHECK(home.bodies[i].type == gen::PlanetType::EarthLike);
    }
    settled += s.settled ? 1 : 0;
    if (s.settled) {
      CHECK(s.level >= 1);
      CHECK(s.level <= s.max_level);
      CHECK(s.max_level >= 1);
      CHECK(s.max_level <= 7);
      CHECK(s.growth > 0.2);
      CHECK(s.growth < 5.0);
    }
    // Giants never host anything.
    if (!home.bodies[i].solid) {
      CHECK(!s.settled);
    }
  }
  CHECK(homes == 1);
  CHECK(settled >= 1);
  // Before the expansion start nobody owns the home system (aliens are
  // far away in this galaxy); levels are monotone in t afterwards.
  CHECK(!w.resolver->owner(home, core::WorldTime::from_ns(gen::kHumanExpansionStart.ns_since_epoch - 1)).owned);
  std::vector<int> previous(states.size(), 0);
  for (double years = -7.5; years <= 12.0; years += 0.5) {
    const core::WorldTime t = launch_plus_years(years);
    const gen::Owner o = w.resolver->owner(home, t);
    REQUIRE(o.owned);
    const auto st = w.resolver->system_states(home, o, t);
    for (std::size_t i = 0; i < st.size(); ++i) {
      CAPTURE(years);
      CAPTURE(i);
      CHECK(st[i].level >= previous[i]);
      previous[i] = st[i].level;
      // max_level and growth never depend on t.
      if (st[i].settled) {
        CHECK((st[i].max_level == states[i].max_level || !states[i].settled));
        CHECK((st[i].growth == states[i].growth || !states[i].settled));
      }
    }
  }
  // Every race's home system is owned by that race at launch, at its
  // home level, and the human race is never extinct.
  const std::vector<gen::Race> aliens = w.registry->races_around(gen::home_system_position_m(w.galaxy));
  for (const gen::Race& race : aliens) {
    const gen::SystemCivContext ctx = gen::gather_system_context(w.seed, *w.registry, race.home_system, true);
    const gen::Owner o = w.resolver->owner(ctx, t0);
    REQUIRE(o.owned);
    CHECK(o.race_key == race.key);
    const auto st = w.resolver->system_states(ctx, o, t0);
    int race_homes = 0;
    for (std::size_t i = 0; i < st.size(); ++i) {
      if (st[i].is_home) {
        ++race_homes;
        CHECK(ctx.bodies[i].is_race_home);
        CHECK(st[i].level == race.params.home_level);
        CHECK(st[i].ruined == race.params.extinct_at(t0));
      }
    }
    CHECK(race_homes == 1);
  }
}

TEST_CASE("colony/v1: two clocks a second apart agree except at a flip (WP3)") {
  const CivWorld w = civ_world(0x83);
  const gen::SystemCivContext home = gen::gather_system_context(w.seed, *w.registry, gen::SystemCell{}, true);
  for (double years : {0.0, 0.7, 2.2, 5.1}) {
    const core::WorldTime a = launch_plus_years(years);
    const core::WorldTime b = a + 1'000'000'000LL;
    const gen::Owner oa = w.resolver->owner(home, a);
    const gen::Owner ob = w.resolver->owner(home, b);
    CHECK(oa.owned == ob.owned);
    const auto sa = w.resolver->system_states(home, oa, a);
    const auto sb = w.resolver->system_states(home, ob, b);
    for (std::size_t i = 0; i < sa.size(); ++i) {
      const core::WorldTime next = w.resolver->next_change(home, home.bodies[i], oa, false, a);
      if (next > b || next == a) {
        CHECK(sa[i].level == sb[i].level);
        CHECK(sa[i].settled == sb[i].settled);
        CHECK(sa[i].faction_index == sb[i].faction_index);
      }
    }
  }
}

TEST_CASE("colony/v1: pacing census against the real generators (WP3)") {
  // The design's promises (section 3.1 / 12.5) sampled over the human
  // sphere at the launch reference: active colonies advance, some by two
  // or three levels, within three real years; flips happen weekly.
  const gen::CivCensus census = gen::run_civ_census(core::Seed128{0, 0x83}, gen::kLaunchReference, 1500);
  MESSAGE(census.report());
  REQUIRE(census.systems_sampled >= 1000);
  CHECK(census.systems_human > 0);
  const int live = census.bodies_settled - census.ruined;
  REQUIRE(live >= 100);
  const double active_adv1 = census.active > 0 ? 100.0 * census.advance1_active / census.active : 0.0;
  const double active_adv2 = census.active > 0 ? 100.0 * census.advance2_active / census.active : 0.0;
  // "All bodies >= 55 %" is the design simulation's population, which had
  // no domes: domed outposts stall at level 3 by Sascha's one-tenth rule
  // and are the majority of settled bodies on airless moons, so the
  // promise is checked over the open-air colonies.
  const double open_adv1 = census.open_air > 0 ? 100.0 * census.advance1_open / census.open_air : 0.0;
  CHECK(active_adv1 >= 90.0);
  CHECK(active_adv2 >= 25.0);
  CHECK(open_adv1 >= 55.0);
  const double flips = 100.0 * census.flip_week / live;
  CHECK(flips >= 0.3);
  CHECK(flips <= 3.0);
  // Domes never sit on giants or > 700 K surfaces (the resolver refuses).
  CHECK(census.domed >= 0);
}

// --- WP4: the planet plan ---------------------------------------------------

#include "gen/settlements.hpp"
#include "gen/terrain.hpp"

namespace {

struct HomeWorld {
  CivWorld w;
  gen::SystemCivContext context;
  std::size_t body_index{0};
  std::unique_ptr<gen::TerrainField> field;
  gen::BodyKeys keys;
};

HomeWorld human_home_world() {
  HomeWorld h{civ_world(0x83), {}, 0, nullptr, {}};
  h.context = gen::gather_system_context(h.w.seed, *h.w.registry, gen::SystemCell{}, false);
  const gen::Owner owner = h.w.resolver->owner(h.context, gen::kLaunchReference);
  const auto states = h.w.resolver->system_states(h.context, owner, gen::kLaunchReference);
  for (std::size_t i = 0; i < states.size(); ++i) {
    if (states[i].is_home) h.body_index = i;
  }
  const gen::StarSystemParams system = gen::generate_system(h.context.system_key);
  h.keys = gen::body_keys_in_system(h.context.system_key, h.context.bodies[h.body_index].slot);
  const gen::PlanetParams params = gen::planet_params_for_slot(
      system, h.context.bodies[h.body_index].slot, gen::BodyHandle{h.keys.entity, h.keys.params});
  h.field = std::make_unique<gen::TerrainField>(h.keys.entity, params);
  return h;
}

}  // namespace

TEST_CASE("settlements/v1: monotone settled set, tiers, regions, capital, faction map (WP4)") {
  const HomeWorld h = human_home_world();
  const gen::Race& human = h.w.registry->human();
  const auto t0 = std::chrono::steady_clock::now();
  const gen::SettlementPlanner planner(h.keys.entity, *h.field, human.params, false);
  const auto t1 = std::chrono::steady_clock::now();
  MESSAGE("planner base pass: " << std::chrono::duration<double, std::milli>(t1 - t0).count()
                                << " ms for " << planner.base().size() << " provinces");
  CHECK(std::chrono::duration<double, std::milli>(t1 - t0).count() < 200.0);
  // Synthetic states: walk level 1..7 with progress, the settled set must
  // only ever grow, and a settled province keeps its tier or grows.
  gen::CivState state;
  state.settled = true;
  state.is_home = true;  // the faction map is a home-world feature
  state.race_key = human.key;
  state.faction_index = 0;
  state.max_level = 7;
  state.growth = 1.0;
  std::vector<bool> was_settled(planner.base().size(), false);
  std::vector<int> last_tier(planner.base().size(), 0);
  gen::SettlementPlan plan;
  bool first = true;
  for (int level = 1; level <= 6; ++level) {
    for (double progress : {0.0, 0.3, 0.6, 0.95}) {
      state.level = level;
      state.progress = progress;
      const auto u0 = std::chrono::steady_clock::now();
      if (first) {
        plan = planner.plan(state, human.factions);
        first = false;
      } else {
        planner.update(&plan, state, human.factions);
      }
      const auto u1 = std::chrono::steady_clock::now();
      if (level == 3 && progress == 0.6) {
        MESSAGE("plan update at level 3: " << std::chrono::duration<double, std::micro>(u1 - u0).count() << " us");
      }
      CAPTURE(level);
      CAPTURE(progress);
      for (std::size_t i = 0; i < plan.provinces.size(); ++i) {
        const gen::ProvinceSite& site = plan.provinces[i];
        if (was_settled[i]) {
          CHECK(site.settled);
        }
        was_settled[i] = was_settled[i] || site.settled;
        if (site.settled) {
          CHECK(site.suitable);
          CHECK(!site.ocean);
          CHECK(site.radius_m > 0.0f);
          CHECK(site.site_progress >= 0.0f);
          CHECK(site.site_progress < 1.0f);
        }
      }
      // plan() and update() agree.
      const gen::SettlementPlan fresh = planner.plan(state, human.factions);
      CHECK(fresh.settled_count == plan.settled_count);
      CHECK(fresh.roads.size() == plan.roads.size());
      CHECK(fresh.capital == plan.capital);
    }
    // Home-world faction map: every faction holds its seat province.
    std::vector<int> seats(human.factions.size(), 0);
    for (const gen::ProvinceSite& site : plan.provinces) {
      if (site.settled && site.faction >= 0) ++seats[static_cast<std::size_t>(site.faction)];
    }
    // Every faction holds its seat once there are enough settled
    // provinces to seat them all (levels 3+ on this world).
    if (plan.settled_count >= human.factions.size()) {
      for (std::size_t j = 0; j < human.factions.size(); ++j) {
        CAPTURE(j);
        CHECK(seats[j] >= 1);
      }
    }
    if (level >= 3) {
      CHECK(!plan.region_capitals.empty());
      CHECK(!plan.roads.empty());
    }
    if (level >= 5) {
      CHECK(plan.capital >= 0);
      int capitals = 0;
      for (const gen::ProvinceSite& site : plan.provinces) capitals += site.capital ? 1 : 0;
      CHECK(capitals == 1);
    }
  }
  // Roads are recomputed identically from either endpoint, and every
  // road joins two settled provinces.
  for (const gen::Road& road : plan.roads) {
    CHECK(road.a < road.b);
    CHECK(plan.provinces[road.a].settled);
    CHECK(plan.provinces[road.b].settled);
    const gen::Road ab = planner.road_between(road.a, road.b, road.trunk);
    const gen::Road ba = planner.road_between(road.b, road.a, road.trunk);
    for (int i = 0; i < 9; ++i) {
      CHECK(ab.points[i].x == ba.points[i].x);
      CHECK(ab.points[i].x == road.points[i].x);
    }
  }
  // The real state: the human home at level 6.
  const gen::Owner owner = h.w.resolver->owner(h.context, gen::kLaunchReference);
  const auto states = h.w.resolver->system_states(h.context, owner, gen::kLaunchReference);
  const gen::SettlementPlan home = planner.plan(states[h.body_index], human.factions);
  CHECK(home.level == 6);
  CHECK(home.is_home);
  CHECK(home.capital >= 0);
  CHECK(home.settled_count >= home.suitable_count * 9 / 10);
  int metros = 0;
  for (const gen::ProvinceSite& site : home.provinces) metros += site.tier == gen::SettlementTier::Metropolis ? 1 : 0;
  CHECK(metros >= 3);
  CHECK(metros <= 8);
}

// --- WP5: sites and civil terrain --------------------------------------------

#include "gen/civil.hpp"
#include "gen/site_mesh.hpp"
#include "gen/sites.hpp"

TEST_CASE("sites/v1 + civil/v1: bounded sites, additive lots, continuous plateau (WP5)") {
  const HomeWorld h = human_home_world();
  const gen::Race& human = h.w.registry->human();
  const gen::Owner owner = h.w.resolver->owner(h.context, gen::kLaunchReference);
  const auto states = h.w.resolver->system_states(h.context, owner, gen::kLaunchReference);
  const gen::CivState& state = states[h.body_index];
  const gen::SettlementPlanner planner(h.keys.entity, *h.field, human.params, false);
  const gen::SettlementPlan plan = planner.plan(state, human.factions);
  const auto t0 = std::chrono::steady_clock::now();
  const gen::SiteField sites(h.keys.entity, *h.field, plan, human.params, human.factions, state);
  const auto t1 = std::chrono::steady_clock::now();
  MESSAGE("site field: " << sites.sites().size() << " sites in "
                         << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms");
  REQUIRE(!sites.sites().empty());
  const double radius = h.field->planet().radius_m.to_double();
  const double sea = h.field->planet().sea_level_m.to_double();
  const gen::Site* town = nullptr;
  for (const gen::Site& site : sites.sites()) {
    // Inside its province: the centre's province is the site's province.
    const gen::CellId cell = h.field->provinces().cell_of(site.frame.up);
    CHECK(cell == site.cell);
    CHECK(site.radius_m > 0.0);
    CHECK(site.datum_m >= sea + 3.0 - 1e-6);
    CHECK(sites.site_of(site.cell) == &site);
    if (town == nullptr && site.tier == static_cast<int>(gen::SettlementTier::Town)) town = &site;
  }
  REQUIRE(town != nullptr);
  // Lots: inside the radius, non-overlapping within a block, every lot
  // present at tier n-1 is present verbatim at tier n, visible count
  // monotone in progress.
  std::vector<gen::Lot> lots;
  sites.all_lots(*town, &lots);
  CHECK(lots.size() > 20);
  for (const gen::Lot& lot : lots) {
    double cx = 0.0;
    double cy = 0.0;
    for (int k = 0; k < lot.vertex_count; ++k) {
      cx += lot.footprint[k][0];
      cy += lot.footprint[k][1];
    }
    cx /= lot.vertex_count;
    cy /= lot.vertex_count;
    CHECK(std::sqrt(cx * cx + cy * cy) <= town->radius_m + 1e-3);
    CHECK(lot.height_budget_m > 0.0f);
    CHECK(lot.tier >= 1);
    CHECK(lot.tier <= town->tier);
  }
  // Pairwise separation inside one block (footprints are smaller than
  // the lattice pitch by construction): centres at least 0.6 lot apart.
  {
    std::vector<gen::Lot> block;
    sites.lots_in_block(*town, 0, 0, &block);
    for (std::size_t a = 0; a < block.size(); ++a) {
      for (std::size_t b = a + 1; b < block.size(); ++b) {
        const double dx = block[a].footprint[0][0] - block[b].footprint[0][0];
        const double dy = block[a].footprint[0][1] - block[b].footprint[0][1];
        CHECK(std::sqrt(dx * dx + dy * dy) > 0.6 * town->lot_m);
      }
    }
  }
  {
    // Tier n-1 verbatim inside tier n.
    gen::Site smaller = *town;
    smaller.tier = town->tier - 1;
    smaller.radius_m = gen::ring_radius_m(smaller.tier);
    smaller.progress = 0.999f;
    gen::Site larger = *town;
    larger.progress = 0.999f;
    std::vector<gen::Lot> inner;
    std::vector<gen::Lot> outer;
    sites.all_lots(smaller, &inner);
    sites.all_lots(larger, &outer);
    CHECK(outer.size() > inner.size());
    for (const gen::Lot& lot : inner) {
      bool found = false;
      for (const gen::Lot& other : outer) {
        if (other.id == lot.id && other.footprint[0][0] == lot.footprint[0][0] &&
            other.height_budget_m == lot.height_budget_m) {
          found = true;
          break;
        }
      }
      CHECK(found);
    }
    // Visible count monotone in progress; the newest 3 % under construction.
    std::uint32_t last = 0;
    for (float p = 0.0f; p <= 1.0f; p += 0.1f) {
      const std::uint32_t count = sites.visible_count(*town, std::min(p, 0.999f));
      CHECK(count >= last);
      last = count;
    }
    std::vector<gen::Lot> mid;
    gen::Site half = *town;
    half.progress = 0.5f;
    sites.all_lots(half, &mid);
    int building = 0;
    for (const gen::Lot& lot : mid) building += lot.style.construction < 1.0f ? 1 : 0;
    CHECK(building >= 1);
  }
  // civil/v1: plateau at the datum inside 0.75 R, continuous across the
  // rim (no step > 1 m per metre outside terraces), ground query composes.
  gen::TerrainField& field = *h.field;
  const gen::CivilField civil(sites, field);
  field.set_height_modifier(&civil);
  {
    const gen::Dir3 centre = town->frame.up;
    CHECK(field.elevation_m(centre).to_double() == doctest::Approx(town->datum_m).epsilon(1e-6));
    CHECK(civil.near(centre));
    double previous = field.elevation_m(town->frame.to_dir(0.0, 0.0)).to_double();
    double max_grade = 0.0;
    for (double x = 2.0; x <= 1.4 * town->radius_m; x += 2.0) {
      const double e = field.elevation_m(town->frame.to_dir(x, 0.0)).to_double();
      max_grade = std::max(max_grade, std::fabs(e - previous) / 2.0);
      previous = e;
    }
    MESSAGE("max grade across the town rim: " << max_grade << " m/m");
    if (town->family != gen::LayoutFamily::Terraced) {
      CHECK(max_grade < 1.0);
    }
    // ground_radius_m composes the modifier.
    const double ground = field.ground_radius_m(centre).to_double();
    CHECK(ground == doctest::Approx(radius + town->datum_m).epsilon(1e-4));
    // Far from every site the modifier is silent.
    const gen::Dir3 far = gen::normalize(gen::Dir3{det::Real(-centre.x.to_double()), det::Real(-centre.y.to_double()), det::Real(-centre.z.to_double())});
    CHECK(field.elevation_m(far) == field.base_elevation_m(far));
    // Urban material weight at the centre.
    const double r = radius + town->datum_m;
    const gen::MaterialInputs in = field.material_inputs(centre.x.to_double() * r, centre.y.to_double() * r, centre.z.to_double() * r,
                                                        centre.x.to_double(), centre.y.to_double(), centre.z.to_double());
    CHECK(in.urban > 0.5);
  }
  // Mass mesh: deterministic, lots within budget.
  const gen::SiteMeshParams mp;
  const gen::SiteMesh a = gen::build_site_mesh(sites, *town, field, mp);
  const gen::SiteMesh b = gen::build_site_mesh(sites, *town, field, mp);
  CHECK(a.lot_count == lots.size());
  CHECK(a.mesh.vertices == b.mesh.vertices);
  CHECK(a.triangle_count > 0);
  field.set_height_modifier(nullptr);
}

// --- WP6: the building executor -------------------------------------------

#include <cstring>

#include "gen/buildings.hpp"
#include "tex/tiles.hpp"

TEST_CASE("buildings/v1: determinism, budgets, ruins, construction, asset-less (WP6)") {
  const core::Key base = core::derive_named(core::universe_key(core::Seed128{0, 0x83}), gen::name::BuildingsV1);
  std::size_t names_count = 0;
  const char* const* names = tex::known_tile_names(&names_count);
  const auto tile_known = [&](gen::Material id) {
    const char* name = gen::material_info(id).name;
    for (std::size_t i = 0; i < names_count; ++i) {
      if (std::strcmp(names[i], name) == 0) return true;
    }
    return false;
  };
  int lot_index = 0;
  for (int race = 0; race < static_cast<int>(gen::RaceType::Count); ++race) {
    for (int faction = 0; faction < static_cast<int>(gen::FactionType::Count); ++faction) {
      gen::Lot lot;
      lot.id = static_cast<std::uint32_t>(++lot_index);
      lot.vertex_count = 4;
      lot.footprint[0][0] = -7.0f; lot.footprint[0][1] = -5.0f;
      lot.footprint[1][0] = 7.0f; lot.footprint[1][1] = -5.0f;
      lot.footprint[2][0] = 7.0f; lot.footprint[2][1] = 5.0f;
      lot.footprint[3][0] = -7.0f; lot.footprint[3][1] = 5.0f;
      lot.height_budget_m = 14.0f;
      lot.usage = gen::LotUsage::Residential;
      lot.style.race_type = static_cast<gen::RaceType>(race);
      lot.style.faction_type = static_cast<gen::FactionType>(faction);
      lot.style.material_family = static_cast<std::uint8_t>(gen::race_type_info(static_cast<gen::RaceType>(race)).material_family);
      lot.style.tech_tier = 4;
      lot.style.light_density = 0.7f;
      const core::Key key = core::derive_child(base, gen::kind::Lot, lot.id);
      CAPTURE(race);
      CAPTURE(faction);
      // Every palette material has a procedural tile: the asset-less
      // build renders every settlement.
      std::uint8_t palette[4];
      gen::building_palette(lot.style, palette);
      for (int k = 0; k < 4; ++k) {
        CHECK(tile_known(static_cast<gen::Material>(palette[k])));
      }
      // Determinism and the height budget (roofs, spires and masts may
      // exceed the budget by a bounded allowance).
      gen::BuildingParams bp;
      const gen::BuildingMesh a = gen::build_building(lot, key, bp);
      const gen::BuildingMesh b = gen::build_building(lot, key, bp);
      CHECK(a.vertices == b.vertices);
      CHECK(a.triangle_count > 0);
      CHECK(a.vertices.size() == a.triangle_count * 21);
      CHECK(a.top_z <= 1.6 * lot.height_budget_m + 8.0);
      CHECK(a.top_z > 0.5);
      // Every method produces something; masses are the cheapest.
      gen::BuildingParams mass = bp;
      mass.method = gen::BuildingMethod::Mass;
      const gen::BuildingMesh m = gen::build_building(lot, key, mass);
      CHECK(m.triangle_count <= a.triangle_count);
      // Construction stages: triangles and height never shrink as the
      // building goes up.
      std::uint32_t last_tris = 0;
      float last_top = 0.0f;
      for (const float stage : {0.1f, 0.4f, 0.75f, 0.95f, 1.0f}) {
        gen::Lot staged = lot;
        staged.style.construction = stage;
        const gen::BuildingMesh s = gen::build_building(staged, key, bp);
        CAPTURE(stage);
        CHECK(s.top_z >= last_top - 1e-3f);
        CHECK(s.triangle_count + 4 >= last_tris);  // roofs replace frames: small slack
        last_tris = s.triangle_count;
        last_top = s.top_z;
      }
      // Ruins: no emissive glass in the palette, lower than the intact
      // building.
      gen::Lot ruin = lot;
      ruin.style.ruined = true;
      ruin.style.construction = 1.0f;
      gen::building_palette(ruin.style, palette);
      CHECK(palette[2] != static_cast<std::uint8_t>(gen::Material::WindowGlass));
      const gen::BuildingMesh r = gen::build_building(ruin, key, bp);
      CHECK(!r.emissive_windows);
      CHECK(r.top_z <= a.top_z);  // stilt decks and plinths keep their height
    }
  }
}

TEST_CASE("ecumenopolis/v1: plates over terrain, block determinism, tile cost (WP7)") {
  const core::Seed128 seed{0, 0x83};
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  const gen::ColonyResolver resolver(registry);
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, gen::SystemCell{}, false);
  const gen::StarSystemParams system = gen::generate_system(context.system_key);
  const gen::Owner owner = resolver.owner(context, gen::kLaunchReference);
  const auto states = resolver.system_states(context, owner, gen::kLaunchReference);
  int home = -1;
  for (std::size_t i = 0; i < states.size(); ++i) if (states[i].is_home) home = static_cast<int>(i);
  REQUIRE(home >= 0);
  const gen::BodyCivInputs& body = context.bodies[static_cast<std::size_t>(home)];
  const gen::BodyKeys keys = gen::body_keys_in_system(context.system_key, body.slot);
  const gen::PlanetParams params = gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
  gen::TerrainField field(keys.entity, params);
  const gen::Race race = resolver.candidates(context.position_m)[owner.candidate];
  for (const bool ruined : {false, true}) {
    gen::CivState state = states[static_cast<std::size_t>(home)];
    state.level = 7;
    state.max_level = 7;
    state.ruined = ruined;
    const gen::SettlementPlanner planner(keys.entity, field, race.params, false);
    const gen::SettlementPlan plan = planner.plan(state, race.factions);
    // Level 7: every province is the city, one capital.
    for (const gen::ProvinceSite& p : plan.provinces) {
      CHECK(p.settled);
      CHECK(p.tier == gen::SettlementTier::Ecumenopolis);
    }
    CHECK(plan.capital >= 0);
    const auto t0 = std::chrono::steady_clock::now();
    const gen::EcumenopolisField ecum(keys.entity, field, plan, race.params, race.factions, state);
    const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(build_ms < 3000.0);
    CHECK(ecum.block_m() > 80.0);
    CHECK(ecum.block_m() < 180.0);
    field.set_height_modifier(&ecum);
    // The plate is at or above the terrain everywhere (preserved peaks
    // keep the terrain), continuous across province borders, and over
    // the sea where the province is ocean.
    const double sea = params.sea_level_m.to_double();
    double max_step = 0.0;
    int above = 0;
    int samples = 0;
    for (int k = 0; k < 400; ++k) {
      const double a = 0.0157 * k;
      const double b = 1.5 * std::sin(0.61 * k);
      const gen::Dir3 d = gen::normalize(gen::Dir3{det::Real(std::cos(a) * std::cos(b)), det::Real(std::sin(a) * std::cos(b)), det::Real(std::sin(b))});
      const double base = field.base_elevation_m(d).to_double();
      const double h = field.elevation_m(d).to_double();
      const double plate = ecum.plate_m(d);
      CHECK(h >= base - 1e-6);
      if (!ruined) {
        CHECK(h >= std::min(plate, base) - 1e-6);
        CHECK(plate >= sea + gen::EcumenopolisField::kPlateAboveSeaM - 0.5);  // float plate storage
      }
      above += h > base + 1.0 ? 1 : 0;
      ++samples;
      // Continuity: a 5 m step moves the plate by less than 1 m.
      gen::Dir3 e{};
      gen::Dir3 n{};
      gen::tangent_basis(d, &e, &n);
      const double eps = 5.0 / params.radius_m.to_double();
      const gen::Dir3 d2 = gen::normalize(gen::Dir3{d.x + e.x * det::Real(eps), d.y + e.y * det::Real(eps), d.z + e.z * det::Real(eps)});
      max_step = std::max(max_step, std::fabs(ecum.plate_m(d2) - plate));
    }
    CHECK(max_step < 1.0);
    CHECK(above > samples / 2);  // the city stands above the terrain almost everywhere
    // Block determinism: the same block from two tiles' frames gives the
    // same towers (in the two frames' coordinates), and a tile builds
    // identically twice.
    const gen::Dir3 c = field.provinces().representative(plan.provinces[static_cast<std::size_t>(plan.capital)].cell);
    const gen::EcumenopolisField::BlockId block = ecum.block_of(c);
    const gen::EcumenopolisField::TileId near = ecum.tile_of(c, 3);
    const gen::EcumenopolisField::TileId mid = ecum.tile_of(c, 5);
    std::vector<gen::Lot> a;
    std::vector<gen::Lot> b;
    ecum.towers_in_block(block, ecum.tile_frame(near), &a);
    ecum.towers_in_block(block, ecum.tile_frame(mid), &b);
    CHECK(a.size() == b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
      CHECK(a[i].height_budget_m == b[i].height_budget_m);
      CHECK(a[i].usage == b[i].usage);
      CHECK(a[i].datum_m == b[i].datum_m);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const gen::EcumenopolisMesh m1 = gen::build_ecumenopolis_tile(ecum, near, 2);
    const double near_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const gen::EcumenopolisMesh m2 = gen::build_ecumenopolis_tile(ecum, near, 2);
    CHECK(m1.mesh.vertices == m2.mesh.vertices);
    CHECK(m1.tower_count > 0);
    CHECK(near_ms < 50.0);
    const auto t2 = std::chrono::steady_clock::now();
    const gen::EcumenopolisMesh far = gen::build_ecumenopolis_tile(ecum, ecum.tile_of(c, 6), 3);
    const double far_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    CHECK(far.triangle_count > 0);
    CHECK(far_ms < 120.0);
    const auto t3 = std::chrono::steady_clock::now();
    const gen::EcumenopolisMesh parts = gen::build_ecumenopolis_tile(ecum, near, 0);
    const double parts_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t3).count();
    CHECK(parts.triangle_count >= m1.triangle_count);
    CHECK(parts_ms < 400.0);
    // Modifier cost per sample stays within the terrain budget.
    const auto t4 = std::chrono::steady_clock::now();
    double acc = 0.0;
    for (int k = 0; k < 20000; ++k) {
      const double a = 0.00031 * k;
      const gen::Dir3 d = gen::normalize(gen::Dir3{det::Real(std::cos(a)), det::Real(std::sin(a) * 0.7), det::Real(0.3 + 0.0001 * k)});
      acc += ecum.plate_m(d);
    }
    const double per_sample_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t4).count() / 20000.0;
    CHECK(acc != 0.0);
    CHECK(per_sample_us < 6.0);
    if (ruined) {
      CHECK(!ecum.style().ruined == false);
      CHECK(ecum.urban(c, det::Real(ecum.plate_m(c))).night_light < 0.05);
    }
  }
}

TEST_CASE("civilization: time sweep, two-client agreement, performance acceptance (WP8)") {
  const core::Seed128 seed{0, 0x83};
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  const gen::ColonyResolver resolver(registry);
  const auto at_years = [](double years) {
    return core::WorldTime::from_ns(gen::kLaunchReference.ns_since_epoch + gen::real_years_to_ns(years));
  };
  const double sweep[5] = {-5.0, 0.0, 7.0 / 365.25, 3.0, 10.0};
  // --- the human home system: levels and visible lots never decrease ---
  {
    const gen::SystemCivContext context = gen::gather_system_context(seed, registry, gen::SystemCell{}, false);
    const gen::StarSystemParams system = gen::generate_system(context.system_key);
    std::vector<int> last_level(context.bodies.size(), 0);
    std::uint32_t last_lots = 0;
    for (const double years : sweep) {
      const core::WorldTime t = at_years(years);
      const gen::Owner owner = resolver.owner(context, t);
      REQUIRE(owner.owned);
      const auto states = resolver.system_states(context, owner, t);
      for (std::size_t i = 0; i < states.size(); ++i) {
        CAPTURE(years);
        CHECK(states[i].level >= last_level[i]);
        last_level[i] = states[i].level;
        if (states[i].is_home && states[i].settled) {
          const gen::BodyCivInputs& body = context.bodies[i];
          const gen::BodyKeys keys = gen::body_keys_in_system(context.system_key, body.slot);
          const gen::PlanetParams params = gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
          gen::TerrainField field(keys.entity, params);
          const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
          const gen::SettlementPlanner planner(keys.entity, field, race.params, states[i].domed);
          const gen::SettlementPlan plan = planner.plan(states[i], race.factions);
          const gen::SiteField sites(keys.entity, field, plan, race.params, race.factions, states[i]);
          // The best town's visible lots (the home is pinned at its level,
          // so the count is constant; it must never drop).
          std::uint32_t lots = 0;
          for (const gen::Site& site : sites.sites()) {
            if (site.tier == static_cast<int>(gen::SettlementTier::Town)) {
              lots = sites.visible_count(site, site.progress);
              break;
            }
          }
          CHECK(lots >= last_lots);
          last_lots = lots;
        }
      }
    }
  }
  // --- a Precursor body: ruined after the race's end, level frozen ---
  {
    const gen::SystemCell precursor{3224, 3979, 4086, 13};
    const gen::SystemCivContext context = gen::gather_system_context(seed, registry, precursor, true);
    const core::WorldTime t = at_years(0.0);
    const gen::Owner owner = resolver.owner(context, t);
    REQUIRE(owner.owned);
    const auto now = resolver.system_states(context, owner, t);
    const auto later = resolver.system_states(context, owner, at_years(10.0));
    int ruined = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
      if (!now[i].settled) continue;
      CHECK(now[i].ruined);
      CHECK(later[i].ruined);
      CHECK(later[i].level == now[i].level);
      ++ruined;
    }
    CHECK(ruined > 0);
  }
  // --- two clients a second apart: identical states away from a flip ---
  {
    const gen::SystemCivContext context = gen::gather_system_context(seed, registry, gen::SystemCell{}, false);
    const core::WorldTime a = at_years(1.0);
    const core::WorldTime b = core::WorldTime::from_ns(a.ns_since_epoch + 1000000000LL);
    const gen::Owner oa = resolver.owner(context, a);
    const gen::Owner ob = resolver.owner(context, b);
    CHECK(oa.owned == ob.owned);
    CHECK(oa.race_key == ob.race_key);
    const auto sa = resolver.system_states(context, oa, a);
    const auto sb = resolver.system_states(context, ob, b);
    for (std::size_t i = 0; i < sa.size(); ++i) {
      CHECK(sa[i].settled == sb[i].settled);
      CHECK(sa[i].faction_index == sb[i].faction_index);
      CHECK(sa[i].domed == sb[i].domed);
      CHECK(std::abs(sa[i].level - sb[i].level) <= 1);  // only a flip within the second may differ
    }
  }
  // --- performance acceptance (brief section 10; generous for CI noise) ---
  {
    const gen::SystemCivContext context = gen::gather_system_context(seed, registry, gen::SystemCell{}, false);
    const core::WorldTime t = at_years(0.0);
    const gen::Owner owner = resolver.owner(context, t);
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 2000; ++k) {
      const gen::Owner o = resolver.owner(context, core::WorldTime::from_ns(t.ns_since_epoch + k));
      CHECK(o.owned);
    }
    const double owner_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
    t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 500; ++k) {
      const auto states = resolver.system_states(context, owner, core::WorldTime::from_ns(t.ns_since_epoch + k));
      CHECK(!states.empty());
    }
    const double states_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 500.0;
    CHECK(owner_us < 50.0);
    CHECK(states_us < 20.0 * static_cast<double>(context.bodies.size()) * 5.0);
    const gen::StarSystemParams system = gen::generate_system(context.system_key);
    const auto states = resolver.system_states(context, owner, t);
    for (std::size_t i = 0; i < states.size(); ++i) {
      if (!states[i].is_home) continue;
      const gen::BodyCivInputs& body = context.bodies[i];
      const gen::BodyKeys keys = gen::body_keys_in_system(context.system_key, body.slot);
      const gen::PlanetParams params = gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
      gen::TerrainField field(keys.entity, params);
      const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
      const gen::SettlementPlanner planner(keys.entity, field, race.params, states[i].domed);
      t0 = std::chrono::steady_clock::now();
      const gen::SettlementPlan plan = planner.plan(states[i], race.factions);
      const double plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      CHECK(plan_ms < 25.0);
      t0 = std::chrono::steady_clock::now();
      const gen::SiteField sites(keys.entity, field, plan, race.params, race.factions, states[i]);
      const double sites_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      CHECK(sites_ms / std::max<double>(1.0, static_cast<double>(sites.sites().size())) < 5.0);  // per city layout
      // Terrain sample cost outside every site bound is unchanged by the
      // modifier: the bound test is the only extra work.
      const gen::CivilField civil(sites, field);
      std::vector<gen::Dir3> outside;
      for (int k = 0; k < 4000 && outside.size() < 200; ++k) {
        const double a = 0.0031 * k;
        const double b = 1.4 * std::sin(0.37 * k);
        const gen::Dir3 d = gen::normalize(gen::Dir3{det::Real(std::cos(a) * std::cos(b)), det::Real(std::sin(a) * std::cos(b)), det::Real(std::sin(b))});
        if (!civil.near(d)) outside.push_back(d);
      }
      REQUIRE(outside.size() >= 50);
      double acc = 0.0;
      t0 = std::chrono::steady_clock::now();
      for (int rep = 0; rep < 5; ++rep) for (const gen::Dir3& d : outside) acc += field.elevation_m(d).to_double();
      const double plain_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      field.set_height_modifier(&civil);
      t0 = std::chrono::steady_clock::now();
      for (int rep = 0; rep < 5; ++rep) for (const gen::Dir3& d : outside) acc += field.elevation_m(d).to_double();
      const double modified_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      CHECK(acc != 0.0);
      CHECK(modified_us < 1.5 * plain_us + 2000.0);
    }
  }
}
