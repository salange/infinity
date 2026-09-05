#include "gen/colony.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/climate.hpp"
#include "gen/names.hpp"
#include "gen/universe.hpp"
#include "world/noise.hpp"

namespace inf::gen {

using civ::u01;
using det::Real;

namespace {

constexpr double kG = 9.81;

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
double uniform_from(double u, double lo, double hi) { return lo + (hi - lo) * u; }

// Linear band membership: 1 inside [lo, hi], falling to 0 over `soft`
// outside either edge.
double band(double x, double lo, double hi, double soft) {
  if (x >= lo && x <= hi) return 1.0;
  const double d = x < lo ? lo - x : x - hi;
  return d >= soft ? 0.0 : 1.0 - d / soft;
}

bool is_organic(RaceType type) {
  return type != RaceType::Machine && type != RaceType::Crystalline &&
         type != RaceType::Precursor;
}

std::int64_t race_counter(const core::Key& race_key) {
  return static_cast<std::int64_t>(race_key.k0 ^ (race_key.k1 * 0x9E3779B97F4A7C15ULL));
}

double distance_m(const Dir3& a, const Dir3& b) {
  const double dx = (a.x - b.x).to_double();
  const double dy = (a.y - b.y).to_double();
  const double dz = (a.z - b.z).to_double();
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// --- spread model ---------------------------------------------------------------

double star_factor(const core::StarPhys& star) {
  double f = 1.0;
  switch (star.cls) {
    case core::StellarClass::O:
    case core::StellarClass::B:
    case core::StellarClass::A: f = 0.2; break;
    case core::StellarClass::F: f = 0.8; break;
    case core::StellarClass::G: f = 1.2; break;
    case core::StellarClass::K: f = 1.15; break;
    case core::StellarClass::M: f = 0.7; break;
    default: f = 0.1; break;  // remnants
  }
  if (star.age_gyr.to_double() < 0.5) {
    f *= 0.5;  // too young for planets worth settling
  }
  return f;
}

Claim race_claim(const Race& race, const SystemCell& cell, const Dir3& system_pos_m,
                 const core::StarPhys& star, core::WorldTime t) {
  Claim claim;
  const RaceParams& p = race.params;
  if (p.sources.empty()) {
    return claim;
  }
  // The cradle: owned by its race from the founding instant.
  if (!race.void_home && race.home_system == cell) {
    claim.reached = true;
    claim.claimed = true;
    claim.home = true;
    claim.t_arrive = p.t_0;
    claim.t_claim = p.t_0;
    claim.p_wave = 1.0;
    return claim;
  }
  const core::Key claims_key = core::derive_named(race.key, name::RaceClaimsV1);
  // Anisotropy: three octaves of keyed noise over galactocentric position
  // at wavelength ~R_max/2 — lobed territories, no graph, no search.
  double aniso = 0.0;
  if (p.anisotropy > 0.0) {
    const std::uint64_t lattice = core::lattice_key(claims_key, channel::Lattice);
    const double inv_wavelength = 2.0 / (p.r_max_ly * kLightYearM);
    world::FbmParams fbm;
    fbm.octaves = 3;
    fbm.gain = Real(0.5);
    fbm.octave0_damp = Real(1.0);
    aniso = p.anisotropy * world::fbm3(lattice, Real(system_pos_m.x.to_double() * inv_wavelength),
                                        Real(system_pos_m.y.to_double() * inv_wavelength),
                                        Real(system_pos_m.z.to_double() * inv_wavelength), fbm)
                               .to_double();
  }
  // Earliest arrival over the sources.
  double best_arrive_s = 1.0e300;
  int best_source = -1;
  double best_d_ly = 0.0;
  double best_speed = 1.0;
  for (std::size_t i = 0; i < p.sources.size(); ++i) {
    const Source& src = p.sources[i];
    const double d_ly = distance_m(system_pos_m, src.position_m) / kLightYearM * (1.0 + aniso);
    const double r_max = src.r_max_ly > 0.0 ? src.r_max_ly : p.r_max_ly;
    if (d_ly > r_max) {
      continue;
    }
    const double speed = p.speed_ly_per_year * src.speed_scale;
    const double arrive_s = ns_to_real_seconds(src.t_source.ns_since_epoch) +
                            d_ly / speed * kRealYearS;
    if (arrive_s < best_arrive_s) {
      best_arrive_s = arrive_s;
      best_source = static_cast<int>(i);
      best_d_ly = d_ly;
      best_speed = speed;
    }
  }
  if (best_source < 0) {
    return claim;  // beyond every source's reach
  }
  const Source& src = p.sources[static_cast<std::size_t>(best_source)];
  const core::Key system_claim =
      core::derive_child(claims_key, kind::System, cell.x, cell.y, cell.z, cell.level);
  const auto d = core::draw_point(system_claim, channel::Claim, 0, 0, 0);
  const double jitter_s = u01(d[0]) * 0.3 * best_d_ly / best_speed * kRealYearS;
  const double t_arrive_s = best_arrive_s + jitter_s;
  claim.source_index = best_source;
  claim.d_eff_ly = best_d_ly;
  claim.t_arrive = core::WorldTime::from_ns(static_cast<std::int64_t>(t_arrive_s * 1e9));
  const double now_s = ns_to_real_seconds(t.ns_since_epoch);
  claim.reached = now_s >= t_arrive_s;
  const double falloff = p.falloff_ly > 1.0 ? p.falloff_ly : 1.0;
  const double p_wave = p.settle_prob * src.settle_scale *
                        det::fast_exp(Real(-best_d_ly / falloff)).to_double() * star_factor(star);
  claim.p_wave = p_wave;
  const double u1 = u01(d[1]);
  const double u2 = u01(d[2]);
  const double tau_s = kInfillTauYears * kRealYearS;
  if (u1 < p_wave) {
    claim.claimed = claim.reached;
    claim.t_claim = claim.t_arrive;
    return claim;
  }
  // Infill: the reached volume keeps filling in behind the front. The
  // settlement instant is the inverse of p_fill(t) at u2 — closed form.
  const double fill_cap = 0.6 * p_wave;
  if (u2 < fill_cap) {
    const double instant_s =
        t_arrive_s - tau_s * det::fast_log(Real(1.0 - u2 / fill_cap)).to_double();
    claim.t_claim = core::WorldTime::from_ns(static_cast<std::int64_t>(instant_s * 1e9));
    claim.claimed = now_s >= instant_s;
    return claim;
  }
  return claim;  // never settles from this source
}

// --- inputs -----------------------------------------------------------------------

SystemCivContext gather_system_context(const core::Seed128& seed, const RaceRegistry& registry,
                                       const SystemCell& cell, bool include_moons) {
  SystemCivContext context;
  context.cell = cell;
  context.system_key = system_key_in_galaxy(home_galaxy_key(seed), cell);
  context.position_m = registry.system_position_m(cell);
  const auto over = registry.home_override(cell);
  HomeSlotOverride slot_override;
  if (over.has_value()) {
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
  }
  const StarSystemParams system =
      generate_system(context.system_key, over.has_value() ? &slot_override : nullptr);
  context.star = system.star;
  context.metallicity = system.star.metallicity.to_double();
  const auto gather = [&](const PlanetParams& params, const core::Key& entity, int slot, int moon,
                          bool solid, bool race_home) {
    BodyCivInputs body;
    body.slot = slot;
    body.moon = moon;
    body.body_entity = entity;
    body.type = params.type;
    body.solid = solid;
    body.is_race_home = race_home;
    body.radius_m = params.radius_m.to_double();
    body.gravity = params.gravity.to_double();
    body.has_atmosphere = params.pressure_rel.to_double() > 0.0;
    body.land_fraction = params.land_fraction.to_double();
    const MacroField macro(entity);
    const ClimateField climate(entity, params, macro);
    body.mean_temperature_k = climate.mean_temperature_k();
    const LifeParams life = derive_life(entity, params, climate);
    body.life_stage = life.stage;
    body.life_occupied = life.occupied;
    context.bodies.push_back(body);
  };
  for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
    const SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
    if (!planet.occupied) {
      continue;
    }
    const BodyKeys keys = body_keys_in_system(context.system_key, slot);
    const PlanetParams params =
        planet_params_for_slot(system, slot, BodyHandle{keys.entity, keys.params});
    const bool solid = planet.phys.cls == core::PlanetClass::Rocky ||
                       planet.phys.cls == core::PlanetClass::SuperEarth;
    gather(params, keys.entity, slot, -1, solid, planet.race_home);
    if (!include_moons) {
      continue;
    }
    for (std::size_t m = 0; m < planet.moons.size(); ++m) {
      const BodyKeys moon_keys = moon_keys_in_system(context.system_key, slot, static_cast<int>(m));
      const PlanetParams moon_params = planet_params_for_moon(
          system, slot, static_cast<int>(m), BodyHandle{moon_keys.entity, moon_keys.params});
      gather(moon_params, moon_keys.entity, slot, static_cast<int>(m), true, false);
    }
  }
  return context;
}

// --- suitability --------------------------------------------------------------

double suitability(const RaceParams& race, const BodyCivInputs& body) {
  if (!body.solid) {
    return 0.0;
  }
  const Habitat& h = race.habitat;
  double match = 0.0;
  for (int i = 0; i < h.preferred_count; ++i) {
    if (h.preferred[i] == body.type) {
      match = 1.0;
    }
  }
  if (h.ignores_climate) {
    // Machines: anything solid; prefer the airless and the metal-rich.
    double s = body.has_atmosphere ? 0.7 : 1.0;
    s *= band(body.gravity, h.gravity_lo, h.gravity_hi, 0.5 * kG);
    return clamp01(s);
  }
  if (match == 0.0) {
    if (h.cryogenic) {
      match = 0.3;
    } else {
      match = body.has_atmosphere && body.type != PlanetType::Barren ? 0.35 : 0.0;
    }
  }
  if (match <= 0.0) {
    return 0.0;
  }
  double temp;
  if (h.cryogenic) {
    // Cold, or hot and dry — never temperate.
    const double cold = band(body.mean_temperature_k, h.temp_lo_k, h.temp_hi_k, 40.0);
    const double hot = body.type == PlanetType::Desert
                           ? band(body.mean_temperature_k, 350.0, 600.0, 60.0)
                           : 0.0;
    temp = cold > hot ? cold : hot;
  } else {
    // A soft 80 K shoulder: marginal (hot Desert, cold EarthLike) worlds
    // are settled unaided at reduced suitability rather than domed.
    temp = band(body.mean_temperature_k, h.temp_lo_k, h.temp_hi_k, 80.0);
  }
  const double gravity = band(body.gravity, h.gravity_lo, h.gravity_hi, 0.5 * kG);
  if (h.needs_atmosphere && !body.has_atmosphere) {
    return 0.0;
  }
  double s = match * temp * gravity;
  if (is_organic(race.type)) {
    if (body.life_occupied) {
      if (body.life_stage == LifeStage::FullBiosphere) s *= 1.3;
      else if (body.life_stage == LifeStage::CrustColonisation) s *= 1.1;
    }
  }
  if (h.wants_ocean) {
    s *= body.land_fraction < 0.5 ? 1.0 : 0.4;
  }
  return clamp01(s);
}

double dome_suitability(const RaceParams& race, const BodyCivInputs& body) {
  if (!body.solid || body.mean_temperature_k > 700.0) {
    return 0.0;
  }
  const double in_band = band(body.gravity, race.habitat.gravity_lo, race.habitat.gravity_hi, 0.0);
  return in_band > 0.5 ? 0.35 : 0.25;
}

// --- the ladder -----------------------------------------------------------------

double time_to_level(int level, double rate, bool domed) {
  if (level <= 1) {
    return 0.0;
  }
  if (rate <= 0.0) {
    return 1.0e300;
  }
  const double t3 = kLevelThresholdYears[3] * kRealYearS / rate;
  if (level <= 3 || !domed) {
    return kLevelThresholdYears[level] * kRealYearS / rate;
  }
  return t3 + (kLevelThresholdYears[level] - kLevelThresholdYears[3]) * kRealYearS /
                  (rate * kDomedRateAboveThree);
}

int level_for_age(double age_s, double rate, bool domed) {
  int level = 0;
  for (int k = 1; k <= 7; ++k) {
    if (age_s >= time_to_level(k, rate, domed)) {
      level = k;
    }
  }
  return level;
}

// --- owner ---------------------------------------------------------------------

Owner ColonyResolver::owner(const SystemCell& cell, const Dir3& system_pos_m,
                            const core::StarPhys& star, core::WorldTime t) const {
  Owner owner;
  const std::vector<Race>& candidates = registry_.candidates_around(system_pos_m);
  // A cradle is sacrosanct: nobody colonises a system whose natives are
  // about to go interstellar. It is unowned before the race's founding and
  // that race's afterwards, whatever older wave passes by.
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const Race& race = candidates[i];
    if (race.void_home || !(race.home_system == cell)) {
      continue;
    }
    if (t >= race.params.t_0) {
      owner.owned = true;
      owner.candidate = i;
      owner.race_key = race.key;
      owner.t_claim = race.params.t_0;
      owner.source_index = 0;
    }
    return owner;
  }
  bool have = false;
  bool have_home = false;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const Claim claim = race_claim(candidates[i], cell, system_pos_m, star, t);
    if (!claim.claimed) {
      continue;
    }
    const core::Key& key = candidates[i].key;
    // A race's cradle is its own regardless of who arrived earlier;
    // otherwise the earliest claim wins, ties by race key.
    if (have_home && !claim.home) {
      continue;
    }
    const bool earlier =
        !have || (claim.home && !have_home) || claim.t_claim < owner.t_claim ||
        (claim.t_claim == owner.t_claim &&
         (key.k0 < owner.race_key.k0 || (key.k0 == owner.race_key.k0 && key.k1 < owner.race_key.k1)));
    if (earlier) {
      have_home = have_home || claim.home;
      have = true;
      owner.owned = true;
      owner.candidate = i;
      owner.race_key = key;
      owner.t_claim = claim.t_claim;
      owner.source_index = claim.source_index;
    }
  }
  return owner;
}

// --- per body ---------------------------------------------------------------------

namespace {

struct BodyDraws {
  double settle_u;
  double delay_u;
  double flip_u;
  double abandon_u;
  double abandon_delay_u;
  double growth_n;
  double cap_n;
  double faction_noise[8];
};

BodyDraws body_draws(const BodyCivInputs& body, const Race& race, int faction_index) {
  const core::Key colony_key = core::derive_named(body.body_entity, name::ColonyV1);
  const std::int64_t rc = race_counter(race.key);
  const auto d0 = core::draw_point(colony_key, channel::Colony, rc, 0, 0);
  const auto d1 = core::draw_point(colony_key, channel::Colony, rc, faction_index + 1, 1);
  const auto d2 = core::draw_point(colony_key, channel::Colony, rc, 0, 2);
  const auto d3 = core::draw_point(colony_key, channel::Colony, rc, 0, 3);
  const auto d4 = core::draw_point(colony_key, channel::Colony, rc, 0, 4);
  BodyDraws d;
  d.settle_u = u01(d0[0]);
  d.delay_u = u01(d0[1]);
  d.flip_u = u01(d0[2]);
  d.abandon_u = u01(d0[3]);
  d.abandon_delay_u = u01(d2[0]);
  // Growth: log-normal keyed by (body, race, faction) — independent of
  // when the body was settled.
  d.growth_n = civ::normal01(d1[0], d1[1]);
  // max-level/v1: its own versioned key so neighbours/trade can enter a v2.
  const core::Key cap_key = core::derive_named(body.body_entity, name::MaxLevelV1);
  const auto c0 = core::draw_point(cap_key, channel::Colony, rc, faction_index + 1, 0);
  d.cap_n = civ::normal01(c0[0], c0[1]);
  for (int i = 0; i < 4; ++i) {
    d.faction_noise[i] = u01(d3[i]) * 2.0 - 1.0;
    d.faction_noise[i + 4] = u01(d4[i]) * 2.0 - 1.0;
  }
  return d;
}

// design section 11.3: which faction owns a body.
int pick_faction(const Race& race, const Dir3& x, double suitability_raw, bool body_hostile,
                 double frontier_at_founding, core::WorldTime t, const BodyDraws& draws,
                 double* score_out, int* runner_up) {
  const RaceParams& p = race.params;
  const double scale = p.r_max_ly * kLightYearM;
  const double lambda_gov = 0.35 * scale;
  const double lambda_ind = 0.6 * scale;
  double gov = 0.0;
  for (const FactionParams& f : race.factions) {
    if (f.type != FactionType::Government || t < f.t_start) continue;
    for (const FactionCentre& c : f.centres) {
      gov += c.weight * det::fast_exp(Real(-distance_m(x, c.position_m) / lambda_gov)).to_double();
    }
  }
  gov = clamp01(gov);
  // Hostility is about the BODY (airless, or only livable under a dome),
  // not about a marginal climate: android factions take the rocks and
  // ice, organic factions the breathable worlds.
  const double hostility = body_hostile ? 1.0 : 0.35 * (1.0 - clamp01(suitability_raw));
  // Frontier flavour: how close to the moving front the system was when
  // it was settled (1 = the wave itself, ~0 = deep infill). Fixed at
  // founding, so an outlaw hideout founded at the frontier stays one.
  const double frontier = 0.25 + 0.75 * frontier_at_founding;
  int best = -1;
  int second = -1;
  double best_score = 0.0;
  double second_score = 0.0;
  for (std::size_t j = 0; j < race.factions.size(); ++j) {
    const FactionParams& f = race.factions[j];
    if (t < f.t_start) continue;
    double nearest = 1.0e300;
    double weight = 1.0;
    for (const FactionCentre& c : f.centres) {
      const double d = distance_m(x, c.position_m);
      if (d < nearest) {
        nearest = d;
        weight = c.weight;
      }
    }
    double k = 0.0;
    switch (f.type) {
      case FactionType::Government: k = det::fast_exp(Real(-nearest / lambda_gov)).to_double(); break;
      case FactionType::Independent:
        k = det::fast_exp(Real(-nearest / lambda_ind)).to_double() * (1.0 - 0.5 * gov);
        break;
      case FactionType::Outlaw: k = (1.0 - gov) * frontier * (1.2 - clamp01(suitability_raw)); break;
      case FactionType::AlignedMachine: k = gov * hostility; break;
      case FactionType::RenegadeMachine: k = (1.0 - gov) * hostility; break;
      case FactionType::Count: break;
    }
    const double score = weight * k * (1.0 + 0.5 * draws.faction_noise[j % 8]);
    if (score > best_score) {
      second = best;
      second_score = best_score;
      best = static_cast<int>(j);
      best_score = score;
    } else if (score > second_score) {
      second = static_cast<int>(j);
      second_score = score;
    }
  }
  if (best < 0) {
    // Before any faction exists (an outpost founded in the race's first
    // days): the first Government faction by convention.
    for (std::size_t j = 0; j < race.factions.size(); ++j) {
      if (race.factions[j].type == FactionType::Government) return static_cast<int>(j);
    }
    return race.factions.empty() ? -1 : 0;
  }
  *score_out = best_score;
  *runner_up = second;
  // Contested flip: q = 0.5 (s2/s1)^2.
  if (second >= 0 && best_score > 0.0) {
    const double ratio = second_score / best_score;
    if (draws.flip_u < 0.5 * ratio * ratio) {
      return second;
    }
  }
  return best;
}

}  // namespace

CivState ColonyResolver::body_state(const SystemCivContext& context, const BodyCivInputs& body,
                                    const Owner& owner, bool is_best, core::WorldTime t) const {
  CivState state;
  if (!owner.owned) {
    return state;
  }
  const std::vector<Race>& candidates = registry_.candidates_around(context.position_m);
  if (owner.candidate >= candidates.size()) {
    return state;
  }
  const Race& race = candidates[owner.candidate];
  const RaceParams& p = race.params;
  const Source& source = p.sources[static_cast<std::size_t>(
      owner.source_index < static_cast<int>(p.sources.size()) ? owner.source_index : 0)];
  state.race_index = static_cast<std::uint32_t>(owner.candidate);
  state.race_key = race.key;
  const bool home_system = !race.void_home && race.home_system == context.cell;
  int human_home_slot = -1;
  if (p.is_human) {
    for (const BodyCivInputs& b : context.bodies) {
      if (b.moon < 0 && b.type == PlanetType::EarthLike) {
        human_home_slot = b.slot;
        break;
      }
    }
    if (human_home_slot < 0 && !context.bodies.empty()) human_home_slot = context.bodies[0].slot;
  }
  const bool is_home =
      home_system && (body.is_race_home || (p.is_human && body.moon < 0 && body.slot == human_home_slot));
  state.is_home = is_home;

  // 1. Suitability, organic and under a dome (race-level affinity).
  const double s_raw = suitability(p, body);
  const double s_dome_race = clamp01(p.dome_affinity) * dome_suitability(p, body);
  const double s_eff0 = s_raw > s_dome_race ? s_raw : s_dome_race;
  const BodyDraws base_draws = body_draws(body, race, 0);

  // 2. Settled? The home always; the best solid body of an owned system
  // always (domed if it must be); a breathable world with probability
  // 0.4 + 0.6 S; a domed outpost with probability S_eff itself.
  bool settle_ok = is_home || (is_best && body.solid && s_eff0 > 0.0);
  if (!settle_ok) {
    if (s_raw >= 0.1) {
      settle_ok = base_draws.settle_u < 0.4 + 0.6 * s_raw;
    } else if (s_eff0 >= 0.1) {
      settle_ok = base_draws.settle_u < s_eff0;
    }
  }
  if (!settle_ok) {
    state.suitability = s_eff0;
    return state;
  }

  // 3. Settlement instant: the claim plus a delay shrinking with
  // suitability; the faction's settle multiplier speeds it up.
  const double delay0_years = is_home ? 0.0 : (1.0 - clamp01(s_eff0)) * uniform_from(base_draws.delay_u, 0.1, 1.0);
  const core::WorldTime settled_base = core::WorldTime::from_ns(
      (is_home ? p.t_0 : owner.t_claim).ns_since_epoch + real_years_to_ns(delay0_years));

  // 4. The faction, drawn ONCE at founding (design section 11.3 evaluated
  // at the settlement instant): a faction that could change later would
  // change the colony's growth and let a level drop.
  double frontier_at_founding = 0.0;
  {
    const double front_ly = p.speed_ly_per_year * source.speed_scale *
                            ns_to_real_years(owner.t_claim.ns_since_epoch - source.t_source.ns_since_epoch);
    const double d_ly = distance_m(context.position_m, source.position_m) / kLightYearM;
    frontier_at_founding = front_ly > 1.0 ? clamp01(d_ly / front_ly) : 1.0;
    frontier_at_founding *= frontier_at_founding;
  }
  const bool body_hostile = !body.has_atmosphere || s_raw < 0.1;
  double score = 0.0;
  int runner_up = -1;
  int faction = is_home ? 0
                        : pick_faction(race, context.position_m, s_raw, body_hostile,
                                       frontier_at_founding, settled_base, base_draws, &score,
                                       &runner_up);
  if (faction < 0 && !race.factions.empty()) faction = 0;
  const FactionParams* fp = faction >= 0 && faction < static_cast<int>(race.factions.size())
                                ? &race.factions[static_cast<std::size_t>(faction)]
                                : nullptr;
  state.faction_index = faction;
  state.faction_type = fp != nullptr ? fp->type : FactionType::Government;
  const BodyDraws draws = fp != nullptr ? body_draws(body, race, faction) : base_draws;
  const double settle_mul = (fp != nullptr ? fp->settle_mul : 1.0) * source.settle_scale;
  const double delay_years = settle_mul > 0.0 ? delay0_years / settle_mul : delay0_years;
  state.settled_at = core::WorldTime::from_ns(
      (is_home ? p.t_0 : owner.t_claim).ns_since_epoch + real_years_to_ns(delay_years));

  // 5. The dome with the faction's affinity (tech vs naturalists).
  const double affinity = clamp01(p.dome_affinity * (fp != nullptr ? fp->dome_mul : 1.0));
  const double s_dome = affinity * dome_suitability(p, body);
  double s_eff = s_raw > s_dome ? s_raw : s_dome;
  state.domed = !is_home && s_raw < 0.1 && s_dome > 0.0;
  if (is_home) {
    s_eff = 1.0;
  }
  state.suitability = s_eff;
  if (t < state.settled_at) {
    return state;  // claimed, settlers en route
  }
  state.settled = true;
  state.age_s = ns_to_real_seconds(t.ns_since_epoch - state.settled_at.ns_since_epoch);

  // 6. Local wealth (max-level/v1) and local growth (colony/v1).
  {
    double star_bonus = 0.0;
    switch (context.star.cls) {
      case core::StellarClass::G:
      case core::StellarClass::K: star_bonus = 0.5; break;
      case core::StellarClass::F:
      case core::StellarClass::M: star_bonus = 0.0; break;
      default: star_bonus = -0.5; break;
    }
    star_bonus += 0.5 * context.metallicity;
    const double mu = 3.0 + 4.5 * s_eff + star_bonus;
    double cap = std::floor(mu + 0.9 * draws.cap_n + 0.5);
    cap = cap < 1.0 ? 1.0 : (cap > 7.0 ? 7.0 : cap);
    state.max_level = static_cast<int>(cap);
    if (state.domed && state.max_level > 3) {
      const core::Key cap_key = core::derive_named(body.body_entity, name::MaxLevelV1);
      const auto c1 = core::draw_point(cap_key, channel::Colony, race_counter(race.key), 0, 1);
      if (u01(c1[0]) >= 0.1) {
        state.max_level = 3;
      }
    }
    if (source.level_cap < state.max_level) state.max_level = source.level_cap;
    if (p.peak_level < state.max_level) state.max_level = p.peak_level;
    if (state.max_level == 7 && p.tech_tier < 4) state.max_level = 6;
    if (is_home) state.max_level = p.home_level > state.max_level ? p.home_level : state.max_level;
  }
  state.growth = det::fast_exp(Real(0.35 * draws.growth_n)).to_double();
  const double rate = p.reproduction * source.reproduction_scale *
                      (fp != nullptr ? fp->reproduction_mul : 1.0) * state.growth;

  // 7. The ladder, progress inside the level, ruins.
  int level = level_for_age(state.age_s, rate, state.domed);
  if (level > state.max_level) level = state.max_level;
  if (is_home) level = p.home_level;
  {
    const double t_here = time_to_level(level, rate, state.domed);
    const double t_next = level < 7 ? time_to_level(level + 1, rate, state.domed) : t_here + 5.0 * kRealYearS;
    const double span = t_next - t_here;
    state.progress = span > 0.0 ? clamp01((state.age_s - t_here) / span) : 1.0;
    if (state.progress >= 1.0) state.progress = 0.999999;
    if (is_home) state.progress = 0.999999;
  }
  if (p.extinct_at(t)) {
    state.ruined = true;
    const double age_at_end = ns_to_real_seconds(p.t_end.ns_since_epoch - state.settled_at.ns_since_epoch);
    if (age_at_end <= 0.0) {
      state.settled = false;  // the race died before the settlers landed
      return state;
    }
    int frozen = level_for_age(age_at_end, rate, state.domed);
    if (frozen > state.max_level) frozen = state.max_level;
    if (is_home) frozen = p.home_level;
    level = frozen;
    state.progress = 0.999999;
  } else if (!is_home && draws.abandon_u < 0.15 * (1.0 - clamp01(s_eff))) {
    const double t_cap = time_to_level(state.max_level, rate, state.domed);
    const double abandon_s = t_cap + uniform_from(draws.abandon_delay_u, 1.0, 5.0) * kRealYearS;
    if (state.age_s >= abandon_s) {
      state.ruined = true;
      level = state.max_level;
      state.progress = 0.999999;
    }
  }
  state.level = level;
  return state;
}

std::vector<CivState> ColonyResolver::system_states(const SystemCivContext& context,
                                                    const Owner& owner, core::WorldTime t) const {
  std::vector<CivState> states(context.bodies.size());
  if (!owner.owned) {
    return states;
  }
  const std::vector<Race>& candidates = registry_.candidates_around(context.position_m);
  if (owner.candidate >= candidates.size()) {
    return states;
  }
  const Race& race = candidates[owner.candidate];
  // The best-body rule: the most suitable solid body (dome option
  // included) is always settled.
  int best = -1;
  double best_s = 0.0;
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    const BodyCivInputs& body = context.bodies[i];
    if (!body.solid) continue;
    const double s_raw = suitability(race.params, body);
    const double s_dome = race.params.dome_affinity * dome_suitability(race.params, body);
    const double s = s_raw > s_dome ? s_raw : s_dome;
    if (s > best_s || best < 0) {
      best_s = s;
      best = static_cast<int>(i);
    }
  }
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    states[i] = body_state(context, context.bodies[i], owner, static_cast<int>(i) == best, t);
  }
  return states;
}

core::WorldTime ColonyResolver::next_change(const SystemCivContext& context,
                                            const BodyCivInputs& body, const Owner& owner,
                                            bool is_best, core::WorldTime t) const {
  // Bisection-free: evaluate the closed-form instants directly.
  const CivState now = body_state(context, body, owner, is_best, t);
  if (!owner.owned) {
    return t;
  }
  const std::vector<Race>& candidates = registry_.candidates_around(context.position_m);
  const Race& race = candidates[owner.candidate];
  const RaceParams& p = race.params;
  std::int64_t best_ns = 0;
  bool have = false;
  const auto consider = [&](std::int64_t ns) {
    if (ns > t.ns_since_epoch && (!have || ns < best_ns)) {
      best_ns = ns;
      have = true;
    }
  };
  if (!now.settled && now.settled_at.ns_since_epoch > t.ns_since_epoch) {
    consider(now.settled_at.ns_since_epoch);
  }
  if (now.settled && !now.ruined && !now.is_home) {
    const FactionParams* fp = now.faction_index >= 0 &&
                                      now.faction_index < static_cast<int>(race.factions.size())
                                  ? &race.factions[static_cast<std::size_t>(now.faction_index)]
                                  : nullptr;
    const Source& source = p.sources[static_cast<std::size_t>(
        owner.source_index < static_cast<int>(p.sources.size()) ? owner.source_index : 0)];
    const double rate = p.reproduction * source.reproduction_scale *
                        (fp != nullptr ? fp->reproduction_mul : 1.0) * now.growth;
    if (now.level < now.max_level) {
      const double t_next = time_to_level(now.level + 1, rate, now.domed);
      consider(now.settled_at.ns_since_epoch + static_cast<std::int64_t>(t_next * 1e9));
    }
  }
  if (p.extinct_ever && !p.extinct_at(t)) {
    consider(p.t_end.ns_since_epoch);
  }
  return have ? core::WorldTime::from_ns(best_ns) : t;
}

}  // namespace inf::gen
