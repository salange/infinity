// Headless civilization inspection (T0020): the race registry around a
// position, later owners / states / census.

#include "commands_civ.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <chrono>

#include "gen/civ_census.hpp"
#include "gen/ecumenopolis.hpp"
#include "gen/planet_texture.hpp"
#include "gen/civ_time.hpp"
#include "gen/colony.hpp"
#include "gen/settlements.hpp"
#include "gen/sites.hpp"
#include "gen/civil.hpp"
#include "gen/site_mesh.hpp"
#include "gen/terrain.hpp"
#include "stb_image_write.h"
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

int cmd_civ_census(const core::Seed128& seed, int max_systems, const char* time_text, int min_level) {
  const core::WorldTime t = parse_time(time_text);
  const gen::CivCensus census = gen::run_civ_census(seed, t, max_systems);
  std::printf("census at %+.3f yr from launch\n%s", 
              gen::ns_to_real_years(t.ns_since_epoch - gen::kLaunchReference.ns_since_epoch),
              census.report().c_str());
  if (min_level > 0) {
    std::printf("systems with a body at level >= %d:\n", min_level);
    int count = 0;
    for (const gen::CivCensus::Listed& l : census.listed) {
      if (l.level < min_level) continue;
      ++count;
      std::printf("  --system %lld %lld %lld %d  slot %d moon %d  L%d%s  %.0f ly from home\n",
                  static_cast<long long>(l.cell.x), static_cast<long long>(l.cell.y),
                  static_cast<long long>(l.cell.z), l.cell.level, l.slot, l.moon, l.level,
                  l.domed ? " domed" : "", l.dist_ly);
    }
    if (count == 0) std::printf("  none among the %d sampled human systems\n", census.systems_human);
  }
  return 0;
}

// WP4: equirect map of a body's settlement plan — settled provinces by
// tier, faction hue on home worlds, roads, regional capitals.
int cmd_civ_map(const core::Seed128& seed, const long long* cell_xyzl, int slot_arg, int moon_arg,
                const char* time_text, const char* out_path) {
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
  const gen::Owner owner = resolver.owner(context, t);
  if (!owner.owned) {
    std::printf("system unowned at this time\n");
    return 1;
  }
  const auto states = resolver.system_states(context, owner, t);
  // The body: --slot/--moon, else the highest-level settled body.
  int pick = -1;
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    const gen::BodyCivInputs& b = context.bodies[i];
    if (slot_arg >= 0 && (b.slot != slot_arg || b.moon != moon_arg)) continue;
    if (slot_arg < 0 && !states[i].settled) continue;
    if (pick < 0 || states[i].level > states[static_cast<std::size_t>(pick)].level) pick = static_cast<int>(i);
  }
  if (pick < 0) {
    std::printf("no settled body (or the requested body is not settled)\n");
    return 1;
  }
  const gen::BodyCivInputs& body = context.bodies[static_cast<std::size_t>(pick)];
  const gen::CivState& state = states[static_cast<std::size_t>(pick)];
  const auto& races = resolver.candidates(context.position_m);
  const gen::Race& race = races[owner.candidate];
  // Rebuild the planet params the way the context did.
  gen::HomeSlotOverride slot_override;
  const auto over = registry.home_override(cell);
  if (over.has_value()) {
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
  }
  const gen::StarSystemParams system = gen::generate_system(context.system_key, over.has_value() ? &slot_override : nullptr);
  const gen::BodyKeys keys = body.moon >= 0 ? gen::moon_keys_in_system(context.system_key, body.slot, body.moon)
                                            : gen::body_keys_in_system(context.system_key, body.slot);
  const gen::PlanetParams params =
      body.moon >= 0 ? gen::planet_params_for_moon(system, body.slot, body.moon, gen::BodyHandle{keys.entity, keys.params})
                     : gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
  const gen::TerrainField field(keys.entity, params);
  const gen::SettlementPlanner planner(keys.entity, field, race.params, state.domed);
  const gen::SettlementPlan plan = planner.plan(state, race.factions);
  std::printf("body slot %d%s: %s L%d progress %.2f, %s\n", body.slot,
              body.moon >= 0 ? " (moon)" : "", gen::to_string(params.type), state.level, state.progress,
              plan.to_json().c_str());
  int tier_hist[9] = {};
  for (const gen::ProvinceSite& site : plan.provinces) {
    if (site.settled) ++tier_hist[static_cast<int>(site.tier)];
  }
  std::printf("tiers:");
  for (int k = 1; k <= 8; ++k) {
    if (tier_hist[k] > 0) std::printf(" %s=%d", gen::to_string(static_cast<gen::SettlementTier>(k)), tier_hist[k]);
  }
  std::printf("; regions %zu, roads %zu\n", plan.region_capitals.size(), plan.roads.size());
  // The PNG: sea dark blue, land by altitude grey, settled provinces by
  // tier (brightening), home-world faction hue, roads ochre, capitals red.
  constexpr int kWidth = 1024;
  constexpr int kHeight = 512;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<unsigned char> map(static_cast<std::size_t>(kWidth) * kHeight * 3);
  const double sea = params.sea_level_m.to_double();
  const double macro_amp = params.macro_amplitude_m.to_double();
  for (int y = 0; y < kHeight; ++y) {
    const double lat = kPi * (0.5 - (y + 0.5) / kHeight);
    for (int x = 0; x < kWidth; ++x) {
      const double lon = 2.0 * kPi * ((x + 0.5) / kWidth) - kPi;
      const double cl = std::cos(lat);
      const gen::Dir3 dir{det::Real(cl * std::cos(lon)), det::Real(cl * std::sin(lon)), det::Real(std::sin(lat))};
      const gen::FaceUV uv = gen::dir_to_face_uv(dir);
      const double elevation = field.macro().canonical_value(uv).to_double() * macro_amp;
      const gen::ProvinceSite& site = plan.at(field.provinces().cell_of(dir));
      unsigned char r = 0;
      unsigned char g = 0;
      unsigned char b = 0;
      if (params.land_fraction.to_double() < 0.999 && elevation < sea) {
        r = 18; g = 34; b = 70;
      } else {
        const double shade = std::clamp(0.35 + (elevation - sea) / 6000.0, 0.25, 0.75);
        r = g = b = static_cast<unsigned char>(shade * 255.0);
      }
      if (site.settled) {
        const double lift = 0.35 + 0.65 * static_cast<int>(site.tier) / 7.0;
        float hue[3] = {0.95f, 0.75f, 0.35f};
        if (plan.is_home && site.faction >= 0 && site.faction < static_cast<int>(race.factions.size())) {
          const gen::FactionParams& f = race.factions[static_cast<std::size_t>(site.faction)];
          hue[0] = f.accent[0]; hue[1] = f.accent[1]; hue[2] = f.accent[2];
        }
        r = static_cast<unsigned char>(std::min(255.0, r * (1.0 - lift) + 255.0 * hue[0] * lift));
        g = static_cast<unsigned char>(std::min(255.0, g * (1.0 - lift) + 255.0 * hue[1] * lift));
        b = static_cast<unsigned char>(std::min(255.0, b * (1.0 - lift) + 255.0 * hue[2] * lift));
      }
      const std::size_t pixel = (static_cast<std::size_t>(y) * kWidth + x) * 3;
      map[pixel] = r; map[pixel + 1] = g; map[pixel + 2] = b;
    }
  }
  const auto plot = [&](const gen::Dir3& d, unsigned char r, unsigned char g, unsigned char b, int size) {
    const double lat = std::asin(std::clamp(d.z.to_double(), -1.0, 1.0));
    const double lon = std::atan2(d.y.to_double(), d.x.to_double());
    const int px = static_cast<int>((lon + kPi) / (2.0 * kPi) * kWidth);
    const int py = static_cast<int>((0.5 - lat / kPi) * kHeight);
    for (int dy = -size; dy <= size; ++dy) {
      for (int dx = -size; dx <= size; ++dx) {
        const int xx = ((px + dx) % kWidth + kWidth) % kWidth;
        const int yy = std::clamp(py + dy, 0, kHeight - 1);
        const std::size_t pixel = (static_cast<std::size_t>(yy) * kWidth + xx) * 3;
        map[pixel] = r; map[pixel + 1] = g; map[pixel + 2] = b;
      }
    }
  };
  for (const gen::Road& road : plan.roads) {
    for (int i = 0; i < 8; ++i) {
      for (int s = 0; s < 12; ++s) {
        const double f = s / 12.0;
        gen::Dir3 p{road.points[i].x + (road.points[i + 1].x - road.points[i].x) * det::Real(f),
                    road.points[i].y + (road.points[i + 1].y - road.points[i].y) * det::Real(f),
                    road.points[i].z + (road.points[i + 1].z - road.points[i].z) * det::Real(f)};
        p = gen::normalize(p);
        plot(p, road.trunk ? 255 : 220, road.trunk ? 200 : 160, 60, road.trunk ? 1 : 0);
      }
    }
  }
  for (const gen::ProvinceSite& site : plan.provinces) {
    if (site.region_capital) plot(site.centre, 255, 60, 60, 2);
    if (site.capital) plot(site.centre, 255, 255, 255, 3);
  }
  if (stbi_write_png(out_path, kWidth, kHeight, 3, map.data(), kWidth * 3) == 0) {
    std::fprintf(stderr, "failed to write %s\n", out_path);
    return 1;
  }
  std::printf("wrote %s\n", out_path);
  return 0;
}

// WP5: one site of a body — its lots as a top-down PNG, a summary, and
// capture script lines (pos/aim) for the app at 20 km, 2 km and 200 m.
// WP7: an ecumenopolis world — the plate range, the block lattice, tile
// costs, and capture lines from orbit down to street level over the
// capital province.
int describe_ecumenopolis(const core::Key& entity, gen::TerrainField& field, const gen::SettlementPlan& plan,
                          const gen::Race& race, const gen::CivState& state, const gen::BodyCivInputs& body,
                          const gen::PlanetParams& params) {
  const auto t0 = std::chrono::steady_clock::now();
  const gen::EcumenopolisField ecum(entity, field, plan, race.params, race.factions, state);
  field.set_height_modifier(&ecum);
  const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double r = field.planet().radius_m.to_double();
  std::printf("body slot %d%s %s L7 %s: ecumenopolis (field %.1f ms)\n", body.slot, body.moon >= 0 ? " (moon)" : "",
              gen::to_string(params.type), state.ruined ? "RUINED" : "Trantorian", build_ms);
  std::printf("blocks: level %d, %u per face, %.1f m; plates %.0f..%.0f m (sea %.0f m)\n", ecum.block_level(),
              ecum.blocks_per_face(), ecum.block_m(), ecum.plate_min_m(), ecum.plate_max_m(),
              field.planet().sea_level_m.to_double());
  const gen::Dir3 centre = plan.capital >= 0 ? field.provinces().representative(plan.provinces[static_cast<std::size_t>(plan.capital)].cell)
                                             : gen::Dir3{det::Real(0.0), det::Real(0.0), det::Real(1.0)};
  const double plate = ecum.plate_m(centre);
  const gen::EcumenopolisField::District d = ecum.district(centre);
  std::printf("capital province %d: plate %.0f m, terrain %.0f m, district %d budget %.0f m\n", plan.capital, plate,
              field.base_elevation_m(centre).to_double(), static_cast<int>(d.type), d.height_budget_m);
  // Tile costs at every detail.
  for (const auto& [shift, detail] : std::vector<std::pair<int, int>>{{3, 0}, {3, 1}, {3, 2}, {5, 2}, {6, 3}}) {
    const auto t1 = std::chrono::steady_clock::now();
    const gen::EcumenopolisMesh mesh = gen::build_ecumenopolis_tile(ecum, ecum.tile_of(centre, shift), detail);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    // Vertex bounds relative to the tile origin (a sanity check on the
    // geometry: horizontal extent ~ the tile, height ~ the tallest tower).
    double max_h = 0.0;
    double max_z = -1e300;
    double min_z = 1e300;
    const double ox = mesh.mesh.origin[0], oy = mesh.mesh.origin[1], oz = mesh.mesh.origin[2];
    const double olen = std::sqrt(ox * ox + oy * oy + oz * oz);
    for (std::size_t v = 0; v + 2 < mesh.mesh.vertices.size(); v += 10) {
      const double px = mesh.mesh.vertices[v], py = mesh.mesh.vertices[v + 1], pz = mesh.mesh.vertices[v + 2];
      const double radial = (px * ox + py * oy + pz * oz) / olen;
      const double h2 = px * px + py * py + pz * pz - radial * radial;
      max_h = std::max(max_h, std::sqrt(std::max(0.0, h2)));
      max_z = std::max(max_z, radial);
      min_z = std::min(min_z, radial);
    }
    std::printf("tile shift %d (%.0f m) detail %d: %u blocks, %u towers, %u triangles, %.1f ms; extent %.0f m, z %.0f..%.0f m\n", shift,
                ecum.tile_m(shift), detail, mesh.block_count, mesh.tower_count, mesh.triangle_count, ms, max_h, min_z, max_z);
  }
  // The far-view bake: how much of the surface carries urban albedo and
  // night light (the orbit impostor's alpha).
  {
    const auto t1 = std::chrono::steady_clock::now();
    const gen::PlanetTexture bake = gen::bake_planet_texture(field, 64, nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::size_t texels = 0;
    std::size_t lit = 0;
    double alpha_sum = 0.0;
    for (const auto& face : bake.faces) {
      for (std::size_t i = 3; i < face.rgba.size(); i += 4) {
        ++texels;
        alpha_sum += face.rgba[i] / 255.0;
        lit += face.rgba[i] > 8 ? 1 : 0;
      }
    }
    std::printf("bake 64: %zu texels, night alpha > 0.03 on %.1f%%, mean alpha %.3f, %.0f ms\n", texels,
                texels > 0 ? 100.0 * lit / texels : 0.0, texels > 0 ? alpha_sum / texels : 0.0, ms);
  }
  // Capture lines: orbit, 50 km, 2 km, street.
  gen::Dir3 east{};
  gen::Dir3 north{};
  gen::tangent_basis(centre, &east, &north);
  const double cx = centre.x.to_double();
  const double cy = centre.y.to_double();
  const double cz = centre.z.to_double();
  std::printf("centre dir (%.6f, %.6f, %.6f)  planet-local (%.1f, %.1f, %.1f)\n", cx, cy, cz, cx * (r + plate),
              cy * (r + plate), cz * (r + plate));
  std::printf("# capture script lines (app --script):\n");
  for (const double range : {2.5 * r, 50000.0, 2000.0}) {
    const double back = range * 0.8;
    const double height = range * 0.6;
    const double px = cx * (r + plate + height) - north.x.to_double() * back;
    const double py = cy * (r + plate + height) - north.y.to_double() * back;
    const double pz = cz * (r + plate + height) - north.z.to_double() * back;
    const double tx = cx * (r + plate) - px;
    const double ty = cy * (r + plate) - py;
    const double tz = cz * (r + plate) - pz;
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    std::printf("pos %.1f %.1f %.1f\naim dir %.5f %.5f %.5f   # %.0f m\n", px, py, pz, tx / len, ty / len, tz / len,
                range);
  }
  {
    // Street level: on an arterial 30 m above the plate, looking along it.
    const gen::EcumenopolisField::BlockId block = ecum.block_of(centre);
    const gen::EcumenopolisField::BlockId on_arterial{block.face, (block.bi >> 3) << 3, block.bj};
    const gen::Dir3 corner = ecum.block_corner(on_arterial, 0);
    const double h = ecum.plate_m(corner) + 30.0;
    const double px = corner.x.to_double() * (r + h);
    const double py = corner.y.to_double() * (r + h);
    const double pz = corner.z.to_double() * (r + h);
    const gen::Dir3 ahead = ecum.block_corner(gen::EcumenopolisField::BlockId{on_arterial.face, on_arterial.bi, on_arterial.bj + 6}, 0);
    const double tx = ahead.x.to_double() * (r + h - 10.0) - px;
    const double ty = ahead.y.to_double() * (r + h - 10.0) - py;
    const double tz = ahead.z.to_double() * (r + h - 10.0) - pz;
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    std::printf("pos %.1f %.1f %.1f\naim dir %.5f %.5f %.5f   # street\n", px, py, pz, tx / len, ty / len, tz / len);
  }
  return 0;
}

int cmd_civ_site(const core::Seed128& seed, const long long* cell_xyzl, int slot_arg, int moon_arg,
                 const char* tier_text, int site_index, const char* time_text,
                 const char* out_path) {
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
  const gen::Owner owner = resolver.owner(context, t);
  if (!owner.owned) {
    std::printf("system unowned at this time\n");
    return 1;
  }
  const auto states = resolver.system_states(context, owner, t);
  int pick = -1;
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    const gen::BodyCivInputs& b = context.bodies[i];
    if (slot_arg >= 0 && (b.slot != slot_arg || b.moon != moon_arg)) continue;
    if (slot_arg < 0 && !states[i].settled) continue;
    if (pick < 0 || states[i].level > states[static_cast<std::size_t>(pick)].level) pick = static_cast<int>(i);
  }
  if (pick < 0 || !states[static_cast<std::size_t>(pick)].settled) {
    std::printf("no settled body\n");
    return 1;
  }
  const gen::BodyCivInputs& body = context.bodies[static_cast<std::size_t>(pick)];
  const gen::CivState& state = states[static_cast<std::size_t>(pick)];
  const auto& races = resolver.candidates(context.position_m);
  const gen::Race& race = races[owner.candidate];
  gen::HomeSlotOverride slot_override;
  const auto over = registry.home_override(cell);
  if (over.has_value()) {
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
  }
  const gen::StarSystemParams system = gen::generate_system(context.system_key, over.has_value() ? &slot_override : nullptr);
  const gen::BodyKeys keys = body.moon >= 0 ? gen::moon_keys_in_system(context.system_key, body.slot, body.moon)
                                            : gen::body_keys_in_system(context.system_key, body.slot);
  const gen::PlanetParams params =
      body.moon >= 0 ? gen::planet_params_for_moon(system, body.slot, body.moon, gen::BodyHandle{keys.entity, keys.params})
                     : gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
  gen::TerrainField field(keys.entity, params);
  const gen::SettlementPlanner planner(keys.entity, field, race.params, state.domed);
  const gen::SettlementPlan plan = planner.plan(state, race.factions);
  if (state.level >= 7) {
    return describe_ecumenopolis(keys.entity, field, plan, race, state, body, params);
  }
  const gen::SiteField sites(keys.entity, field, plan, race.params, race.factions, state);
  const gen::CivilField civil(sites, field);
  field.set_height_modifier(&civil);
  // The site: by --tier (first site of that tier by rank) or --site index.
  gen::SettlementTier want = gen::SettlementTier::Town;
  if (tier_text != nullptr) {
    for (int k = 1; k <= 7; ++k) {
      if (std::strcmp(tier_text, gen::to_string(static_cast<gen::SettlementTier>(k))) == 0) want = static_cast<gen::SettlementTier>(k);
    }
  }
  const gen::Site* site = nullptr;
  if (site_index >= 0 && site_index < static_cast<int>(sites.sites().size())) {
    site = &sites.sites()[static_cast<std::size_t>(site_index)];
  } else {
    // First site of the tier by rank; a site still filling its ring is
    // preferred (it shows growth between two times).
    const gen::Site* complete = nullptr;
    for (const std::uint32_t index : plan.by_rank) {
      const gen::Site* candidate = sites.site_of(plan.provinces[index].cell);
      if (candidate == nullptr || static_cast<gen::SettlementTier>(candidate->tier) != want) continue;
      if (candidate->progress < 0.85f) {
        site = candidate;
        break;
      }
      if (complete == nullptr) complete = candidate;
    }
    if (site == nullptr) site = complete;
  }
  std::printf("body slot %d%s %s L%d: %zu sites; plan %s\n", body.slot, body.moon >= 0 ? " (moon)" : "",
              gen::to_string(params.type), state.level, sites.sites().size(), plan.to_json().c_str());
  int per_tier[9] = {};
  for (const gen::Site& s : sites.sites()) ++per_tier[s.tier];
  std::printf("sites by tier:");
  for (int k = 1; k <= 7; ++k) if (per_tier[k] > 0) std::printf(" %s=%d", gen::to_string(static_cast<gen::SettlementTier>(k)), per_tier[k]);
  std::printf("\n");
  if (site == nullptr) {
    std::printf("no site of tier %s\n", gen::to_string(want));
    return 1;
  }
  std::vector<gen::Lot> lots;
  sites.all_lots(*site, &lots);
  int under_construction = 0;
  for (const gen::Lot& lot : lots) under_construction += lot.style.construction < 1.0f ? 1 : 0;
  const double r = field.planet().radius_m.to_double();
  const gen::Dir3& up = site->frame.up;
  std::printf("site: province %u tier %s (max %s) radius %.0f m family %s datum %.1f m (sea %.1f) "
              "progress %.3f lots %zu (%d under construction) arterials %zu%s%s\n",
              site->province, gen::to_string(static_cast<gen::SettlementTier>(site->tier)),
              gen::to_string(static_cast<gen::SettlementTier>(site->max_tier)), site->radius_m,
              gen::to_string(site->family), site->datum_m, site->sea_m, site->progress, lots.size(),
              under_construction, site->arterials.size(), site->coastal ? " coastal" : "",
              site->river ? " river" : "");
  const double cx = up.x.to_double();
  const double cy = up.y.to_double();
  const double cz = up.z.to_double();
  std::printf("centre dir (%.6f, %.6f, %.6f)  planet-local (%.1f, %.1f, %.1f)\n", cx, cy, cz,
              cx * (r + site->datum_m), cy * (r + site->datum_m), cz * (r + site->datum_m));
  // Capture script lines: the camera above and to the south of the site,
  // looking down at ~35 degrees, at three ranges.
  std::printf("# capture script lines (app --script):\n");
  const gen::Dir3& north = site->frame.north;
  for (const double range : {20000.0, 2000.0, 200.0}) {
    const double back = range * 0.8;
    const double height = range * 0.6;
    const double px = cx * (r + site->datum_m + height) - north.x.to_double() * back;
    const double py = cy * (r + site->datum_m + height) - north.y.to_double() * back;
    const double pz = cz * (r + site->datum_m + height) - north.z.to_double() * back;
    const double tx = cx * (r + site->datum_m) - px;
    const double ty = cy * (r + site->datum_m) - py;
    const double tz = cz * (r + site->datum_m) - pz;
    const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
    std::printf("pos %.1f %.1f %.1f\naim dir %.5f %.5f %.5f   # %.0f m\n", px, py, pz, tx / len, ty / len,
                tz / len, range);
  }
  // One lot for the building comparison: the first residential lot
  // within 0.3 R of the centre — camera lines at 200 m and 20 m looking
  // at it from the south-west, 30 degrees down.
  {
    const gen::Lot* pick_lot = nullptr;
    double best_h = -1.0;
    for (const gen::Lot& lot : lots) {
      if (lot.usage != gen::LotUsage::Residential || lot.style.construction < 1.0f) continue;
      const double lx = lot.footprint[0][0];
      const double ly = lot.footprint[0][1];
      const double d = std::sqrt(lx * lx + ly * ly);
      if (d < 0.3 * site->radius_m && lot.height_budget_m > best_h) {
        best_h = lot.height_budget_m;
        pick_lot = &lot;
      }
    }
    if (pick_lot != nullptr) {
      double lx = 0.0;
      double ly = 0.0;
      for (int k = 0; k < pick_lot->vertex_count; ++k) {
        lx += pick_lot->footprint[k][0];
        ly += pick_lot->footprint[k][1];
      }
      lx /= pick_lot->vertex_count;
      ly /= pick_lot->vertex_count;
      const double lot_z = field.elevation_m(site->frame.to_dir(lx, ly)).to_double();
      std::printf("# lot %u (%s, budget %.1f m) capture lines:\n", pick_lot->id, gen::to_string(pick_lot->usage),
                  static_cast<double>(pick_lot->height_budget_m));
      for (const double range : {200.0, 20.0}) {
        const double back = range * 0.87;
        const double up = range * 0.5 + 0.5 * pick_lot->height_budget_m;
        const double ex = site->frame.east.x.to_double(), ey = site->frame.east.y.to_double(), ez = site->frame.east.z.to_double();
        const double nx = site->frame.north.x.to_double(), ny = site->frame.north.y.to_double(), nz = site->frame.north.z.to_double();
        const double ux = site->frame.up.x.to_double(), uy = site->frame.up.y.to_double(), uz = site->frame.up.z.to_double();
        const gen::Dir3 ld = site->frame.to_dir(lx, ly);
        const double tx0 = ld.x.to_double() * (r + lot_z + 0.5 * pick_lot->height_budget_m);
        const double ty0 = ld.y.to_double() * (r + lot_z + 0.5 * pick_lot->height_budget_m);
        const double tz0 = ld.z.to_double() * (r + lot_z + 0.5 * pick_lot->height_budget_m);
        const double px = tx0 - (ex + nx) * 0.7071 * back + ux * up;
        const double py = ty0 - (ey + ny) * 0.7071 * back + uy * up;
        const double pz = tz0 - (ez + nz) * 0.7071 * back + uz * up;
        const double dx = tx0 - px, dy = ty0 - py, dz = tz0 - pz;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        std::printf("pos %.1f %.1f %.1f\naim dir %.5f %.5f %.5f   # lot at %.0f m\n", px, py, pz, dx / len, dy / len, dz / len, range);
      }
    }
  }
  // Top-down PNG: 1024 px across 2.2 R; lots by usage, arterials ochre,
  // construction hatched, ground shade from the civil elevation.
  constexpr int kSize = 1024;
  std::vector<unsigned char> map(static_cast<std::size_t>(kSize) * kSize * 3, 0);
  const double span = 2.2 * site->radius_m;
  const double scale = kSize / span;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const double lx = (x + 0.5) / scale - 0.5 * span;
      const double ly = 0.5 * span - (y + 0.5) / scale;
      const double e = field.elevation_m(site->frame.to_dir(lx, ly)).to_double();
      const double shade = std::clamp(0.3 + (e - site->datum_m) / 80.0, 0.1, 0.8);
      const std::size_t pixel = (static_cast<std::size_t>(y) * kSize + x) * 3;
      const bool sea = params.land_fraction.to_double() < 0.999 && e < params.sea_level_m.to_double();
      map[pixel] = static_cast<unsigned char>((sea ? 0.1 : shade) * 255.0);
      map[pixel + 1] = static_cast<unsigned char>((sea ? 0.18 : shade * 1.05) * 255.0);
      map[pixel + 2] = static_cast<unsigned char>((sea ? 0.35 : shade * 0.9) * 255.0);
    }
  }
  const auto fill = [&](double lx, double ly, int size, unsigned char rr, unsigned char gg, unsigned char bb) {
    const int px = static_cast<int>((lx + 0.5 * span) * scale);
    const int py = static_cast<int>((0.5 * span - ly) * scale);
    for (int dy = -size; dy <= size; ++dy) {
      for (int dx = -size; dx <= size; ++dx) {
        const int xx = px + dx;
        const int yy = py + dy;
        if (xx < 0 || yy < 0 || xx >= kSize || yy >= kSize) continue;
        const std::size_t pixel = (static_cast<std::size_t>(yy) * kSize + xx) * 3;
        map[pixel] = rr; map[pixel + 1] = gg; map[pixel + 2] = bb;
      }
    }
  };
  for (const gen::Arterial& a : site->arterials) {
    for (std::size_t s2 = 0; s2 + 3 < a.xy.size(); s2 += 2) {
      for (int k = 0; k < 64; ++k) {
        const double f = k / 64.0;
        fill(a.xy[s2] + (a.xy[s2 + 2] - a.xy[s2]) * f, a.xy[s2 + 1] + (a.xy[s2 + 3] - a.xy[s2 + 1]) * f,
             std::max(1, static_cast<int>(0.5 * a.width_m * scale)), 210, 170, 90);
      }
    }
  }
  for (const gen::Lot& lot : lots) {
    double ccx = 0.0;
    double ccy = 0.0;
    for (int k = 0; k < lot.vertex_count; ++k) { ccx += lot.footprint[k][0]; ccy += lot.footprint[k][1]; }
    ccx /= lot.vertex_count;
    ccy /= lot.vertex_count;
    unsigned char rr = 200, gg = 190, bb = 170;
    switch (lot.usage) {
      case gen::LotUsage::Civic: rr = 235; gg = 225; bb = 120; break;
      case gen::LotUsage::Industrial: rr = 170; gg = 120; bb = 100; break;
      case gen::LotUsage::Agricultural: rr = 120; gg = 170; bb = 80; break;
      case gen::LotUsage::Pad: rr = 90; gg = 90; bb = 110; break;
      case gen::LotUsage::Monument: rr = 255; gg = 255; bb = 255; break;
      default: break;
    }
    if (lot.style.construction < 1.0f) { rr = 255; gg = 80; bb = 200; }
    const double half = 0.5 * std::sqrt(std::fabs((lot.footprint[1][0] - lot.footprint[0][0]) * (lot.footprint[2][1] - lot.footprint[0][1])));
    fill(ccx, ccy, std::max(1, static_cast<int>(half * scale)), rr, gg, bb);
  }
  if (stbi_write_png(out_path, kSize, kSize, 3, map.data(), kSize * 3) == 0) {
    std::fprintf(stderr, "failed to write %s\n", out_path);
    return 1;
  }
  const gen::SiteMeshParams mp;
  const gen::SiteMesh mesh = gen::build_site_mesh(sites, *site, field, mp);
  std::printf("mass mesh: %u lots, %u triangles; wrote %s\n", mesh.lot_count, mesh.triangle_count, out_path);
  return 0;
}

int cmd_hash_civ() {
  std::fputs(gen::hash_civ_report().c_str(), stdout);
  return 0;
}

}  // namespace inf::cli
