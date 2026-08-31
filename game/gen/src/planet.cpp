#include "gen/planet.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace inf::gen {

using det::Real;

namespace {

// Uniform [0, 1) from a raw 64-bit draw: top 53 bits, exact in a double.
Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

Real uniform(std::uint64_t word, double lo, double hi) {
  return Real(lo) + Real(hi - lo) * u01(word);
}

std::uint32_t pick(std::uint64_t word, std::uint32_t count) {
  // Top bits, modulo — bias is negligible at these ranges (count <= dozens).
  return static_cast<std::uint32_t>((word >> 32U) % count);
}

// Cosmetic, per-surface-type columns ONLY. Radius deliberately does not
// live here any more (2026-08-31): it comes from radius_range_m(), the
// single class-keyed authority both this path and the system layer share.
// cells_lo/hi are MINIMUMS — the province grid additionally scales with
// radius (see below).
struct TypeTable {
  double core_frac_lo, core_frac_hi;
  double sea_offset_lo_m, sea_offset_hi_m;  // 0/0 = no sea
  double atmo_lo_m, atmo_hi_m;              // 0/0 = no atmosphere
  std::uint32_t cells_lo, cells_hi;         // province grid N range
};

// Type changes data, never pipeline shape (spec section 5).
constexpr std::array<TypeTable, 4> kTypeTables = {{
    // EarthLike
    {0.60, 0.75, 100.0, 500.0, 8'000.0, 15'000.0, 5, 7},
    // Barren
    {0.65, 0.80, 0.0, 0.0, 0.0, 0.0, 3, 5},
    // Desert
    {0.60, 0.78, 0.0, 0.0, 4'000.0, 8'000.0, 4, 6},
    // Ice
    {0.62, 0.78, 0.0, 0.0, 5'000.0, 9'000.0, 4, 6},
}};

// Class radius ranges in EARTH RADII, then scaled by kEarthRadiusGame.
// Straight 1:10 of the real ranges — the radius valley at ~1.8 R_earth
// splits Rocky/SuperEarth from SubNeptune, and the giants are finally at
// true scale (Jupiter = 10.97 R_earth = 6991 km at 1:10).
constexpr double kClassRadiiEarth[5][2] = {
    /* Rocky      */ {0.35, 1.40},
    /* SuperEarth */ {1.40, 1.80},
    /* SubNeptune */ {1.90, 3.50},
    /* IceGiant   */ {3.40, 4.60},
    /* GasGiant   */ {7.50, 12.50},
};

// Density families: mass = factor * r_rel^3 in Earth units. Calibrated so
// the Solar System lands where it should (Neptune ~17 M_earth, Jupiter
// ~318 M_earth, sub-Neptunes puffy at 2-15 M_earth).
constexpr double kClassMassFactor[5] = {1.00, 1.25, 0.35, 0.30, 0.24};

void append_real(std::string& out, const char* name, Real value, bool comma = true) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "\"%s\": %.6f%s", name, value.to_double(),
                comma ? "," : "");
  out += "  ";
  out += buffer;
  out += "\n";
}

}  // namespace

const char* to_string(PlanetType type) {
  switch (type) {
    case PlanetType::EarthLike: return "EarthLike";
    case PlanetType::Barren: return "Barren";
    case PlanetType::Desert: return "Desert";
    case PlanetType::Ice: return "Ice";
  }
  return "?";
}

RadiusRange radius_range_m(core::PlanetClass cls) {
  const auto index = static_cast<std::size_t>(cls);
  return RadiusRange{kClassRadiiEarth[index][0] * kEarthRadiusGame,
                     kClassRadiiEarth[index][1] * kEarthRadiusGame};
}

det::Real mass_earth_for(core::PlanetClass cls, det::Real radius_m) {
  const double r_rel = radius_m.to_double() / kEarthRadiusGame;
  return Real(kClassMassFactor[static_cast<std::size_t>(cls)] * r_rel * r_rel * r_rel);
}

det::Real surface_gravity(det::Real mass_earth, det::Real radius_m, double jitter) {
  // g = 9.81 * M / r^2 in Earth units. Physical in REAL terms, which is
  // exactly why it stays decoupled from the /10 game-scale mu used by the
  // ephemerides (spec section 4: two gravity domains, by design).
  const double r_rel = radius_m.to_double() / kEarthRadiusGame;
  const double g = 9.81 * mass_earth.to_double() / (r_rel * r_rel) * jitter;
  // Clamp LAST so the jitter can never push a body past the playable band
  // (spec section 4: authored g in roughly 2-25 m/s^2).
  return Real(std::clamp(g, 1.0, 25.0));
}

core::PlanetClass class_for_surface_type(PlanetType type, std::uint64_t word) {
  const double roll = u01(word).to_double();
  switch (type) {
    case PlanetType::EarthLike: return roll < 0.60 ? core::PlanetClass::Rocky
                                                   : core::PlanetClass::SuperEarth;
    case PlanetType::Desert: return roll < 0.70 ? core::PlanetClass::Rocky
                                                : core::PlanetClass::SuperEarth;
    case PlanetType::Ice: return roll < 0.80 ? core::PlanetClass::Rocky
                                             : core::PlanetClass::SuperEarth;
    case PlanetType::Barren: break;
  }
  return core::PlanetClass::Rocky;  // airless rock/moon; giants come via the system
}

PlanetParams derive_planet_params(const core::Key& body_entity_key,
                                  const core::Key& params_key_root,
                                  std::optional<PlanetType> forced_type) {
  const core::Key params_key = core::derive_named(params_key_root, name::PlanetParamsV1);
  const auto draw0 = core::draw_point(params_key, channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(params_key, channel::Params, 1, 0, 0);
  const auto draw2 = core::draw_point(params_key, channel::Params, 2, 0, 0);
  const auto draw3 = core::draw_point(params_key, channel::Params, 3, 0, 0);

  PlanetParams params;
  params.type = forced_type.value_or(static_cast<PlanetType>(pick(draw0[0], 4)));
  const TypeTable& table = kTypeTables[static_cast<std::size_t>(params.type)];

  // Radius comes from the shared class table; EarthLike additionally
  // needs enough mass to hold air, so its low end clamps to the same
  // threshold the system layer's surface-type mapping uses.
  const core::PlanetClass cls = class_for_surface_type(params.type, draw2[1]);
  RadiusRange range = radius_range_m(cls);
  if (params.type == PlanetType::EarthLike) {
    range.lo_m = std::max(range.lo_m, kAtmosphereMinRadiusM);
  }
  params.radius_m = uniform(draw0[1], range.lo_m, range.hi_m);
  params.core_radius_m = params.radius_m * uniform(draw0[2], table.core_frac_lo, table.core_frac_hi);
  params.gravity = surface_gravity(mass_earth_for(cls, params.radius_m), params.radius_m,
                                   uniform(draw0[3], 0.9, 1.1).to_double());
  params.atmosphere_height_m = uniform(draw1[1], table.atmo_lo_m, table.atmo_hi_m);

  // --- macro/v1: continents + the SOLVED sea level (T0015 WP1) ---------
  // The continent pattern comes from the macro layer's own key; the
  // water inventory is an independent draw here (orthogonal axes). The
  // sea level is the macro-elevation quantile at (1 - land) — that is
  // what makes the measured land fraction track the target.
  const MacroField macro(body_entity_key);
  params.macro_pattern = macro.pattern();
  params.macro_amplitude_m = params.radius_m * macro_amplitude_fraction(macro.pattern());
  switch (params.type) {
    case PlanetType::EarthLike:
      // EarthLike means "has water" by definition: temperate band.
      params.land_fraction = uniform(draw3[0], 0.15, 0.45);
      break;
    case PlanetType::Ice: {
      // Sheet worlds span the full range: frozen-dry through global sheet.
      const double roll = u01(draw3[1]).to_double();
      if (roll < 0.18) {
        params.land_fraction = Real(1.0);
      } else if (roll < 0.30) {
        params.land_fraction = uniform(draw3[0], 0.0, 0.05);
      } else {
        params.land_fraction = uniform(draw3[0], 0.10, 0.50);
      }
      break;
    }
    case PlanetType::Desert:
    case PlanetType::Barren:
      params.land_fraction = Real(1.0);  // dry: basins are dry lowlands
      break;
  }
  // The province layer rides on top of macro with a positive mean base
  // elevation, which lifts land above the pure-macro quantile. Correct
  // the solved sea level by the type's EXPECTED province base (weights x
  // range midpoints of the archetype table) so measured land tracks the
  // target. Calibrated against `infinity-cli macro-stats`.
  constexpr double kExpectedProvinceBase[4] = {402.0, 202.0, 184.0, 329.0};
  const Real base_correction =
      Real(kExpectedProvinceBase[static_cast<std::size_t>(params.type)] * 1.05);
  params.sea_level_m =
      macro.solve_sea_level(params.land_fraction) * params.macro_amplitude_m +
      (params.land_fraction.to_double() < 0.999 ? base_correction : Real(0.0));
  (void)table.sea_offset_lo_m;  // superseded by the solve; table kept for layout
  (void)table.sea_offset_hi_m;
  params.sky_palette = static_cast<std::uint32_t>(draw1[2] >> 40U);
  // Province grid scales with radius: target province size ~60-120 km
  // (Earth/10 country scale), plus seeded variance within the type range.
  const double face_edge_km = 1.57 * params.radius_m.to_double() / 1000.0;
  const auto scaled = static_cast<std::uint32_t>(face_edge_km / 90.0);
  const std::uint32_t base = table.cells_lo + pick(draw1[3], table.cells_hi - table.cells_lo + 1);
  params.cells_per_face = std::clamp(scaled + base - table.cells_lo, base, 24U);
  params.palette_id = static_cast<std::uint32_t>(draw2[0] >> 40U);
  return params;
}

std::string PlanetParams::to_json() const {
  std::string out = "{\n";
  out += "  \"type\": \"";
  out += gen::to_string(type);
  out += "\",\n";
  append_real(out, "radius_m", radius_m);
  append_real(out, "core_radius_m", core_radius_m);
  append_real(out, "gravity", gravity);
  append_real(out, "sea_level_m", sea_level_m);
  append_real(out, "atmosphere_height_m", atmosphere_height_m);
  out += "  \"macro_pattern\": \"";
  out += gen::to_string(macro_pattern);
  out += "\",\n";
  append_real(out, "land_fraction", land_fraction);
  append_real(out, "macro_amplitude_m", macro_amplitude_m);
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer),
                "  \"sky_palette\": %u,\n  \"cells_per_face\": %u,\n  \"palette_id\": %u\n",
                sky_palette, cells_per_face, palette_id);
  out += buffer;
  out += "}\n";
  return out;
}

}  // namespace inf::gen
