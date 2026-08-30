#include "gen/system.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>

#include "core/ephem/ephemeris.hpp"
#include "core/golden.hpp"
#include "core/tree/tree.hpp"
#include "gen/names.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

using core::OrbitalElements;
using core::PlanetClass;
using core::PlanetPhys;
using core::SpinState;
using core::StarPhys;
using core::StellarClass;
using det::Real;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
// Game-scale constants (1:10 rule: lengths /10 AND mu /10).
constexpr double kAuGame = 1.495978707e10;        // 1 AU / 10, meters
constexpr double kMuSunGame = 1.32712440018e19;   // GM_sun / 10
constexpr double kEarthDayGame = 8640.0;          // 24 h / 10, seconds

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}
double u01d(std::uint64_t word) { return u01(word).to_double(); }
double uniform(std::uint64_t word, double lo, double hi) {
  return lo + (hi - lo) * u01d(word);
}
std::uint32_t pick(std::uint64_t word, std::uint32_t count) {
  return static_cast<std::uint32_t>((word >> 32U) % count);
}
// Weighted pick over a small table.
template <std::size_t N>
std::size_t weighted(std::uint64_t word, const std::array<int, N>& weights) {
  int total = 0;
  for (const int w : weights) total += w;
  int roll = static_cast<int>((word >> 32U) % static_cast<std::uint32_t>(total));
  for (std::size_t i = 0; i < N; ++i) {
    if (roll < weights[i]) return i;
    roll -= weights[i];
  }
  return N - 1;
}

// ---- stellar/v1 ---------------------------------------------------------

StarPhys draw_star(const core::Key& stellar_key) {
  const auto d0 = core::draw_point(stellar_key, channel::Params, 0, 0, 0);
  const auto d1 = core::draw_point(stellar_key, channel::Params, 1, 0, 0);

  // Class weights: gameplay-tilted IMF (G overweighted per the
  // Solar-System-overweight rule; remnants deferred).
  static constexpr std::array<int, 6> kClassWeights = {58, 18, 13, 6, 4, 1};
  static constexpr StellarClass kClasses[6] = {StellarClass::M, StellarClass::K,
                                               StellarClass::G, StellarClass::F,
                                               StellarClass::A, StellarClass::B};
  static constexpr double kMassRanges[6][2] = {{0.10, 0.50}, {0.50, 0.80}, {0.80, 1.10},
                                               {1.10, 1.50}, {1.50, 2.50}, {2.50, 8.00}};
  const std::size_t cls_index = weighted(d0[0], kClassWeights);

  StarPhys star{};
  star.cls = kClasses[cls_index];
  star.mass_solar = Real(uniform(d0[1], kMassRanges[cls_index][0], kMassRanges[cls_index][1]));
  star.age_gyr = Real(uniform(d0[2], 0.5, 9.0));
  // Metallicity ~ N(0, 0.2) via sum of three uniforms.
  star.metallicity =
      Real((u01d(d0[3]) + u01d(d1[0]) + u01d(d1[1]) - 1.5) * 0.4);

  // Main-sequence relations (piecewise L ~ M^a; DSP-style derived phys).
  const double m = star.mass_solar.to_double();
  const double lum = m < 0.43 ? 0.23 * det::sqrt(Real(m)).to_double() * m * m  // ~M^2.5
                              : m * m * m * det::sqrt(Real(m)).to_double();    // ~M^3.5
  star.luminosity_solar = Real(lum);
  // R ~ M^0.75 via sqrt chain: sqrt(M) * sqrt(sqrt(M)).
  const double r075 = det::sqrt(Real(m)).to_double() * det::sqrt(det::sqrt(Real(m))).to_double();
  star.radius_solar = Real(r075);
  // T = 5778 * (L / R^2)^(1/4).
  const double t4 = lum / (r075 * r075);
  star.temperature_k = Real(5778.0 * det::sqrt(det::sqrt(Real(t4))).to_double());
  star.mu = Real(kMuSunGame * m);
  return star;
}

// ---- architecture tables ------------------------------------------------

struct ArchetypeSpec {
  int planet_count_lo, planet_count_hi;
  double first_a_lo, first_a_hi;    // AU-scaled (game units of kAuGame)
  double ratio_lo, ratio_hi;        // orbit spacing ratio
  double e_scale;                   // eccentricity scale
};

constexpr ArchetypeSpec kArchetypeSpecs[6] = {
    /* SolarLike      */ {6, 9, 0.30, 0.45, 1.55, 1.90, 0.05},
    /* CompactMulti   */ {3, 7, 0.05, 0.10, 1.35, 1.60, 0.03},
    /* GiantDominated */ {2, 4, 0.80, 1.50, 1.90, 2.40, 0.08},
    /* SparseBarren   */ {1, 3, 0.40, 0.90, 1.80, 2.50, 0.10},
    /* HotJupiter     */ {1, 3, 0.03, 0.06, 2.20, 3.00, 0.05},
    /* Exotic         */ {2, 6, 0.10, 0.60, 1.50, 2.20, 0.15},
};

SystemArchetype draw_archetype(const core::Key& arch_key, StellarClass cls) {
  const auto d = core::draw_point(arch_key, channel::Archetype, 0, 0, 0);
  // FGK weights per the design table; M dwarfs reweight toward
  // CompactMulti/SparseBarren, giants rarer.
  if (cls == StellarClass::M) {
    static constexpr std::array<int, 6> kM = {10, 40, 3, 35, 1, 11};
    return static_cast<SystemArchetype>(weighted(d[0], kM));
  }
  static constexpr std::array<int, 6> kFgk = {35, 25, 10, 20, 2, 8};
  return static_cast<SystemArchetype>(weighted(d[0], kFgk));
}

// ---- planets/v1 ---------------------------------------------------------

PlanetClass draw_planet_class(std::uint64_t word, double a_game, double frost_line,
                              SystemArchetype archetype) {
  const bool beyond_frost = a_game > frost_line;
  const double roll = u01d(word);
  if (archetype == SystemArchetype::HotJupiter && a_game < 0.1 * kAuGame) {
    return PlanetClass::GasGiant;
  }
  if (beyond_frost) {
    if (archetype == SystemArchetype::SolarLike || archetype == SystemArchetype::GiantDominated) {
      if (roll < 0.45) return PlanetClass::GasGiant;
      if (roll < 0.80) return PlanetClass::IceGiant;
      return PlanetClass::Rocky;  // cold rocky/dwarf
    }
    if (roll < 0.25) return PlanetClass::IceGiant;
    if (roll < 0.40) return PlanetClass::SubNeptune;
    return PlanetClass::Rocky;
  }
  // Inside the frost line: radius-valley split; SolarLike favors rocky.
  const double sub_neptune_share = archetype == SystemArchetype::CompactMulti ? 0.45 : 0.20;
  if (roll < sub_neptune_share) return PlanetClass::SubNeptune;
  if (roll < sub_neptune_share + 0.25) return PlanetClass::SuperEarth;
  return PlanetClass::Rocky;
}

// Game-scale physical radius by class (1:10 global: Earth-analogue
// ~640 km; type ranges ~150-800 km — supersedes v0's 40-100 km).
double draw_radius_m(std::uint64_t word, PlanetClass cls) {
  switch (cls) {
    case PlanetClass::Rocky: return uniform(word, 250'000.0, 700'000.0);
    case PlanetClass::SuperEarth: return uniform(word, 600'000.0, 800'000.0);
    case PlanetClass::SubNeptune: return uniform(word, 650'000.0, 800'000.0);
    case PlanetClass::IceGiant: return uniform(word, 700'000.0, 800'000.0);
    case PlanetClass::GasGiant: return uniform(word, 750'000.0, 800'000.0);
  }
  return 400'000.0;
}

PlanetType surface_type_for(PlanetClass cls, double flux_rel, double radius_m,
                            std::uint64_t word) {
  if (cls == PlanetClass::GasGiant || cls == PlanetClass::IceGiant ||
      cls == PlanetClass::SubNeptune) {
    return PlanetType::Barren;  // not landable anyway
  }
  if (flux_rel > 2.2) {
    return PlanetType::Desert;
  }
  if (flux_rel < 0.30) {
    return PlanetType::Ice;
  }
  // Temperate band: EarthLike if big enough to hold an atmosphere.
  if (radius_m > 420'000.0) {
    return PlanetType::EarthLike;
  }
  return u01d(word) < 0.5 ? PlanetType::Barren : PlanetType::Desert;
}

}  // namespace

const char* to_string(SystemArchetype archetype) {
  switch (archetype) {
    case SystemArchetype::SolarLike: return "SolarLike";
    case SystemArchetype::CompactMulti: return "CompactMulti";
    case SystemArchetype::GiantDominated: return "GiantDominated";
    case SystemArchetype::SparseBarren: return "SparseBarren";
    case SystemArchetype::HotJupiter: return "HotJupiter";
    case SystemArchetype::Exotic: return "Exotic";
  }
  return "?";
}

StarSystemParams generate_system(const core::Key& system_entity_key) {
  StarSystemParams system;

  // stellar/v1
  const core::Key stellar_key = core::derive_named(system_entity_key, name::StellarV1);
  system.star = draw_star(stellar_key);
  const double mu_star = system.star.mu.to_double();
  const double lum = system.star.luminosity_solar.to_double();

  // disk/v1: frost line ~ 2.7 AU * sqrt(L), game scale.
  const core::Key disk_key = core::derive_named(system_entity_key, name::DiskV1);
  (void)disk_key;  // scaffold-only for now (mass factor folded into tables)
  const double frost_line = 2.7 * kAuGame * det::sqrt(Real(lum)).to_double();
  system.frost_line_m = Real(frost_line);

  // architecture/v1
  const core::Key arch_key = core::derive_named(system_entity_key, name::ArchitectureV1);
  system.archetype = draw_archetype(arch_key, system.star.cls);
  const ArchetypeSpec& spec = kArchetypeSpecs[static_cast<std::size_t>(system.archetype)];
  const auto arch_draw = core::draw_point(arch_key, channel::Params, 0, 0, 0);
  const int planet_count = spec.planet_count_lo +
                           static_cast<int>(pick(arch_draw[0],
                                                 static_cast<std::uint32_t>(
                                                     spec.planet_count_hi -
                                                     spec.planet_count_lo + 1)));

  // planets/v1
  const core::Key planets_key = core::derive_named(system_entity_key, name::PlanetsV1);
  system.planets.resize(kMaxPlanetSlots);
  double a = uniform(arch_draw[1], spec.first_a_lo, spec.first_a_hi) * kAuGame;
  bool any_landable = false;
  for (int slot = 0; slot < planet_count && slot < kMaxPlanetSlots; ++slot) {
    const auto d0 = core::draw_point(planets_key, channel::Params, slot, 0, 0);
    const auto d1 = core::draw_point(planets_key, channel::Params, slot, 1, 0);
    const auto d2 = core::draw_point(planets_key, channel::Params, slot, 2, 0);

    SystemPlanet planet;
    planet.occupied = true;
    planet.phys.cls = draw_planet_class(d0[0], a, frost_line, system.archetype);
    planet.phys.radius_m = Real(draw_radius_m(d0[1], planet.phys.cls));
    // Mass in Earth masses from radius/class (rough density families).
    const double r_rel = planet.phys.radius_m.to_double() / 637'000.0;
    const double mass_scale = planet.phys.cls == PlanetClass::GasGiant   ? 120.0
                              : planet.phys.cls == PlanetClass::IceGiant ? 16.0
                              : planet.phys.cls == PlanetClass::SubNeptune ? 6.0
                                                                            : 1.0;
    planet.phys.mass_earth = Real(mass_scale * r_rel * r_rel * r_rel);
    planet.phys.mu = Real(3.986004418e13 * planet.phys.mass_earth.to_double());  // GM_earth/10
    planet.phys.g_surface = Real(uniform(d0[2], 0.75, 1.25) * 9.81 * r_rel);

    // Hill-spacing enforcement (>= 10 mutual Hill radii; stability by
    // construction, never integrated).
    if (slot > 0) {
      // Find the previous occupied slot.
      int prev = slot - 1;
      const SystemPlanet& prior = system.planets[static_cast<std::size_t>(prev)];
      if (prior.occupied) {
        const double m_sum_solar = (prior.phys.mass_earth.to_double() +
                                    planet.phys.mass_earth.to_double()) *
                                   3.003e-6;  // Earth masses -> solar
        const double a_prev = prior.orbit.a_m.to_double();
        // Deterministic cbrt via fixed Newton iteration.
        const double target = m_sum_solar / (3.0 * system.star.mass_solar.to_double());
        double cbrt = 0.01;
        for (int it = 0; it < 24; ++it) {
          cbrt = (2.0 * cbrt + target / (cbrt * cbrt)) / 3.0;
        }
        // Fixpoint push: the required gap grows with the pushed orbit, so
        // iterate a fixed number of times (deterministic).
        for (int push = 0; push < 6; ++push) {
          const double mutual_hill = 0.5 * (a_prev + a) * cbrt;
          const double min_gap = 10.0 * mutual_hill;
          if (a - a_prev < min_gap) {
            a = a_prev + min_gap * 1.02;  // small margin over the limit
          }
        }
      }
    }

    // Orbital elements at Epoch Zero.
    planet.orbit.a_m = Real(a);
    // Triangular-ish small eccentricities scaled per archetype.
    planet.orbit.e =
        det::clamp(Real(spec.e_scale * (u01d(d1[0]) + u01d(d1[1]))), Real(0.0), Real(0.6));
    planet.orbit.i_rad = Real(uniform(d1[2], 0.0, 0.06));
    planet.orbit.raan_rad = Real(u01d(d1[3]) * kTwoPi);
    planet.orbit.argp_rad = Real(u01d(d2[0]) * kTwoPi);
    planet.orbit.mean_anom_0_rad = Real(u01d(d2[1]) * kTwoPi);
    planet.orbit.mu_parent = Real(mu_star);

    // Spin at Epoch Zero (game-scale days ~ 1-6 h; tidal-lock rule).
    const double a_au = a / kAuGame;
    const double flux = lum / (a_au * a_au);
    const double day_s = uniform(d2[2], 0.4, 2.5) * kEarthDayGame;
    planet.spin.spin_rate_rad_s = Real(kTwoPi / day_s);
    const double obliquity_roll = u01d(d2[3]);
    planet.spin.obliquity_rad =
        obliquity_roll < 0.85 ? Real(obliquity_roll / 0.85 * 0.52)  // 0-30 deg bulk
                              : Real((obliquity_roll - 0.85) / 0.15 * kPi);  // wild tail
    planet.spin.axis_azimuth_rad = Real(u01d(d0[3]) * kTwoPi);
    planet.spin.spin_phase_0_rad = Real(u01d(d1[0]) * kTwoPi);
    if (flux > 6.0) {
      planet.spin.tidally_locked = true;
      const double a3 = a * a * a;
      planet.spin.spin_rate_rad_s = Real(det::sqrt(Real(mu_star / a3)).to_double());
    }

    // Surface mapping for the surface generator.
    planet.landable = planet.phys.cls == PlanetClass::Rocky ||
                      planet.phys.cls == PlanetClass::SuperEarth;
    planet.surface_type = surface_type_for(planet.phys.cls, flux,
                                           planet.phys.radius_m.to_double(), d0[2]);
    planet.phys.surface_type = static_cast<std::uint32_t>(planet.surface_type);
    planet.phys.atmosphere.height_m =
        planet.landable && planet.surface_type != PlanetType::Barren
            ? Real(uniform(d1[3], 6'000.0, 15'000.0))
            : Real(0.0);
    planet.phys.atmosphere.pressure_rel = Real(uniform(d2[0], 0.4, 1.6));

    system.planets[static_cast<std::size_t>(slot)] = planet;
    // Next slot's provisional semi-major axis.
    a = a * uniform(d2[3], spec.ratio_lo, spec.ratio_hi);
    if (!planet.landable) {
      // keep scanning
    } else {
      any_landable = true;
    }
  }

  // Guarantee at least one landable body (default spawn contract): if the
  // draw produced none, re-class slot 0 as a temperate rocky world.
  if (!any_landable) {
    SystemPlanet& first = system.planets[0];
    if (!first.occupied) {
      first = SystemPlanet{};
      first.occupied = true;
      const auto d0 = core::draw_point(planets_key, channel::Params, 0, 0, 0);
      first.orbit.a_m = Real(1.0 * kAuGame);
      first.orbit.e = Real(0.02);
      first.orbit.mu_parent = Real(mu_star);
      first.orbit.mean_anom_0_rad = Real(u01d(d0[0]) * kTwoPi);
      first.spin.spin_rate_rad_s = Real(kTwoPi / kEarthDayGame);
    }
    first.phys.cls = PlanetClass::Rocky;
    first.phys.radius_m = Real(550'000.0);
    first.phys.g_surface = Real(9.0);
    first.landable = true;
    first.surface_type = PlanetType::EarthLike;
    first.phys.surface_type = static_cast<std::uint32_t>(PlanetType::EarthLike);
    first.phys.atmosphere.height_m = Real(10'000.0);
  }

  // moons/v1: recursive mini-systems within the Hill sphere.
  const core::Key moons_key = core::derive_named(system_entity_key, name::MoonsV1);
  for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
    SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
    if (!planet.occupied) continue;
    const auto md = core::draw_point(moons_key, channel::Params, slot, 0, 0);
    int moon_count = 0;
    switch (planet.phys.cls) {
      case PlanetClass::GasGiant: moon_count = 3 + static_cast<int>(pick(md[0], 4)); break;
      case PlanetClass::IceGiant: moon_count = 1 + static_cast<int>(pick(md[0], 4)); break;
      case PlanetClass::SubNeptune: moon_count = static_cast<int>(pick(md[0], 3)); break;
      default: moon_count = static_cast<int>(pick(md[0], 3));  // 0-2
    }
    // Hill radius (prograde stability limit ~ r_H/3).
    const double m_solar = planet.phys.mass_earth.to_double() * 3.003e-6;
    double cbrt = 0.01;
    const double target = m_solar / (3.0 * system.star.mass_solar.to_double());
    for (int it = 0; it < 24; ++it) {
      cbrt = (2.0 * cbrt + target / (cbrt * cbrt)) / 3.0;
    }
    const double hill_m = planet.orbit.a_m.to_double() * cbrt;
    const double moon_zone = hill_m / 3.0;
    double moon_a = std::max(planet.phys.radius_m.to_double() * 3.0, moon_zone * 0.02);
    for (int mi = 0; mi < moon_count; ++mi) {
      if (moon_a > moon_zone) break;
      const auto mdraw = core::draw_point(moons_key, channel::Params, slot, mi + 1, 0);
      SystemMoon moon;
      moon.phys.cls = PlanetClass::Rocky;
      moon.phys.radius_m = Real(uniform(mdraw[0], 120'000.0, 320'000.0));
      const double mr = moon.phys.radius_m.to_double() / 637'000.0;
      moon.phys.mass_earth = Real(mr * mr * mr);
      moon.phys.mu = Real(3.986004418e13 * moon.phys.mass_earth.to_double());
      moon.phys.g_surface = Real(uniform(mdraw[1], 0.6, 1.1) * 9.81 * mr);
      moon.phys.surface_type = static_cast<std::uint32_t>(
          u01d(mdraw[2]) < 0.6 ? PlanetType::Barren : PlanetType::Ice);
      moon.orbit.a_m = Real(moon_a);
      moon.orbit.e = Real(u01d(mdraw[3]) * 0.03);
      moon.orbit.i_rad = Real(u01d(mdraw[0]) * 0.05);
      moon.orbit.raan_rad = Real(u01d(mdraw[1]) * kTwoPi);
      moon.orbit.argp_rad = Real(u01d(mdraw[2]) * kTwoPi);
      moon.orbit.mean_anom_0_rad = Real(u01d(mdraw[3]) * kTwoPi);
      moon.orbit.mu_parent = planet.phys.mu;
      // Regular satellites: tidally locked to the parent.
      moon.spin.tidally_locked = true;
      const double ma3 = moon_a * moon_a * moon_a;
      moon.spin.spin_rate_rad_s =
          Real(det::sqrt(Real(planet.phys.mu.to_double() / ma3)).to_double());
      planet.moons.push_back(moon);
      moon_a = moon_a * uniform(mdraw[1], 1.8, 2.6);
    }
  }

  // belts/v1: SolarLike gets an inner belt near the frost line and an
  // outer Kuiper-analogue beyond the last planet.
  const core::Key belts_key = core::derive_named(system_entity_key, name::BeltsV1);
  const auto bd = core::draw_point(belts_key, channel::Params, 0, 0, 0);
  if (system.archetype == SystemArchetype::SolarLike ||
      system.archetype == SystemArchetype::SparseBarren) {
    SystemBelt inner;
    inner.inner_m = Real(frost_line * uniform(bd[0], 0.75, 0.9));
    inner.outer_m = Real(frost_line * uniform(bd[1], 1.0, 1.2));
    inner.thickness_m = Real(frost_line * 0.02);
    system.belts.push_back(inner);
  }
  double last_a = kAuGame;
  for (const SystemPlanet& planet : system.planets) {
    if (planet.occupied) last_a = std::max(last_a, planet.orbit.a_m.to_double());
  }
  SystemBelt outer;
  outer.inner_m = Real(last_a * uniform(bd[2], 1.5, 1.9));
  outer.outer_m = Real(last_a * uniform(bd[3], 2.2, 3.0));
  outer.thickness_m = Real(last_a * 0.05);
  system.belts.push_back(outer);

  return system;
}

int default_landable_slot(const StarSystemParams& system) {
  for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
    const SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
    if (planet.occupied && planet.landable) {
      return slot;
    }
  }
  return 0;  // unreachable: generation guarantees a landable body
}

PlanetParams planet_params_for_slot(const StarSystemParams& system, int slot,
                                    const core::Key& planet_params_key) {
  const SystemPlanet& sys_planet = system.planets[static_cast<std::size_t>(slot)];
  PlanetParams params = derive_planet_params(planet_params_key, sys_planet.surface_type);
  // System-layer truths override the standalone draws (one-directional
  // layering: planets/v1 decides, the surface generator obeys).
  params.radius_m = sys_planet.phys.radius_m;
  params.core_radius_m = params.radius_m * Real(0.7);
  params.gravity = sys_planet.phys.g_surface;
  params.atmosphere_height_m = sys_planet.phys.atmosphere.height_m;
  return params;
}


namespace {

void json_real(std::string* out, const char* key, Real value, bool comma = true) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "\"%s\": %.6g%s", key, value.to_double(),
                comma ? ", " : "");
  *out += buffer;
}

void json_orbit(std::string* out, const OrbitalElements& orbit) {
  *out += "{";
  json_real(out, "a_m", orbit.a_m);
  json_real(out, "e", orbit.e);
  json_real(out, "i_rad", orbit.i_rad);
  json_real(out, "raan_rad", orbit.raan_rad);
  json_real(out, "argp_rad", orbit.argp_rad);
  json_real(out, "M0_rad", orbit.mean_anom_0_rad);
  json_real(out, "mu_parent", orbit.mu_parent, false);
  *out += "}";
}

}  // namespace

std::string system_to_json(const StarSystemParams& system) {
  std::string out = "{\n\"star\": {";
  static constexpr const char* kClassNames[] = {"O", "B", "A", "F", "G", "K", "M",
                                                "WD", "NS", "BH"};
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "\"class\": \"%s\", ",
                kClassNames[static_cast<int>(system.star.cls)]);
  out += buffer;
  json_real(&out, "mass_solar", system.star.mass_solar);
  json_real(&out, "age_gyr", system.star.age_gyr);
  json_real(&out, "luminosity_solar", system.star.luminosity_solar);
  json_real(&out, "temperature_k", system.star.temperature_k);
  json_real(&out, "mu", system.star.mu, false);
  out += "},\n";
  std::snprintf(buffer, sizeof(buffer), "\"archetype\": \"%s\",\n",
                to_string(system.archetype));
  out += buffer;
  out += "\"frost_line_m\": ";
  std::snprintf(buffer, sizeof(buffer), "%.6g,\n", system.frost_line_m.to_double());
  out += buffer;
  out += "\"planets\": [\n";
  bool first_planet = true;
  for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
    const SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
    if (!planet.occupied) continue;
    if (!first_planet) out += ",\n";
    first_planet = false;
    static constexpr const char* kPlanetClassNames[] = {"Rocky", "SuperEarth", "SubNeptune",
                                                        "IceGiant", "GasGiant"};
    std::snprintf(buffer, sizeof(buffer),
                  " {\"slot\": %d, \"class\": \"%s\", \"surface\": \"%s\", "
                  "\"landable\": %s, \"moons\": %d, ",
                  slot, kPlanetClassNames[static_cast<int>(planet.phys.cls)],
                  planet.landable ? gen::to_string(planet.surface_type) : "-",
                  planet.landable ? "true" : "false",
                  static_cast<int>(planet.moons.size()));
    out += buffer;
    json_real(&out, "radius_m", planet.phys.radius_m);
    json_real(&out, "mass_earth", planet.phys.mass_earth);
    json_real(&out, "g_surface", planet.phys.g_surface);
    json_real(&out, "day_s", Real(planet.spin.spin_rate_rad_s.to_double() != 0.0
                                      ? kTwoPi / planet.spin.spin_rate_rad_s.to_double()
                                      : 0.0));
    out += "\"orbit\": ";
    json_orbit(&out, planet.orbit);
    out += "}";
  }
  out += "\n],\n\"belts\": [";
  for (std::size_t b = 0; b < system.belts.size(); ++b) {
    if (b > 0) out += ", ";
    out += "{";
    json_real(&out, "inner_m", system.belts[b].inner_m);
    json_real(&out, "outer_m", system.belts[b].outer_m, false);
    out += "}";
  }
  out += "]\n}\n";
  return out;
}

std::string ephemeris_table_json(const StarSystemParams& system, core::WorldTime start,
                                 std::int64_t step_ns, int steps) {
  std::string out = "[\n";
  for (int step = 0; step < steps; ++step) {
    const core::WorldTime t = start + step * step_ns;
    if (step > 0) out += ",\n";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), " {\"t_ns\": %lld, \"planets\": [",
                  static_cast<long long>(t.ns_since_epoch));
    out += buffer;
    bool first = true;
    for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
      const SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
      if (!planet.occupied) continue;
      const auto pv = core::Ephemeris::evaluate(planet.orbit, t);
      if (!first) out += ", ";
      first = false;
      std::snprintf(buffer, sizeof(buffer), "[%.6g, %.6g, %.6g]", pv.x.to_double(),
                    pv.y.to_double(), pv.z.to_double());
      out += buffer;
    }
    out += "]}";
  }
  out += "\n]\n";
  return out;
}

std::string hash_system_report() {
  static constexpr std::array<core::Seed128, 3> kSeeds = {
      core::Seed128{0, 1},
      core::Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},
      core::Seed128{0, 0xDEADBEEFULL},
  };
  // Fixed evaluation times (ManualClock semantics): Epoch Zero, +1 game
  // day, +1 game year, and 1000 s before the epoch.
  static constexpr std::int64_t kTimesNs[] = {0, 8'640'000'000'000LL,
                                              3'155'760'000'000'000LL, -1'000'000'000'000LL};
  std::string report = "hash-system v1\n";
  static constexpr char kDigits[] = "0123456789abcdef";
  for (const core::Seed128& seed : kSeeds) {
    const auto tree = make_tree(seed);
    const auto node = tree->get(default_system_address());
    const StarSystemParams system = generate_system(node->key());
    core::GoldenHash hash;
    hash.feed(static_cast<std::uint64_t>(system.archetype));
    hash.feed(std::bit_cast<std::uint64_t>(system.star.mass_solar.to_double()));
    hash.feed(std::bit_cast<std::uint64_t>(system.frost_line_m.to_double()));
    for (int slot = 0; slot < kMaxPlanetSlots; ++slot) {
      const SystemPlanet& planet = system.planets[static_cast<std::size_t>(slot)];
      if (!planet.occupied) continue;
      hash.feed(static_cast<std::uint64_t>(planet.phys.cls));
      hash.feed(std::bit_cast<std::uint64_t>(planet.phys.radius_m.to_double()));
      hash.feed(std::bit_cast<std::uint64_t>(planet.orbit.a_m.to_double()));
      hash.feed(static_cast<std::uint64_t>(planet.moons.size()));
      for (const std::int64_t t_ns : kTimesNs) {
        const auto pv =
            core::Ephemeris::evaluate(planet.orbit, core::WorldTime::from_ns(t_ns));
        hash.feed(std::bit_cast<std::uint64_t>(pv.x.to_double()));
        hash.feed(std::bit_cast<std::uint64_t>(pv.y.to_double()));
        hash.feed(std::bit_cast<std::uint64_t>(pv.vz.to_double()));
      }
    }
    report += "seed=" + core::to_hex(seed) + " fnv=";
    const std::uint64_t value = hash.value();
    for (int i = 15; i >= 0; --i) {
      report += kDigits[(value >> (i * 4)) & 0xFU];
    }
    report += "\n";
  }
  return report;
}

}  // namespace inf::gen

