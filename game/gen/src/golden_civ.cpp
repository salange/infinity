#include "gen/golden.hpp"

#include <array>
#include <bit>
#include <vector>

#include "core/golden.hpp"
#include "gen/civilization.hpp"
#include "gen/colony.hpp"
#include "gen/human.hpp"
#include "gen/settlements.hpp"
#include "gen/sites.hpp"
#include "gen/civil.hpp"
#include "gen/terrain.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

namespace {

void feed_double(core::GoldenHash& hash, double value) {
  hash.feed(std::bit_cast<std::uint64_t>(value));
}

void feed_key(core::GoldenHash& hash, const core::Key& key) {
  hash.feed(key.k0);
  hash.feed(key.k1);
}

void append_hex(std::string* report, std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    *report += kDigits[(value >> (i * 4)) & 0xFU];
  }
}

// WP1: civilization/v1 + the race block around the home system.
std::uint64_t hash_races_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  hash.feed(civ.race_count);
  hash.feed(civ.teeming ? 1U : 0U);
  hash.feed(static_cast<std::uint64_t>(civ.l_civ));
  feed_double(hash, civ.cell_width_ly);
  const RaceRegistry registry(galaxy_key, galaxy, civ);
  const auto& races = registry.races_around(home_system_position_m(galaxy));
  hash.feed(races.size());
  for (const Race& race : races) {
    feed_key(hash, race.key);
    hash.feed(static_cast<std::uint64_t>(race.cell.x));
    hash.feed(static_cast<std::uint64_t>(race.cell.y));
    hash.feed(static_cast<std::uint64_t>(race.cell.z));
    hash.feed(static_cast<std::uint64_t>(race.home_system.x));
    hash.feed(static_cast<std::uint64_t>(race.home_system.y));
    hash.feed(static_cast<std::uint64_t>(race.home_system.z));
    hash.feed(static_cast<std::uint64_t>(race.home_system.level));
    const RaceParams& p = race.params;
    hash.feed(static_cast<std::uint64_t>(p.type));
    hash.feed(p.variant);
    hash.feed(static_cast<std::uint64_t>(p.tech_tier));
    hash.feed(static_cast<std::uint64_t>(p.peak_level));
    hash.feed(static_cast<std::uint64_t>(p.home_level));
    feed_double(hash, p.dome_affinity);
    hash.feed(static_cast<std::uint64_t>(p.t_0.ns_since_epoch));
    feed_double(hash, p.speed_ly_per_year);
    feed_double(hash, p.reproduction);
    feed_double(hash, p.settle_prob);
    feed_double(hash, p.r_max_ly);
    feed_double(hash, p.falloff_ly);
    feed_double(hash, p.anisotropy);
    hash.feed(p.extinct_ever ? 1U : 0U);
    hash.feed(static_cast<std::uint64_t>(p.extinct_ever ? p.t_end.ns_since_epoch : 0));
    hash.feed(race.factions.size());
    for (const FactionParams& f : race.factions) {
      hash.feed(static_cast<std::uint64_t>(f.type));
      hash.feed(static_cast<std::uint64_t>(f.t_start.ns_since_epoch));
      feed_double(hash, f.speed_mul);
      feed_double(hash, f.reproduction_mul);
      feed_double(hash, f.settle_mul);
      hash.feed(f.centres.size());
    }
  }
  return hash.value();
}

// WP2: the human race of the home galaxy and the enclave/gate table of
// the home cluster.
std::uint64_t hash_human_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const Race human = human_race(home_galaxy_key(seed), galaxy);
  feed_key(hash, human.key);
  feed_double(hash, human.params.speed_ly_per_year);
  feed_double(hash, human.params.falloff_ly);
  feed_double(hash, human.params.r_max_ly);
  hash.feed(static_cast<std::uint64_t>(human.params.t_0.ns_since_epoch));
  hash.feed(human.factions.size());
  for (const FactionParams& f : human.factions) {
    hash.feed(static_cast<std::uint64_t>(f.type));
    hash.feed(static_cast<std::uint64_t>(f.t_start.ns_since_epoch));
    feed_double(hash, f.speed_mul);
    feed_double(hash, f.reproduction_mul);
    feed_double(hash, f.settle_mul);
    hash.feed(f.centres.size());
    for (const FactionCentre& c : f.centres) {
      feed_double(hash, c.position_m.x.to_double());
      feed_double(hash, c.position_m.y.to_double());
      feed_double(hash, c.weight);
    }
  }
  for (const WormholeGate& gate : home_galaxy_gates(seed)) {
    hash.feed(gate.partner_galaxy);
    hash.feed(gate.partner_enclave);
    feed_double(hash, gate.position_m.x.to_double());
    feed_double(hash, gate.position_m.y.to_double());
    feed_double(hash, gate.partner_position_m.x.to_double());
  }
  return hash.value();
}

// WP3: owners and body states of the home system and of the nearest
// occupied systems at fixed ManualClock offsets from the launch
// reference (before and after the first flips).
std::uint64_t hash_state_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(human_race(galaxy_key, galaxy));
  const ColonyResolver resolver(registry);
  std::vector<SystemCell> cells;
  cells.push_back(SystemCell{});
  {
    std::vector<GalaxyOctree::CellId> near;
    registry.octree().systems_in_ball(home_system_position_m(galaxy), det::Real(40.0 * kLightYearM),
                                      6, &near);
    for (const auto& c : near) {
      cells.push_back(SystemCell{c.x, c.y, c.z, c.level});
    }
  }
  static constexpr double kOffsetsYears[] = {-5.0, 0.0, 0.0192, 3.0, 10.0};
  for (const SystemCell& cell : cells) {
    const SystemCivContext context = gather_system_context(seed, registry, cell, true);
    for (const double years : kOffsetsYears) {
      const core::WorldTime t =
          core::WorldTime::from_ns(kLaunchReference.ns_since_epoch + real_years_to_ns(years));
      const Owner owner = resolver.owner(context, t);
      hash.feed(owner.owned ? 1U : 0U);
      if (owner.owned) {
        feed_key(hash, owner.race_key);
        hash.feed(static_cast<std::uint64_t>(owner.t_claim.ns_since_epoch));
      }
      for (const CivState& s : resolver.system_states(context, owner, t)) {
        hash.feed(s.settled ? 1U : 0U);
        if (!s.settled) continue;
        hash.feed(static_cast<std::uint64_t>(s.level));
        hash.feed(static_cast<std::uint64_t>(s.max_level));
        hash.feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(s.faction_index)));
        hash.feed(s.ruined ? 1U : 0U);
        hash.feed(s.domed ? 1U : 0U);
        hash.feed(static_cast<std::uint64_t>(s.settled_at.ns_since_epoch));
        feed_double(hash, s.growth);
      }
    }
  }
  return hash.value();
}

// WP4: the human home world's plan at two times (settled set, tiers,
// regions, factions, roads).
std::uint64_t hash_plan_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(human_race(galaxy_key, galaxy));
  const ColonyResolver resolver(registry);
  const SystemCivContext context = gather_system_context(seed, registry, SystemCell{}, false);
  const StarSystemParams system = generate_system(context.system_key);
  for (const double years : {0.0, 3.0}) {
    const core::WorldTime t =
        core::WorldTime::from_ns(kLaunchReference.ns_since_epoch + real_years_to_ns(years));
    const Owner owner = resolver.owner(context, t);
    const auto states = resolver.system_states(context, owner, t);
    for (std::size_t i = 0; i < states.size(); ++i) {
      if (!states[i].is_home) continue;
      const BodyCivInputs& body = context.bodies[i];
      const BodyKeys keys = body_keys_in_system(context.system_key, body.slot);
      const PlanetParams params =
          planet_params_for_slot(system, body.slot, BodyHandle{keys.entity, keys.params});
      const TerrainField field(keys.entity, params);
      const Race& race = resolver.candidates(context.position_m)[owner.candidate];
      const SettlementPlanner planner(keys.entity, field, race.params, states[i].domed);
      const SettlementPlan plan = planner.plan(states[i], race.factions);
      hash.feed(plan.suitable_count);
      hash.feed(plan.settled_count);
      hash.feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(plan.capital)));
      hash.feed(plan.region_capitals.size());
      hash.feed(plan.roads.size());
      for (const ProvinceSite& site : plan.provinces) {
        if (!site.settled) continue;
        hash.feed(site.index);
        hash.feed(static_cast<std::uint64_t>(site.tier));
        hash.feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(site.region)));
        hash.feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(site.faction)));
        hash.feed(std::bit_cast<std::uint32_t>(site.score));
      }
      for (const Road& road : plan.roads) {
        hash.feed(road.a);
        hash.feed(road.b);
        feed_double(hash, road.points[4].x.to_double());
      }
    }
  }
  return hash.value();
}

// WP5: the human home world's sites — centres, datums, families, the
// lots of the best town at two progress values, and the civil modifier
// at fixed directions.
std::uint64_t hash_sites_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(human_race(galaxy_key, galaxy));
  const ColonyResolver resolver(registry);
  const SystemCivContext context = gather_system_context(seed, registry, SystemCell{}, false);
  const StarSystemParams system = generate_system(context.system_key);
  const core::WorldTime t = kLaunchReference;
  const Owner owner = resolver.owner(context, t);
  const auto states = resolver.system_states(context, owner, t);
  for (std::size_t i = 0; i < states.size(); ++i) {
    if (!states[i].is_home) continue;
    const BodyCivInputs& body = context.bodies[i];
    const BodyKeys keys = body_keys_in_system(context.system_key, body.slot);
    const PlanetParams params = planet_params_for_slot(system, body.slot, BodyHandle{keys.entity, keys.params});
    TerrainField field(keys.entity, params);
    const Race& race = resolver.candidates(context.position_m)[owner.candidate];
    const SettlementPlanner planner(keys.entity, field, race.params, states[i].domed);
    const SettlementPlan plan = planner.plan(states[i], race.factions);
    const SiteField sites(keys.entity, field, plan, race.params, race.factions, states[i]);
    hash.feed(sites.sites().size());
    const Site* town = nullptr;
    for (const Site& site : sites.sites()) {
      feed_double(hash, site.frame.up.x.to_double());
      feed_double(hash, site.frame.up.z.to_double());
      feed_double(hash, site.datum_m);
      hash.feed(static_cast<std::uint64_t>(site.family));
      hash.feed(static_cast<std::uint64_t>(site.tier));
      hash.feed(site.arterials.size());
      if (town == nullptr && site.tier == static_cast<int>(SettlementTier::Town)) town = &site;
    }
    if (town != nullptr) {
      for (const float progress : {0.2f, 0.9f}) {
        Site probe = *town;
        probe.progress = progress;
        std::vector<Lot> lots;
        sites.all_lots(probe, &lots);
        hash.feed(lots.size());
        for (const Lot& lot : lots) {
          hash.feed(lot.id);
          hash.feed(lot.order);
          hash.feed(std::bit_cast<std::uint32_t>(lot.footprint[0][0]));
          hash.feed(std::bit_cast<std::uint32_t>(lot.height_budget_m));
          hash.feed(static_cast<std::uint64_t>(lot.usage));
        }
      }
    }
    const CivilField civil(sites, field);
    field.set_height_modifier(&civil);
    for (const Site& site : sites.sites()) {
      // Centre, half radius, rim, and just outside.
      for (const double f : {0.0, 0.5, 0.95, 1.2}) {
        const Dir3 d = site.frame.to_dir(f * site.radius_m, 0.3 * f * site.radius_m);
        feed_double(hash, field.elevation_m(d).to_double());
      }
    }
  }
  return hash.value();
}

}  // namespace

std::string hash_civ_report() {
  static constexpr std::array<core::Seed128, 4> kSeeds = {
      core::Seed128{0, 1},
      core::Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},
      core::Seed128{0, 0xDEADBEEFULL},
      core::Seed128{0, 0x83},
  };
  std::string report = "hash-civ v1\n";
  for (const core::Seed128& seed : kSeeds) {
    report += "races seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_races_script(seed));
    report += "\n";
  }
  for (const core::Seed128& seed : kSeeds) {
    report += "human seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_human_script(seed));
    report += "\n";
  }
  for (const core::Seed128& seed : kSeeds) {
    report += "state seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_state_script(seed));
    report += "\n";
  }
  for (const core::Seed128& seed : kSeeds) {
    report += "plan seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_plan_script(seed));
    report += "\n";
  }
  for (const core::Seed128& seed : kSeeds) {
    report += "sites seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_sites_script(seed));
    report += "\n";
  }
  return report;
}

}  // namespace inf::gen
