// Headless civilization inspection (T0020): the race registry around a
// position, later owners / states / census.

#include "commands_civ.hpp"

#include <cmath>
#include <cstdio>

#include "gen/civ_time.hpp"
#include "gen/civilization.hpp"
#include "gen/golden.hpp"
#include "gen/human.hpp"
#include "gen/universe.hpp"

namespace inf::cli {

int cmd_civ_races(const core::Seed128& seed, const double* at_ly) {
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  gen::Dir3 at = gen::home_system_position_m(galaxy);
  if (at_ly != nullptr) {
    at = gen::Dir3{det::Real(at_ly[0] * gen::kLightYearM), det::Real(at_ly[1] * gen::kLightYearM),
                   det::Real(at_ly[2] * gen::kLightYearM)};
  }
  const gen::MacroCell centre = registry.macro_cell_of(at);
  std::printf("galaxy: %s %.0f ly, %u races ever started%s, macro level %d (%.0f ly cells)\n",
              gen::to_string(galaxy.type), galaxy.diameter_ly.to_double(), civ.race_count,
              civ.teeming ? " (teeming)" : "", civ.l_civ, civ.cell_width_ly);
  std::printf("at (%.0f, %.0f, %.0f) ly -> macro cell (%lld, %lld, %lld); block of %d cells, reach %d\n",
              at.x.to_double() / gen::kLightYearM, at.y.to_double() / gen::kLightYearM,
              at.z.to_double() / gen::kLightYearM, static_cast<long long>(centre.x),
              static_cast<long long>(centre.y), static_cast<long long>(centre.z),
              gen::RaceRegistry::kBlockCells, gen::RaceRegistry::kReach);
  const auto& races = registry.candidates_around(centre);
  std::printf("%zu race(s) can reach this point (humans included):\n", races.size());
  for (const gen::Race& race : races) {
    const gen::RaceParams& p = race.params;
    const gen::Dir3& home = p.sources[0].position_m;
    const double dx = (home.x - at.x).to_double();
    const double dy = (home.y - at.y).to_double();
    const double dz = (home.z - at.z).to_double();
    const double dist_ly = std::sqrt(dx * dx + dy * dy + dz * dz) / gen::kLightYearM;
    const double age_years =
        gen::ns_to_real_years(gen::kLaunchReference.ns_since_epoch - p.t_0.ns_since_epoch);
    std::printf("  %-14s %-11s home %6.0f ly away (cell %lld,%lld,%lld L%d)  age %5.1f yr [%4.0f gy]"
                "  reach %5.0f ly  speed %5.0f ly/yr  settle %.2f  repro %.2f  tech %d  "
                "peak L%d  home L%d  dome %.2f  factions %d%s%s\n",
                p.name.c_str(), gen::to_string(p.type), dist_ly,
                static_cast<long long>(race.home_system.x), static_cast<long long>(race.home_system.y),
                static_cast<long long>(race.home_system.z), race.home_system.level, age_years,
                age_years * 80.0, p.r_max_ly, p.speed_ly_per_year, p.settle_prob, p.reproduction,
                p.tech_tier, p.peak_level, p.home_level, p.dome_affinity, p.faction_count,
                p.extinct_ever ? (p.extinct_at(gen::kLaunchReference) ? "  EXTINCT" : "  (dying)") : "",
                p.type == gen::RaceType::Machine ? "  (machine)" : "");
    for (const gen::FactionParams& f : race.factions) {
      std::printf("      - %-30s %-16s start %+.1f yr  x(speed %.2f repro %.2f settle %.2f dome %.2f)%s\n",
                  f.name.c_str(), gen::faction_type_label(f.type, p.type, false),
                  gen::ns_to_real_years(f.t_start.ns_since_epoch - gen::kLaunchReference.ns_since_epoch),
                  f.speed_mul, f.reproduction_mul, f.settle_mul, f.dome_mul,
                  f.hostile ? "  HOSTILE" : "");
    }
  }
  // Reachability at the queried point (a race must be within r_max).
  int in_reach = 0;
  for (const gen::Race& race : races) {
    const gen::Dir3& home = race.params.sources[0].position_m;
    const double dx = (home.x - at.x).to_double();
    const double dy = (home.y - at.y).to_double();
    const double dz = (home.z - at.z).to_double();
    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= race.params.r_max_ly * gen::kLightYearM) {
      ++in_reach;
    }
  }
  std::printf("%d of them within their own reach of this point.\n", in_reach);
  return 0;
}

int cmd_civ_enclaves(const core::Seed128& seed) {
  const std::uint32_t count = gen::galaxy_count_in_cluster(gen::home_cluster_key(seed));
  int with = 0;
  int total = 0;
  for (std::uint32_t g = 1; g < count; ++g) {
    const auto enclaves = gen::human_enclaves(seed, 0, 0, 0, g);
    if (enclaves.empty()) {
      continue;
    }
    ++with;
    total += static_cast<int>(enclaves.size());
    for (const gen::HumanEnclave& e : enclaves) {
      std::printf("galaxy %3u enclave %u: at (%.0f, %.0f, %.0f) ly, arrived %+.2f yr, reach %.0f ly, "
                  "cap L%d, dead gate at (%.0f, %.0f, %.0f) ly in the home galaxy\n",
                  g, e.index, e.source.position_m.x.to_double() / gen::kLightYearM,
                  e.source.position_m.y.to_double() / gen::kLightYearM,
                  e.source.position_m.z.to_double() / gen::kLightYearM,
                  gen::ns_to_real_years(e.source.t_source.ns_since_epoch -
                                        gen::kHumanExpansionStart.ns_since_epoch),
                  e.source.r_max_ly, e.source.level_cap,
                  e.gate_partner_m.x.to_double() / gen::kLightYearM,
                  e.gate_partner_m.y.to_double() / gen::kLightYearM,
                  e.gate_partner_m.z.to_double() / gen::kLightYearM);
    }
  }
  std::printf("%u galaxies in the home cluster; %d (%.1f%%) hold human enclaves, %d beachheads, "
              "%zu dead gates in the home galaxy\n",
              count, with, count > 1 ? 100.0 * with / static_cast<double>(count - 1) : 0.0, total,
              gen::home_galaxy_gates(seed).size());
  return 0;
}

int cmd_hash_civ() {
  std::fputs(gen::hash_civ_report().c_str(), stdout);
  return 0;
}

}  // namespace inf::cli
