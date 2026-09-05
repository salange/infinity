#include "gen/civilization.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/civ_names.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace civ {

double normal01(std::uint64_t word_a, std::uint64_t word_b) {
  const double u1 = u01(word_a) * (1.0 - 1.0e-12) + 1.0e-12;
  const double u2 = u01(word_b);
  const double radius = std::sqrt(-2.0 * det::fast_log(Real(u1)).to_double());
  Real sine(0.0);
  Real cosine(0.0);
  det::fast_sin_cos(Real(6.283185307179586 * u2), &sine, &cosine);
  return radius * cosine.to_double();
}

double log_uniform(std::uint64_t word, double lo, double hi) {
  const double ln_ratio = det::fast_log(Real(hi / lo)).to_double();
  return lo * det::fast_exp(Real(u01(word) * ln_ratio)).to_double();
}

std::uint32_t poisson(double lambda, std::uint64_t word_a, std::uint64_t word_b) {
  if (lambda <= 0.0) {
    return 0;
  }
  if (lambda < 24.0) {
    const double limit = det::fast_exp(Real(-lambda)).to_double();
    double product = u01(word_a);
    std::uint32_t count = 0;
    std::uint64_t state = word_b;
    while (product > limit && count < 128) {
      ++count;
      state = det::mix64(state + 0x9E3779B97F4A7C15ULL);
      product *= u01(state);
    }
    return count;
  }
  const double value = lambda + std::sqrt(lambda) * normal01(word_a, word_b);
  return value <= 0.0 ? 0U : static_cast<std::uint32_t>(value + 0.5);
}

std::size_t weighted_pick(std::uint64_t word, const double* weights, std::size_t count) {
  double total = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    total += weights[i] > 0.0 ? weights[i] : 0.0;
  }
  if (total <= 0.0) {
    return 0;
  }
  double roll = u01(word) * total;
  for (std::size_t i = 0; i < count; ++i) {
    const double w = weights[i] > 0.0 ? weights[i] : 0.0;
    if (roll < w) {
      return i;
    }
    roll -= w;
  }
  return count - 1;
}

}  // namespace civ

namespace {

using civ::pick;
using civ::u01;
using civ::uniform;

constexpr double kG = 9.81;

// design section 7.1: weights for a neutral galaxy, habitat preference,
// dome affinity prior, tech tier range, material family, layout families.
const RaceTypeInfo kTypeInfo[static_cast<int>(RaceType::Count)] = {
    // Humanoid
    {24.0, {{PlanetType::EarthLike, PlanetType::EarthLike}, 1, 270.0, 305.0, 0.6 * kG, 1.4 * kG,
            true, false, false, false},
     0.40, 2, 5, 0, LayoutFamily::Grid, LayoutFamily::Organic},
    // Insectoid
    {14.0, {{PlanetType::Desert, PlanetType::EarthLike}, 2, 285.0, 330.0, 0.7 * kG, 2.0 * kG,
            true, false, false, false},
     0.35, 1, 4, 2, LayoutFamily::Hive, LayoutFamily::Hive},
    // Reptilian
    {12.0, {{PlanetType::Desert, PlanetType::EarthLike}, 2, 290.0, 340.0, 0.7 * kG, 1.6 * kG,
            true, false, false, false},
     0.30, 1, 4, 0, LayoutFamily::Radial, LayoutFamily::Grid},
    // Avian
    {8.0, {{PlanetType::EarthLike, PlanetType::EarthLike}, 1, 265.0, 300.0, 0.4 * kG, 1.0 * kG,
           true, false, false, false},
     0.35, 2, 5, 0, LayoutFamily::Terraced, LayoutFamily::Organic},
    // Aquatic
    {8.0, {{PlanetType::EarthLike, PlanetType::EarthLike}, 1, 275.0, 305.0, 0.6 * kG, 1.4 * kG,
           true, true, false, false},
     0.25, 2, 4, 4, LayoutFamily::Linear, LayoutFamily::Organic},
    // Fungoid
    {8.0, {{PlanetType::EarthLike, PlanetType::Ice}, 2, 255.0, 300.0, 0.5 * kG, 1.5 * kG,
           true, false, false, false},
     0.15, 1, 3, 4, LayoutFamily::Organic, LayoutFamily::Organic},
    // Machine
    {8.0, {{PlanetType::Barren, PlanetType::Ice}, 2, 40.0, 500.0, 0.1 * kG, 3.0 * kG,
           false, false, true, false},
     0.90, 4, 5, 1, LayoutFamily::Lattice, LayoutFamily::Grid},
    // Crystalline
    {10.0, {{PlanetType::Barren, PlanetType::Ice}, 2, 40.0, 160.0, 0.3 * kG, 2.5 * kG,
            false, false, false, true},
     0.70, 2, 5, 3, LayoutFamily::Crystal, LayoutFamily::Crystal},
    // Precursor
    {8.0, {{PlanetType::EarthLike, PlanetType::Desert}, 2, 250.0, 340.0, 0.3 * kG, 2.5 * kG,
           false, false, true, false},
     0.50, 5, 5, 0, LayoutFamily::Radial, LayoutFamily::Grid},
};

bool is_organic(RaceType type) {
  return type != RaceType::Machine && type != RaceType::Crystalline &&
         type != RaceType::Precursor;
}

// HSV -> RGB with deterministic arithmetic only.
void hsv_to_rgb(double h, double s, double v, float out[3]) {
  h = h - std::floor(h);
  const double c = v * s;
  const double hp = h * 6.0;
  const double hm = hp - 2.0 * std::floor(hp * 0.5);
  const double x = c * (1.0 - std::fabs(hm - 1.0));
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  if (hp < 1.0) { r = c; g = x; }
  else if (hp < 2.0) { r = x; g = c; }
  else if (hp < 3.0) { g = c; b = x; }
  else if (hp < 4.0) { g = x; b = c; }
  else if (hp < 5.0) { r = x; b = c; }
  else { r = c; b = x; }
  const double m = v - c;
  out[0] = static_cast<float>(r + m);
  out[1] = static_cast<float>(g + m);
  out[2] = static_cast<float>(b + m);
}

// The greenhouse the climate model gives a type (climate.cpp): used to
// turn a preferred surface temperature into a preferred stellar flux.
double greenhouse_k(PlanetType type) {
  switch (type) {
    case PlanetType::EarthLike: return 33.0;
    case PlanetType::Desert: return 25.0;
    case PlanetType::Ice: return 25.0;
    case PlanetType::Barren: return 0.0;
  }
  return 0.0;
}

double preferred_flux(const Habitat& habitat, PlanetType type) {
  double t = 0.5 * (habitat.temp_lo_k + habitat.temp_hi_k);
  if (habitat.cryogenic) {
    t = 120.0;
  } else if (habitat.ignores_climate) {
    t = 200.0;
  }
  const double eq = (t - greenhouse_k(type)) / 255.0;
  const double f = eq > 0.15 ? eq : 0.15;
  return f * f * f * f;
}

}  // namespace

const RaceTypeInfo& race_type_info(RaceType type) {
  const auto index = static_cast<std::size_t>(type);
  return kTypeInfo[index < static_cast<std::size_t>(RaceType::Count) ? index : 0];
}

// --- civilization/v1 --------------------------------------------------------

CivilizationParams derive_civilization(const core::Key& galaxy_entity_key,
                                       const GalaxyParams& galaxy, bool force_home_minimum) {
  const core::Key key = core::derive_named(galaxy_entity_key, name::CivilizationV1);
  const auto d0 = core::draw_point(key, channel::Params, 0, 0, 0);
  CivilizationParams civ;
  const double roll = u01(d0[0]);
  if (roll < 0.04) {
    civ.race_count = 0;
  } else if (roll < 0.06) {
    civ.teeming = true;
    civ.race_count = 25 + pick(d0[1], 76);
  } else {
    // round(exp(Normal(ln 5, 0.6))) clamped 1-25: median 5, p90 ~ 11.
    const double ln_n = 1.6094379124341003 + 0.6 * civ::normal01(d0[1], d0[2]);
    double n = det::fast_exp(Real(ln_n)).to_double();
    n = std::floor(n + 0.5);
    civ.race_count = static_cast<std::uint32_t>(n < 1.0 ? 1.0 : (n > 25.0 ? 25.0 : n));
  }
  if (force_home_minimum && civ.race_count < 6) {
    civ.race_count = 6;
  }
  // L_civ: ~1 500 ly cells. Pure halving loop, no libm.
  {
    const double root_ly = galaxy.diameter_ly.to_double() * 1.1;
    double ratio = root_ly / 1500.0;
    int level = 0;
    // round(log2(ratio)): count doublings until ratio < sqrt(2).
    while (ratio >= 1.4142135623730951 && level < 9) {
      ratio *= 0.5;
      ++level;
    }
    civ.l_civ = level < 2 ? 2 : level;
    civ.cell_width_ly = root_ly / static_cast<double>(std::uint64_t{1} << civ.l_civ);
  }
  // Type tilt (design section 5): ellipticals toward Crystalline /
  // Precursor / Machine, young irregulars toward organics; age pushes
  // toward Precursors and extinction.
  for (int i = 0; i < static_cast<int>(RaceType::Count); ++i) {
    civ.type_weight[i] = kTypeInfo[i].base_weight;
  }
  const auto scale = [&](RaceType t, double f) {
    civ.type_weight[static_cast<int>(t)] *= f;
  };
  const auto scale_organics = [&](double f) {
    for (int i = 0; i < static_cast<int>(RaceType::Count); ++i) {
      if (is_organic(static_cast<RaceType>(i))) {
        civ.type_weight[i] *= f;
      }
    }
  };
  switch (galaxy.type) {
    case GalaxyType::Elliptical:
      scale(RaceType::Crystalline, 2.0);
      scale(RaceType::Machine, 1.3);
      scale(RaceType::Precursor, 2.0);
      scale_organics(0.6);
      break;
    case GalaxyType::Lenticular:
      scale(RaceType::Precursor, 1.5);
      scale(RaceType::Crystalline, 1.3);
      break;
    case GalaxyType::Irregular:
      scale_organics(1.3);
      scale(RaceType::Precursor, 0.5);
      scale(RaceType::Crystalline, 0.7);
      break;
    default: break;
  }
  const double age = galaxy.age_gyr.to_double();
  if (age > 9.0) {
    scale(RaceType::Precursor, 1.5);
  } else if (age < 3.0) {
    scale(RaceType::Precursor, 0.5);
  }
  civ.extinction_tilt = std::clamp((age - 4.0) / 8.0, 0.0, 1.0);
  return civ;
}

// --- races/v1 -----------------------------------------------------------------

RaceRegistry::RaceRegistry(const core::Key& galaxy_entity_key, const GalaxyParams& galaxy,
                           const CivilizationParams& civ)
    : races_key_(core::derive_named(galaxy_entity_key, name::RacesV1)),
      galaxy_(galaxy),
      civ_(civ),
      octree_(galaxy_entity_key, galaxy),
      home_position_m_(home_system_position_m(galaxy)) {}

Dir3 RaceRegistry::system_position_m(const SystemCell& cell) const {
  if (cell.is_home()) {
    return home_position_m_;
  }
  return octree_.system_position_m({cell.x, cell.y, cell.z, cell.level});
}

MacroCell RaceRegistry::macro_cell_of(const Dir3& p_m) const {
  const double inv = 1.0 / octree_.root_size_m();
  const double root_min = -0.5 * octree_.root_size_m();
  const auto scale = static_cast<double>(std::uint64_t{1} << civ_.l_civ);
  const auto axis = [&](double v) {
    double u = (v - root_min) * inv;
    u = u < 0.0 ? 0.0 : (u >= 1.0 ? 0.9999999999 : u);
    return static_cast<std::int64_t>(u * scale);
  };
  return MacroCell{axis(p_m.x.to_double()), axis(p_m.y.to_double()), axis(p_m.z.to_double()),
                   civ_.l_civ};
}

bool RaceRegistry::valid_cell(const MacroCell& cell) const {
  const std::int64_t extent = std::int64_t{1} << civ_.l_civ;
  return cell.level == civ_.l_civ && cell.x >= 0 && cell.x < extent && cell.y >= 0 &&
         cell.y < extent && cell.z >= 0 && cell.z < extent;
}

core::Key RaceRegistry::cell_key(const MacroCell& cell) const {
  return core::derive_child(races_key_, kind::MacroCell, cell.x, cell.y, cell.z, cell.level);
}

int RaceRegistry::home_count(const MacroCell& cell) const {
  if (civ_.race_count == 0 || !valid_cell(cell)) {
    return 0;
  }
  const double total = galaxy_.total_mass_suns.to_double();
  if (total <= 0.0) {
    return 0;
  }
  // Poisson thinning by stellar mass: the expectation over the galaxy is
  // exactly N; the bulge gets the most, the halo almost none.
  const double lambda =
      static_cast<double>(civ_.race_count) * octree_.expected_mass_suns(cell).to_double() / total;
  const auto d = core::draw_point(cell_key(cell), channel::Params, 0, 0, 0);
  const std::uint32_t n = civ::poisson(lambda, d[0], d[1]);
  return static_cast<int>(n > static_cast<std::uint32_t>(kMaxHomesPerCell)
                              ? static_cast<std::uint32_t>(kMaxHomesPerCell)
                              : n);
}

namespace {

void draw_factions(Race* race, const CivilizationParams& civ, double cell_width_ly);

}  // namespace

Race RaceRegistry::home(const MacroCell& cell, int index) const {
  Race race;
  race.cell = cell;
  race.index = index;
  race.key = core::derive_child(cell_key(cell), kind::Race, index);

  // --- placement (design section 6.4) ---------------------------------
  // Eight keyed candidate points in the cell, scored by the density model
  // (homes sit where stars are, i.e. toward the disc plane); take the
  // best whose octree leaf is occupied AND whose system position is still
  // inside this cell (coarse halo leaves can straddle cells — those homes
  // are void, which is right for the halo).
  {
    const double s = octree_.cell_size_m(cell.level);
    const Dir3 lo = octree_.cell_min_m(cell);
    struct Candidate {
      Dir3 p;
      double score;
    };
    Candidate candidates[8];
    for (int k = 0; k < 8; ++k) {
      const auto d = core::draw_point(race.key, channel::Params, 1, k, 0);
      const Dir3 p{lo.x + Real(u01(d[0]) * s), lo.y + Real(u01(d[1]) * s),
                   lo.z + Real(u01(d[2]) * s)};
      const double density = octree_.density().stars(p).to_double();
      candidates[k] = Candidate{p, density * (0.5 + 0.5 * u01(d[3]))};
    }
    std::sort(candidates, candidates + 8,
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    race.void_home = true;
    for (const Candidate& candidate : candidates) {
      const GalaxyOctree::CellId leaf = octree_.leaf_at(candidate.p);
      if (!octree_.occupied(leaf)) {
        continue;
      }
      const Dir3 pos = octree_.system_position_m(leaf);
      const MacroCell hosting = macro_cell_of(pos);
      if (hosting.x != cell.x || hosting.y != cell.y || hosting.z != cell.z) {
        continue;
      }
      race.home_system = SystemCell{leaf.x, leaf.y, leaf.z, leaf.level};
      race.void_home = false;
      break;
    }
    if (race.void_home) {
      return race;
    }
  }

  // --- properties (design section 7.2 a) ---------------------------------
  RaceParams& p = race.params;
  const auto d0 = core::draw_point(race.key, channel::Params, 2, 0, 0);
  const auto d1 = core::draw_point(race.key, channel::Params, 2, 1, 0);
  const auto d2 = core::draw_point(race.key, channel::Params, 2, 2, 0);
  const auto d3 = core::draw_point(race.key, channel::Params, 2, 3, 0);
  const auto d4 = core::draw_point(race.key, channel::Params, 2, 4, 0);
  p.type = static_cast<RaceType>(
      civ::weighted_pick(d0[0], civ_.type_weight, static_cast<std::size_t>(RaceType::Count)));
  const RaceTypeInfo& info = race_type_info(p.type);
  if (p.type == RaceType::Machine) {
    // A machine civilization was built by someone: organic creators, or
    // (rarely) the Precursors — the still-running ancient machines trope.
    double w[static_cast<int>(RaceType::Count)] = {};
    for (int i = 0; i < static_cast<int>(RaceType::Count); ++i) {
      const auto t = static_cast<RaceType>(i);
      w[i] = is_organic(t) ? civ_.type_weight[i] : (t == RaceType::Precursor ? 0.15 * civ_.type_weight[i] : 0.0);
    }
    p.parent_type = static_cast<RaceType>(
        civ::weighted_pick(d0[1], w, static_cast<std::size_t>(RaceType::Count)));
  } else {
    p.parent_type = p.type;
  }
  race_names(race.key, p.type, &p.name, &p.adjective);
  p.variant = static_cast<std::uint32_t>(d0[2] >> 32U);
  {
    const double hue = u01(d0[3]);
    const double hue2 = hue + uniform(d1[0], 0.3, 0.6);
    const double sat = p.type == RaceType::Machine ? 0.15 : (p.type == RaceType::Crystalline ? 0.7 : 0.45);
    hsv_to_rgb(hue, sat + 0.2 * u01(d1[1]), 0.55 + 0.35 * u01(d1[2]), p.palette[0]);
    hsv_to_rgb(hue2, sat, 0.35 + 0.35 * u01(d1[3]), p.palette[1]);
  }
  p.material_family = info.material_family;
  if (p.type == RaceType::Humanoid && u01(d2[0]) < 0.35) {
    p.material_family = 1;  // metal-and-glass humanoids
  }
  p.habitat = info.habitat;
  {
    const double jitter_t = uniform(d2[1], -6.0, 6.0);
    p.habitat.temp_lo_k += jitter_t;
    p.habitat.temp_hi_k += jitter_t;
    const double jitter_g = uniform(d2[2], -0.1, 0.1) * kG;
    p.habitat.gravity_lo = std::max(0.5, p.habitat.gravity_lo + jitter_g);
    p.habitat.gravity_hi = p.habitat.gravity_hi + jitter_g;
  }
  p.tech_tier = info.tech_lo + static_cast<int>(pick(d2[3], static_cast<std::uint32_t>(info.tech_hi - info.tech_lo + 1)));
  {
    // Aliens: 3-6, with a 3 % chance of a Trantorian-capable race;
    // Precursors always 7.
    static constexpr double kPeak[4] = {20.0, 35.0, 30.0, 15.0};
    p.peak_level = 3 + static_cast<int>(civ::weighted_pick(d3[0], kPeak, 4));
    if (u01(d3[1]) < 0.03) {
      p.peak_level = 7;
    }
    if (p.type == RaceType::Precursor) {
      p.peak_level = 7;
    }
    static constexpr double kHome[3] = {30.0, 45.0, 25.0};
    p.home_level = 4 + static_cast<int>(civ::weighted_pick(d3[2], kHome, 3));
    if (p.type == RaceType::Precursor) {
      p.home_level = 7;  // a ruined ecumenopolis
    }
    if (p.peak_level < p.home_level) {
      p.peak_level = p.home_level;
    }
  }
  p.dome_affinity = std::clamp(info.dome_prior + uniform(d3[3], -0.15, 0.15), 0.0, 1.0);
  p.disposition[0] = static_cast<float>(u01(d4[0]));
  p.disposition[1] = static_cast<float>(u01(d4[1]));
  p.disposition[2] = static_cast<float>(u01(d4[2]));

  // --- spread model (design section 7.2 b) --------------------------------
  const auto s0 = core::draw_point(race.key, channel::Params, 3, 0, 0);
  const auto s1 = core::draw_point(race.key, channel::Params, 3, 1, 0);
  const auto s2 = core::draw_point(race.key, channel::Params, 3, 2, 0);
  const double cell_width_ly = civ_.cell_width_ly;
  if (p.type == RaceType::Precursor) {
    // Long dead: gone 50-180 real years before launch, founded 20-80
    // years before that. (The design says 50-300; WorldTime spans +-292
    // years around 2000, so the extinction window is clipped to what the
    // clock can represent — in lore that is still 4 000-14 000 game-years.)
    const double gone_years = uniform(s0[0], 50.0, 180.0);
    const double span_years = uniform(s0[1], 20.0, 80.0);
    p.extinct_ever = true;
    p.t_end = core::WorldTime::from_ns(kLaunchReference.ns_since_epoch - real_years_to_ns(gone_years));
    p.t_0 = core::WorldTime::from_ns(p.t_end.ns_since_epoch - real_years_to_ns(span_years));
  } else {
    // Founding age at launch: log-uniform 8-60 real years — every alien
    // race is older than humanity's expansion (8 yr), so alien pockets
    // predate the human wave and hold (design section 7.2 b note).
    const double age_years = civ::log_uniform(s0[0], 8.0, 60.0);
    p.t_0 = core::WorldTime::from_ns(kLaunchReference.ns_since_epoch - real_years_to_ns(age_years));
    const double extinct_roll = u01(s0[1]);
    p.extinct_ever = extinct_roll < 0.25 * (0.6 + 0.8 * civ_.extinction_tilt);
    if (p.extinct_ever) {
      p.t_end = core::WorldTime::from_ns(p.t_0.ns_since_epoch +
                                         real_years_to_ns(uniform(s0[2], 1.0, 30.0)));
    }
  }
  p.speed_ly_per_year = civ::log_uniform(s0[3], 100.0, 1500.0);
  p.reproduction = uniform(s1[0], 0.3, 1.0);
  p.settle_prob = uniform(s1[1], 0.2, 0.8);
  const double r_cap = static_cast<double>(RaceRegistry::kReach) * cell_width_ly;
  p.r_max_ly = std::min(civ::log_uniform(s1[2], 50.0, r_cap), r_cap);
  p.falloff_ly = uniform(s1[3], 0.3, 1.0) * p.r_max_ly;
  p.anisotropy = uniform(s2[0], 0.0, 0.6);
  if (p.type == RaceType::Precursor) {
    p.speed_ly_per_year *= 2.0;
    p.r_max_ly = r_cap;
    p.falloff_ly = 0.8 * r_cap;
  }
  Source home;
  home.position_m = system_position_m(race.home_system);
  home.t_source = p.t_0;
  p.sources.push_back(home);
  // Faction count rises with age and peak level (1-6); Precursors none.
  {
    const double age_years = ns_to_real_years(kLaunchReference.ns_since_epoch - p.t_0.ns_since_epoch);
    int count = 1 + static_cast<int>(age_years / 15.0) + (p.peak_level >= 5 ? 1 : 0) +
                static_cast<int>(pick(s2[1], 2));
    p.faction_count = std::clamp(count, 1, 6);
    if (p.type == RaceType::Precursor) {
      p.faction_count = 0;
    }
  }
  draw_factions(&race, civ_, cell_width_ly);
  return race;
}

namespace {

// race-factions/v1 (design section 11.2).
void draw_factions(Race* race, const CivilizationParams& civ, double cell_width_ly) {
  (void)civ;
  (void)cell_width_ly;
  const RaceParams& p = race->params;
  const core::Key factions_key = core::derive_named(race->key, name::RaceFactionsV1);
  const bool machine_race = p.type == RaceType::Machine;
  const Dir3 home = p.sources.empty() ? Dir3{Real(0.0), Real(0.0), Real(0.0)}
                                      : p.sources[0].position_m;
  for (int j = 0; j < p.faction_count; ++j) {
    const core::Key key = core::derive_child(factions_key, kind::Faction, j);
    const auto d0 = core::draw_point(key, channel::Params, 0, 0, 0);
    const auto d1 = core::draw_point(key, channel::Params, 1, 0, 0);
    const auto d2 = core::draw_point(key, channel::Params, 2, 0, 0);
    FactionParams f;
    if (j == 0) {
      f.type = FactionType::Government;  // a race always has a polity
    } else {
      double w[static_cast<int>(FactionType::Count)] = {
          1.5 + p.disposition[0], 1.0 + p.disposition[1], 0.5 + p.disposition[2],
          machine_race ? 0.0 : (p.tech_tier >= 3 ? 0.3 + 0.1 * p.tech_tier : 0.0),
          machine_race ? 0.0 : (p.tech_tier >= 3 ? 0.2 + 0.5 * p.disposition[2] : 0.0)};
      f.type = static_cast<FactionType>(
          civ::weighted_pick(d0[0], w, static_cast<std::size_t>(FactionType::Count)));
    }
    f.name = faction_name(key, p.type, f.type, p.adjective, false);
    f.emblem_seed = static_cast<std::uint32_t>(d0[1] >> 32U);
    {
      const double shift = f.type == FactionType::Government ? 0.0 : uniform(d0[2], 0.1, 0.5);
      // Rotate the race's primary accent by `shift` in hue space.
      const float* base = p.palette[0];
      const double max_c = std::max(base[0], std::max(base[1], base[2]));
      const double min_c = std::min(base[0], std::min(base[1], base[2]));
      double hue = 0.0;
      if (max_c > min_c) {
        const double d = max_c - min_c;
        if (max_c == base[0]) hue = (base[1] - base[2]) / d / 6.0;
        else if (max_c == base[1]) hue = (2.0 + (base[2] - base[0]) / d) / 6.0;
        else hue = (4.0 + (base[0] - base[1]) / d) / 6.0;
      }
      hsv_to_rgb(hue + shift, 0.5, 0.8, f.accent);
    }
    // Start time: governments at founding, others within the race's
    // first decade.
    const double age_years = ns_to_real_years(kLaunchReference.ns_since_epoch - p.t_0.ns_since_epoch);
    const double delay = f.type == FactionType::Government
                             ? 0.0
                             : u01(d0[3]) * std::min(age_years * 0.6, 10.0);
    f.t_start = core::WorldTime::from_ns(p.t_0.ns_since_epoch + real_years_to_ns(delay));
    f.hostile = f.type == FactionType::RenegadeMachine;
    // Type multipliers (design section 11.1) with +-20 % jitter.
    static constexpr double kMul[5][4] = {
        {1.0, 1.2, 1.0, 1.0}, {0.8, 1.0, 1.2, 0.5}, {1.2, 0.6, 0.7, 0.8},
        {1.0, 1.3, 1.0, 1.5}, {1.3, 1.0, 0.8, 1.5}};
    const auto& m = kMul[static_cast<int>(f.type)];
    f.speed_mul = m[0] * uniform(d1[0], 0.8, 1.2);
    f.reproduction_mul = m[1] * uniform(d1[1], 0.8, 1.2);
    f.settle_mul = m[2] * uniform(d1[2], 0.8, 1.2);
    f.dome_mul = m[3] * uniform(d1[3], 0.8, 1.2);
    // Territorial centres: radial distance by type (governments near
    // home, outlaws far), random direction, 1-3 of them.
    static constexpr double kRadial[5][2] = {
        {0.0, 0.3}, {0.2, 0.7}, {0.5, 1.0}, {0.1, 0.5}, {0.6, 1.0}};
    const int centres = 1 + static_cast<int>(pick(d2[0], 3));
    for (int c = 0; c < centres; ++c) {
      const auto dc = core::draw_point(key, channel::Params, 3, c, 0);
      const double frac = uniform(dc[0], kRadial[static_cast<int>(f.type)][0],
                                  kRadial[static_cast<int>(f.type)][1]);
      // Uniform direction: z uniform in [-1, 1], azimuth uniform.
      const double z = uniform(dc[1], -1.0, 1.0);
      const double az = u01(dc[2]) * 6.283185307179586;
      const double rxy = std::sqrt(std::max(0.0, 1.0 - z * z));
      Real sine(0.0);
      Real cosine(0.0);
      det::fast_sin_cos(Real(az), &sine, &cosine);
      const double dist = frac * p.r_max_ly * kLightYearM;
      FactionCentre centre;
      centre.position_m = Dir3{home.x + Real(rxy * cosine.to_double() * dist),
                               home.y + Real(rxy * sine.to_double() * dist),
                               home.z + Real(z * dist * 0.3)};  // flattened toward the disc
      centre.weight = uniform(dc[3], 0.5, 1.0);
      f.centres.push_back(centre);
    }
    race->factions.push_back(std::move(f));
  }
}

}  // namespace

const std::vector<Race>& RaceRegistry::races_around(const MacroCell& center) const {
  if (block_valid_ && block_center_.x == center.x && block_center_.y == center.y &&
      block_center_.z == center.z && block_center_.level == center.level) {
    return block_;
  }
  block_.clear();
  if (civ_.race_count > 0) {
    for (std::int64_t dx = -kReach; dx <= kReach; ++dx) {
      for (std::int64_t dy = -kReach; dy <= kReach; ++dy) {
        for (std::int64_t dz = -kReach; dz <= kReach; ++dz) {
          const MacroCell cell{center.x + dx, center.y + dy, center.z + dz, civ_.l_civ};
          if (!valid_cell(cell)) {
            continue;
          }
          const int homes = home_count(cell);
          for (int i = 0; i < homes; ++i) {
            Race race = home(cell, i);
            if (!race.void_home) {
              block_.push_back(std::move(race));
            }
          }
        }
      }
    }
  }
  std::sort(block_.begin(), block_.end(), [](const Race& a, const Race& b) {
    return a.key.k0 != b.key.k0 ? a.key.k0 < b.key.k0 : a.key.k1 < b.key.k1;
  });
  block_center_ = center;
  block_valid_ = true;
  return block_;
}

std::optional<SystemOverride> RaceRegistry::home_override(const SystemCell& cell) const {
  if (cell.is_home() || civ_.race_count == 0) {
    return std::nullopt;  // the default system is never a race home
  }
  const MacroCell macro = macro_cell_of(system_position_m(cell));
  const int homes = home_count(macro);
  for (int i = 0; i < homes; ++i) {
    const Race race = home(macro, i);
    if (race.void_home || !(race.home_system == cell)) {
      continue;
    }
    SystemOverride over;
    const RaceParams& p = race.params;
    over.habitat = p.habitat.preferred[0];
    if (p.habitat.preferred_count > 1) {
      const auto d = core::draw_point(race.key, channel::Params, 4, 0, 0);
      over.habitat = p.habitat.preferred[pick(d[0], 2)];
    }
    over.preferred_flux = preferred_flux(p.habitat, over.habitat);
    over.force_biosphere = p.type != RaceType::Machine && p.type != RaceType::Precursor;
    over.race_key = race.key;
    return over;
  }
  return std::nullopt;
}

}  // namespace inf::gen
