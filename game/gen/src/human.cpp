#include "gen/human.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/trig.hpp"
#include "gen/civ_names.hpp"
#include "gen/names.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

using civ::pick;
using civ::u01;
using civ::uniform;
using det::Real;

namespace {

double galaxy_radius_ly(const GalaxyParams& galaxy) {
  return galaxy.diameter_ly.to_double() * 0.5;
}

RaceParams human_constants(const GalaxyParams& galaxy) {
  RaceParams p;
  p.type = RaceType::Humanoid;
  p.parent_type = RaceType::Humanoid;
  p.name = "Humans";
  p.adjective = "Human";
  p.variant = 0;
  p.palette[0][0] = 0.82f; p.palette[0][1] = 0.80f; p.palette[0][2] = 0.74f;  // warm concrete
  p.palette[1][0] = 0.25f; p.palette[1][1] = 0.45f; p.palette[1][2] = 0.70f;  // steel blue
  p.material_family = 1;  // metal and glass over stone
  p.habitat = race_type_info(RaceType::Humanoid).habitat;
  p.tech_tier = 4;
  p.peak_level = kHumanPeakLevel;
  p.home_level = kHumanHomeLevel;
  p.dome_affinity = kHumanDomeAffinity;
  p.disposition[0] = 0.6f;
  p.disposition[1] = 0.6f;
  p.disposition[2] = 0.4f;
  p.t_0 = kHumanExpansionStart;
  const double scale = galaxy_radius_ly(galaxy) / kHumanReferenceRadiusLy;
  p.speed_ly_per_year = kHumanSpeedRefLyPerYear * scale;
  p.reproduction = kHumanReproduction;
  p.settle_prob = kHumanSettleProb;
  p.r_max_ly = galaxy.diameter_ly.to_double() * 0.6;  // the whole galaxy, halo included
  p.falloff_ly = kHumanFalloffRefLy * scale;
  p.anisotropy = 0.2;
  p.extinct_ever = false;  // never
  p.is_human = true;
  return p;
}

void human_factions(Race* race, const core::Key& human_key, const GalaxyParams& galaxy) {
  const RaceParams& p = race->params;
  const core::Key factions_key = core::derive_named(human_key, name::RaceFactionsV1);
  const auto d = core::draw_point(factions_key, channel::Params, 0, 0, 0);
  // 1-2 Government, 2-3 Independent, 2-3 Outlaw, exactly one aligned and
  // one renegade android faction (design section 9).
  struct Plan {
    FactionType type;
    int count;
  };
  const Plan plan[5] = {
      {FactionType::Government, 1 + static_cast<int>(pick(d[0], 2))},
      {FactionType::Independent, 2 + static_cast<int>(pick(d[1], 2))},
      {FactionType::Outlaw, 2 + static_cast<int>(pick(d[2], 2))},
      {FactionType::AlignedMachine, 1},
      {FactionType::RenegadeMachine, 1},
  };
  static constexpr double kMul[5][4] = {
      {1.0, 1.2, 1.0, 1.0}, {0.8, 1.0, 1.2, 0.5}, {1.2, 0.6, 0.7, 0.8},
      {1.0, 1.3, 1.0, 1.5}, {1.3, 1.0, 0.8, 1.5}};
  static constexpr double kRadial[5][2] = {
      {0.0, 0.2}, {0.2, 0.6}, {0.5, 0.9}, {0.1, 0.4}, {0.6, 0.95}};
  const Dir3 home = p.sources[0].position_m;
  const double r_gal = galaxy_radius_ly(galaxy) * kLightYearM;
  int j = 0;
  for (const Plan& entry : plan) {
    for (int c = 0; c < entry.count; ++c, ++j) {
      const core::Key key = core::derive_child(factions_key, kind::Faction, j);
      const auto d0 = core::draw_point(key, channel::Params, 0, 0, 0);
      const auto d1 = core::draw_point(key, channel::Params, 1, 0, 0);
      FactionParams f;
      f.type = entry.type;
      f.name = faction_name(key, RaceType::Humanoid, f.type, p.adjective, true);
      f.emblem_seed = static_cast<std::uint32_t>(d0[0] >> 32U);
      {
        // Accent: governments blue-grey, independents ochre, outlaws
        // rust, aligned androids cool cyan, renegades amber-red.
        static constexpr float kAccent[5][3] = {{0.55f, 0.65f, 0.85f}, {0.85f, 0.70f, 0.40f},
                                                {0.70f, 0.35f, 0.25f}, {0.40f, 0.85f, 0.95f},
                                                {0.95f, 0.45f, 0.20f}};
        for (int k = 0; k < 3; ++k) {
          f.accent[k] = kAccent[static_cast<int>(f.type)][k] * (0.85f + 0.3f * static_cast<float>(u01(d0[1 + (k % 3)])));
        }
      }
      const bool android = f.type == FactionType::AlignedMachine || f.type == FactionType::RenegadeMachine;
      const double start_years =
          android ? kAndroidFactionStartYears
                  : (f.type == FactionType::Government ? 0.0 : u01(d0[2]) * 3.0);
      f.t_start = core::WorldTime::from_ns(p.t_0.ns_since_epoch + real_years_to_ns(start_years));
      f.hostile = f.type == FactionType::RenegadeMachine;
      const auto& m = kMul[static_cast<int>(f.type)];
      f.speed_mul = m[0] * uniform(d1[0], 0.8, 1.2);
      f.reproduction_mul = m[1] * uniform(d1[1], 0.8, 1.2);
      f.settle_mul = m[2] * uniform(d1[2], 0.8, 1.2);
      f.dome_mul = std::min(1.0, m[3] * uniform(d1[3], 0.8, 1.2));
      const int centres = 1 + static_cast<int>(pick(d0[3], 3));
      for (int k = 0; k < centres; ++k) {
        const auto dc = core::draw_point(key, channel::Params, 3, k, 0);
        const double frac = uniform(dc[0], kRadial[static_cast<int>(f.type)][0],
                                    kRadial[static_cast<int>(f.type)][1]);
        const double az = u01(dc[1]) * 6.283185307179586;
        Real sine(0.0);
        Real cosine(0.0);
        det::fast_sin_cos(Real(az), &sine, &cosine);
        const double dist = frac * r_gal;
        FactionCentre centre;
        centre.position_m = Dir3{home.x + Real(cosine.to_double() * dist),
                                 home.y + Real(sine.to_double() * dist),
                                 home.z + Real(uniform(dc[2], -0.02, 0.02) * dist)};
        centre.weight = uniform(dc[3], 0.5, 1.0);
        f.centres.push_back(centre);
      }
      race->factions.push_back(std::move(f));
    }
  }
  race->params.faction_count = j;
}

}  // namespace

Race human_race(const core::Key& home_galaxy_key, const GalaxyParams& galaxy) {
  Race race;
  race.key = core::derive_named(home_galaxy_key, name::HumanV1);
  race.cell = MacroCell{};  // not hosted by a macro cell: fixed home
  race.index = 0;
  race.home_system = SystemCell{};  // the default system
  race.void_home = false;
  race.params = human_constants(galaxy);
  Source home;
  home.position_m = home_system_position_m(galaxy);
  home.t_source = kHumanExpansionStart;
  race.params.sources.push_back(home);
  human_factions(&race, race.key, galaxy);
  return race;
}

std::vector<HumanEnclave> human_enclaves(const core::Seed128& seed, std::int64_t cx,
                                         std::int64_t cy, std::int64_t cz,
                                         std::uint32_t galaxy_index) {
  std::vector<HumanEnclave> out;
  const bool home_cluster = cx == 0 && cy == 0 && cz == 0;
  if (home_cluster && galaxy_index == 0) {
    return out;  // the home galaxy has the real thing
  }
  const core::Key galaxy_key = galaxy_key_in_cluster(seed, cx, cy, cz, galaxy_index);
  const GalaxyParams galaxy = derive_galaxy_params(galaxy_key);
  const core::Key key = core::derive_named(galaxy_key, name::HumanEnclavesV1);
  const auto d = core::draw_point(key, channel::Params, 0, 0, 0);
  const double p = home_cluster ? 0.30 : 0.002;
  if (u01(d[0]) >= p) {
    return out;
  }
  const int count = 1 + static_cast<int>(pick(d[1], 3));
  const double radius = galaxy.diameter_ly.to_double() * 0.5 * kLightYearM;
  const double height = galaxy.thin_scale_height_ly.to_double() * kLightYearM;
  const GalaxyParams home_galaxy = home_galaxy_params(seed);
  const double home_radius = home_galaxy.diameter_ly.to_double() * 0.5 * kLightYearM;
  for (int i = 0; i < count; ++i) {
    const auto e0 = core::draw_point(key, channel::Params, 1, i, 0);
    const auto e1 = core::draw_point(key, channel::Params, 2, i, 0);
    HumanEnclave enclave;
    enclave.index = static_cast<std::uint32_t>(i);
    // Beachhead: mid-disc point of this galaxy.
    {
      const double r = uniform(e0[0], 0.2, 0.8) * radius;
      const double az = u01(e0[1]) * 6.283185307179586;
      Real sine(0.0);
      Real cosine(0.0);
      det::fast_sin_cos(Real(az), &sine, &cosine);
      enclave.source.position_m = Dir3{Real(cosine.to_double() * r), Real(sine.to_double() * r),
                                       Real(uniform(e0[2], -1.0, 1.0) * height)};
    }
    // The gate era: within two real years of the expansion start.
    enclave.source.t_source = core::WorldTime::from_ns(
        kHumanExpansionStart.ns_since_epoch + real_years_to_ns(u01(e0[3]) * 2.0));
    // Stranded parameters (design section 9).
    enclave.source.speed_scale = 0.1;
    enclave.source.settle_scale = 0.3;
    enclave.source.reproduction_scale = 0.5;
    enclave.source.r_max_ly = uniform(e1[0], 100.0, 600.0);
    const double cap_roll = u01(e1[1]);
    enclave.source.level_cap = cap_roll < 0.60 ? 2 : (cap_roll < 0.95 ? 4 : 5 + static_cast<int>(pick(e1[2], 2)));
    // Dead gate partner: a mid-disc point of the home galaxy.
    {
      const auto g = core::draw_point(key, channel::Params, 3, i, 0);
      const double r = uniform(g[0], 0.15, 0.85) * home_radius;
      const double az = u01(g[1]) * 6.283185307179586;
      Real sine(0.0);
      Real cosine(0.0);
      det::fast_sin_cos(Real(az), &sine, &cosine);
      const double home_height = home_galaxy.thin_scale_height_ly.to_double() * kLightYearM;
      enclave.gate_partner_m = Dir3{Real(cosine.to_double() * r), Real(sine.to_double() * r),
                                    Real(uniform(g[2], -1.0, 1.0) * home_height)};
    }
    out.push_back(enclave);
  }
  return out;
}

Race human_race_in_galaxy(const core::Seed128& seed, std::int64_t cx, std::int64_t cy,
                          std::int64_t cz, std::uint32_t galaxy_index,
                          const GalaxyParams& galaxy) {
  if (cx == 0 && cy == 0 && cz == 0 && galaxy_index == 0) {
    return human_race(home_galaxy_key(seed), galaxy);
  }
  Race race;
  race.key = core::derive_named(galaxy_key_in_cluster(seed, cx, cy, cz, galaxy_index),
                                name::HumanV1);
  race.void_home = true;  // no home here: only beachheads
  race.params = human_constants(galaxy);
  for (const HumanEnclave& enclave : human_enclaves(seed, cx, cy, cz, galaxy_index)) {
    race.params.sources.push_back(enclave.source);
  }
  race.params.faction_count = 0;
  return race;
}

std::vector<WormholeGate> home_galaxy_gates(const core::Seed128& seed) {
  std::vector<WormholeGate> gates;
  const std::uint32_t count = galaxy_count_in_cluster(home_cluster_key(seed));
  for (std::uint32_t g = 1; g < count; ++g) {
    for (const HumanEnclave& enclave : human_enclaves(seed, 0, 0, 0, g)) {
      WormholeGate gate;
      gate.position_m = enclave.gate_partner_m;
      gate.partner_galaxy = g;
      gate.partner_enclave = enclave.index;
      gate.partner_position_m = enclave.source.position_m;
      gate.dead = true;
      gates.push_back(gate);
    }
  }
  return gates;
}

}  // namespace inf::gen
