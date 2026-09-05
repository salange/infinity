// Headless civilization inspection (T0020): the race registry around a
// position, later owners / states / census.

#include "commands_civ.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gen/civ_census.hpp"
#include "gen/civ_time.hpp"
#include "gen/colony.hpp"
#include "gen/civilization.hpp"
#include "gen/golden.hpp"
#include "gen/human.hpp"
#include "gen/universe.hpp"

namespace inf::cli {

int cmd_civ_races(const core::Seed128& seed, const double* at_ly, bool all) {
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
  std::vector<gen::Race> races = all ? registry.all_races() : registry.races_around(centre);
  races.push_back(registry.human());
  if (all) {
    std::printf("%zu race(s) in the whole galaxy (humans included):\n", races.size());
  } else {
    std::printf("%zu race(s) can reach this point (humans included):\n", races.size());
  }
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

namespace {

const char* star_class_name(core::StellarClass cls) {
  static constexpr const char* kNames[] = {"O", "B", "A", "F", "G", "K", "M", "WD", "NS", "BH"};
  const int i = static_cast<int>(cls);
  return i >= 0 && i < 10 ? kNames[i] : "?";
}

// "+N" = N real years after the launch reference; "YYYY-MM-DD" = that
// civil date at 00:00 UTC (proleptic Gregorian, no leap seconds — the
// display convention); default = the launch reference.
core::WorldTime parse_time(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return gen::kLaunchReference;
  }
  if (text[0] == '+' || text[0] == '-') {
    const double years = std::atof(text);
    return core::WorldTime::from_ns(gen::kLaunchReference.ns_since_epoch + gen::real_years_to_ns(years));
  }
  int y = 2000;
  int m = 1;
  int d = 1;
  if (std::sscanf(text, "%d-%d-%d", &y, &m, &d) < 1) {
    return gen::kLaunchReference;
  }
  // Days from civil (Howard Hinnant's algorithm).
  const int yy = y - (m <= 2 ? 1 : 0);
  const int era = (yy >= 0 ? yy : yy - 399) / 400;
  const int yoe = yy - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = static_cast<long long>(era) * 146097 + doe - 719468;  // since 1970-01-01
  const long long unix_s = days * 86400LL;
  return core::WorldTime::from_ns((unix_s - core::kEpochZeroUnixSeconds) * 1'000'000'000LL);
}

void print_states(const gen::ColonyResolver& resolver, const gen::SystemCivContext& context,
                  const gen::Owner& owner, core::WorldTime t) {
  const auto& races = resolver.candidates(context.position_m);
  if (!owner.owned) {
    std::printf("  unowned at this time\n");
    return;
  }
  const gen::Race& race = races[owner.candidate];
  std::printf("  owner: %s (%s), claimed %+.2f yr from launch via source %d\n",
              race.params.name.c_str(), gen::to_string(race.params.type),
              gen::ns_to_real_years(owner.t_claim.ns_since_epoch - gen::kLaunchReference.ns_since_epoch),
              owner.source_index);
  const auto states = resolver.system_states(context, owner, t);
  for (std::size_t i = 0; i < states.size(); ++i) {
    const gen::BodyCivInputs& b = context.bodies[i];
    const gen::CivState& s = states[i];
    char id[24];
    if (b.moon >= 0) std::snprintf(id, sizeof(id), "slot %d moon %d", b.slot, b.moon);
    else std::snprintf(id, sizeof(id), "slot %d", b.slot);
    if (!s.settled) {
      std::printf("  %-14s %-9s T=%.0fK g=%.1f  suit %.2f  %s\n", id, gen::to_string(b.type),
                  b.mean_temperature_k, b.gravity, s.suitability,
                  s.settled_at.ns_since_epoch > t.ns_since_epoch && s.suitability > 0.1
                      ? "claimed, settlers en route"
                      : "unsettled");
      continue;
    }
    const gen::FactionParams* f = s.faction_index >= 0 && s.faction_index < static_cast<int>(race.factions.size())
                                      ? &race.factions[static_cast<std::size_t>(s.faction_index)]
                                      : nullptr;
    const core::WorldTime next = resolver.next_change(context, b, owner, false, t);
    std::printf("  %-14s %-9s T=%.0fK g=%.1f  suit %.2f  L%d/%d %-22s %s%s%s  age %.2f yr  growth %.2f  "
                "progress %.2f  %s%s  next change %+.2f yr\n",
                id, gen::to_string(b.type), b.mean_temperature_k, b.gravity, s.suitability, s.level,
                s.max_level, gen::to_string(static_cast<gen::DevLevel>(s.level)),
                f != nullptr ? f->name.c_str() : "-", f != nullptr ? " [" : "",
                f != nullptr ? gen::faction_type_label(f->type, race.params.type, race.params.is_human) : "",
                gen::ns_to_real_years(static_cast<long long>(s.age_s * 1e9)), s.growth, s.progress,
                s.domed ? "DOMED " : "", s.ruined ? "RUINED " : (s.is_home ? "HOME " : ""),
                gen::ns_to_real_years(next.ns_since_epoch - gen::kLaunchReference.ns_since_epoch));
    if (f != nullptr) std::printf("%s", "");
  }
}

}  // namespace

int cmd_civ_state(const core::Seed128& seed, const long long* cell_xyzl, const char* time_text) {
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  const gen::ColonyResolver resolver(registry);
  gen::SystemCell cell{};
  if (cell_xyzl != nullptr) {
    cell = gen::SystemCell{cell_xyzl[0], cell_xyzl[1], cell_xyzl[2], static_cast<std::int32_t>(cell_xyzl[3])};
  }
  const core::WorldTime t = parse_time(time_text);
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, cell, true);
  std::printf("system (%lld, %lld, %lld, L%d) at (%.0f, %.0f, %.0f) ly, star %s, time %+.3f yr from launch\n",
              static_cast<long long>(cell.x), static_cast<long long>(cell.y), static_cast<long long>(cell.z),
              cell.level, context.position_m.x.to_double() / gen::kLightYearM,
              context.position_m.y.to_double() / gen::kLightYearM,
              context.position_m.z.to_double() / gen::kLightYearM,
              star_class_name(context.star.cls),
              gen::ns_to_real_years(t.ns_since_epoch - gen::kLaunchReference.ns_since_epoch));
  const auto& races = resolver.candidates(context.position_m);
  std::printf("  %zu candidate race(s):", races.size());
  for (const gen::Race& race : races) {
    const gen::Claim claim = gen::race_claim(race, cell, context.position_m, context.star, t);
    std::printf(" %s[%s%s]", race.params.name.c_str(), claim.reached ? "reached" : "-",
                claim.claimed ? ",claimed" : "");
  }
  std::printf("\n");
  print_states(resolver, context, resolver.owner(context, t), t);
  return 0;
}

int cmd_civ_census(const core::Seed128& seed, int max_systems, const char* time_text) {
  const core::WorldTime t = parse_time(time_text);
  const gen::CivCensus census = gen::run_civ_census(seed, t, max_systems);
  std::printf("census at %+.3f yr from launch\n%s", 
              gen::ns_to_real_years(t.ns_since_epoch - gen::kLaunchReference.ns_since_epoch),
              census.report().c_str());
  return 0;
}

int cmd_hash_civ() {
  std::fputs(gen::hash_civ_report().c_str(), stdout);
  return 0;
}

}  // namespace inf::cli
