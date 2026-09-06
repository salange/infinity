#include "gen/civ_census.hpp"

#include <cmath>
#include <cstdio>

#include "core/det/mix.hpp"
#include "gen/human.hpp"

namespace inf::gen {

namespace {

double u01(std::uint64_t z) { return static_cast<double>(z >> 11U) * 0x1.0p-53; }

}  // namespace

CivCensus run_civ_census(const core::Seed128& seed, core::WorldTime t, int max_systems,
                         double radius_ly) {
  CivCensus census;
  census.t = t;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  RaceRegistry registry(galaxy_key, galaxy, civ);
  const Race human = human_race(galaxy_key, galaxy);
  registry.set_human(human);
  const ColonyResolver resolver(registry);
  const Dir3 home = home_system_position_m(galaxy);
  const double r_gal_ly = galaxy.diameter_ly.to_double() * 0.5;
  const double r_ly = radius_ly > 0.0 ? radius_ly : r_gal_ly;
  const double height_m = galaxy.thin_scale_height_ly.to_double() * kLightYearM * 3.0;
  census.front_ly = human.params.speed_ly_per_year *
                    ns_to_real_years(t.ns_since_epoch - human.params.t_0.ns_since_epoch);
  const std::int64_t week_ns = static_cast<std::int64_t>(kRealWeekS * 1e9);
  const std::int64_t three_years_ns = real_years_to_ns(3.0);
  // Deterministic point set: a fixed-seed mixer, never the platform RNG.
  std::uint64_t state = 0x5EEDC1F5C0DEULL ^ seed.lo ^ (seed.hi * 0x9E3779B97F4A7C15ULL);
  const auto next = [&state] {
    state = det::mix64(state + 0x9E3779B97F4A7C15ULL);
    return state;
  };
  const GalaxyOctree& octree = registry.octree();
  int tries = 0;
  (void)height_m;
  while (census.systems_sampled < max_systems && tries < max_systems * 40) {
    ++tries;
    // A system drawn UNIFORMLY OVER SYSTEMS: descend the octree choosing
    // each child with probability proportional to its expected system
    // count (a uniform point in space would over-weight the big sparse
    // halo leaves), then accept the leaf if it is occupied.
    GalaxyOctree::CellId cell_id{0, 0, 0, 0};
    while (!octree.is_leaf(cell_id)) {
      double weights[8];
      double total = 0.0;
      for (int child = 0; child < 8; ++child) {
        const GalaxyOctree::CellId c{cell_id.x * 2 + (child & 1), cell_id.y * 2 + ((child >> 1) & 1),
                                     cell_id.z * 2 + ((child >> 2) & 1), cell_id.level + 1};
        weights[child] = octree.expected_systems(c).to_double();
        total += weights[child];
      }
      if (total <= 0.0) {
        break;
      }
      double roll = u01(next()) * total;
      int picked = 7;
      for (int child = 0; child < 8; ++child) {
        if (roll < weights[child]) {
          picked = child;
          break;
        }
        roll -= weights[child];
      }
      cell_id = GalaxyOctree::CellId{cell_id.x * 2 + (picked & 1), cell_id.y * 2 + ((picked >> 1) & 1),
                                     cell_id.z * 2 + ((picked >> 2) & 1), cell_id.level + 1};
    }
    const GalaxyOctree::CellId leaf = cell_id;
    if (!octree.occupied(leaf)) {
      continue;
    }
    {
      const Dir3 pp = octree.system_position_m(leaf);
      const double rr = std::sqrt(pp.x.to_double() * pp.x.to_double() + pp.y.to_double() * pp.y.to_double());
      if (rr > r_ly * kLightYearM) {
        continue;  // outside the requested disc
      }
    }
    const SystemCell cell{leaf.x, leaf.y, leaf.z, leaf.level};
    const Dir3 pos = octree.system_position_m(leaf);
    const core::Key system_key = system_key_in_galaxy(galaxy_key, cell);
    const core::StarPhys star = system_star(system_key);
    ++census.systems_sampled;
    const double dx = (pos.x - home.x).to_double();
    const double dy = (pos.y - home.y).to_double();
    const double dz = (pos.z - home.z).to_double();
    const double dist_ly = std::sqrt(dx * dx + dy * dy + dz * dz) / kLightYearM;
    int bin = static_cast<int>(dist_ly / r_gal_ly * 10.0);
    if (bin > 9) bin = 9;
    ++census.bin_systems[bin];
    const Owner owner = resolver.owner(cell, pos, star, t);
    if (!owner.owned) {
      const Owner later = resolver.owner(cell, pos, star, t + week_ns);
      if (later.owned) {
        ++census.new_claims_week;
      }
      continue;
    }
    ++census.systems_owned;
    const bool is_human = owner.race_key == human.key;
    if (is_human) {
      ++census.systems_human;
      ++census.bin_human[bin];
    } else {
      ++census.systems_alien;
      ++census.bin_alien[bin];
      continue;
    }
    const SystemCivContext context = gather_system_context(seed, registry, cell, true);
    const auto now = resolver.system_states(context, owner, t);
    const auto week = resolver.system_states(context, owner, t + week_ns);
    const auto later = resolver.system_states(context, owner, t + three_years_ns);
    {
      int best = -1;
      for (std::size_t i = 0; i < now.size(); ++i) {
        if (now[i].settled && (best < 0 || now[i].level > now[static_cast<std::size_t>(best)].level)) {
          best = static_cast<int>(i);
        }
      }
      if (best >= 0 && census.listed.size() < 64) {
        census.listed.push_back(CivCensus::Listed{cell, dist_ly, now[static_cast<std::size_t>(best)].level,
                                                  context.bodies[static_cast<std::size_t>(best)].slot,
                                                  context.bodies[static_cast<std::size_t>(best)].moon,
                                                  now[static_cast<std::size_t>(best)].domed});
      }
    }
    for (std::size_t i = 0; i < now.size(); ++i) {
      const CivState& s = now[i];
      if (!s.settled) {
        continue;
      }
      ++census.bodies_settled;
      ++census.level_hist[s.level];
      const int ft = static_cast<int>(s.faction_type);
      ++census.faction_hist[ft];
      if (dist_ly > 0.5 * r_gal_ly) ++census.faction_far[ft]; else ++census.faction_near[ft];
      census.domed += s.domed ? 1 : 0;
      census.ruined += s.ruined ? 1 : 0;
      census.level7 += s.level == 7 ? 1 : 0;
      if (s.ruined) {
        continue;
      }
      const bool at_cap = s.level >= s.max_level;
      const bool active = !at_cap && s.growth >= 0.7;
      census.at_cap += at_cap ? 1 : 0;
      census.active += active ? 1 : 0;
      const int adv = later[i].level - s.level;
      census.advance1_all += adv >= 1 ? 1 : 0;
      census.advance2_all += adv >= 2 ? 1 : 0;
      census.advance3_all += adv >= 3 ? 1 : 0;
      if (active) {
        census.advance1_active += adv >= 1 ? 1 : 0;
        census.advance2_active += adv >= 2 ? 1 : 0;
        census.advance3_active += adv >= 3 ? 1 : 0;
      }
      if (!s.domed) {
        ++census.open_air;
        census.advance1_open += adv >= 1 ? 1 : 0;
        census.advance2_open += adv >= 2 ? 1 : 0;
        census.advance3_open += adv >= 3 ? 1 : 0;
      }
      census.flip_week += week[i].level > s.level ? 1 : 0;
    }
  }
  return census;
}

std::string CivCensus::report() const {
  std::string out;
  char line[512];
  const auto pct = [](int a, int b) { return b > 0 ? 100.0 * a / b : 0.0; };
  std::snprintf(line, sizeof(line),
                "systems sampled %d: owned %d (human %d, alien %d); human front %.0f ly\n",
                systems_sampled, systems_owned, systems_human, systems_alien, front_ly);
  out += line;
  out += "owner fraction by distance from home (tenths of the galaxy radius):\n";
  for (int b = 0; b < 10; ++b) {
    std::snprintf(line, sizeof(line), "  %d.%d R: %5d systems, human %5.1f%%, alien %5.1f%%\n", b,
                  b + 1, bin_systems[b], pct(bin_human[b], bin_systems[b]),
                  pct(bin_alien[b], bin_systems[b]));
    out += line;
  }
  std::snprintf(line, sizeof(line), "human bodies settled %d (domed %.1f%%, ruined %.1f%%, level 7: %d)\n",
                bodies_settled, pct(domed, bodies_settled), pct(ruined, bodies_settled), level7);
  out += line;
  out += "levels L1..L7:";
  for (int k = 1; k <= 7; ++k) {
    std::snprintf(line, sizeof(line), " %.1f%%", pct(level_hist[k], bodies_settled));
    out += line;
  }
  out += "\nfaction types (all / inner half / outer half):";
  for (int f = 0; f < static_cast<int>(FactionType::Count); ++f) {
    int near_total = 0;
    int far_total = 0;
    for (int g = 0; g < static_cast<int>(FactionType::Count); ++g) {
      near_total += faction_near[g];
      far_total += faction_far[g];
    }
    std::snprintf(line, sizeof(line), "\n  %-16s %5.1f%% / %5.1f%% / %5.1f%%",
                  to_string(static_cast<FactionType>(f)), pct(faction_hist[f], bodies_settled),
                  pct(faction_near[f], near_total), pct(faction_far[f], far_total));
    out += line;
  }
  const int live = bodies_settled - ruined;
  std::snprintf(line, sizeof(line),
                "\nat cap %.1f%% of living; active (below cap, growth >= 0.7) %.1f%%\n"
                "advance within +3 yr, all living:    >=1 %.1f%%  >=2 %.1f%%  >=3 %.1f%%\n"
                "advance within +3 yr, active only:   >=1 %.1f%%  >=2 %.1f%%  >=3 %.1f%%\n"
                "advance within +3 yr, open-air only: >=1 %.1f%%  >=2 %.1f%%  >=3 %.1f%%  (%d colonies)\n"
                "level flips within +1 week: %.2f%%; systems newly claimed within +1 week: %d\n",
                pct(at_cap, live), pct(active, live), pct(advance1_all, live),
                pct(advance2_all, live), pct(advance3_all, live), pct(advance1_active, active),
                pct(advance2_active, active), pct(advance3_active, active),
                pct(advance1_open, open_air), pct(advance2_open, open_air),
                pct(advance3_open, open_air), open_air, pct(flip_week, live), new_claims_week);
  out += line;
  if (!listed.empty()) {
    out += "human systems sampled (cell x y z L, distance, best body):\n";
    for (const Listed& l : listed) {
      std::snprintf(line, sizeof(line), "  %lld %lld %lld %d  %6.0f ly  L%d slot %d%s%s\n",
                    static_cast<long long>(l.cell.x), static_cast<long long>(l.cell.y),
                    static_cast<long long>(l.cell.z), l.cell.level, l.dist_ly, l.level, l.slot,
                    l.moon >= 0 ? " (moon)" : "", l.domed ? " domed" : "");
      out += line;
    }
  }
  return out;
}

}  // namespace inf::gen
