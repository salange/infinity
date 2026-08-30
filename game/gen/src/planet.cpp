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

struct TypeTable {
  double radius_lo_m, radius_hi_m;
  double core_frac_lo, core_frac_hi;
  double sea_offset_lo_m, sea_offset_hi_m;  // 0/0 = no sea
  double atmo_lo_m, atmo_hi_m;              // 0/0 = no atmosphere
  std::uint32_t cells_lo, cells_hi;         // province grid N range
};

// Type changes data, never pipeline shape (spec section 5).
// Global 1:10 scale (planetary-systems spec, supersedes the 40-100 km v0
// ranges): Earth at 1:10 (637 km) is the EarthLike normal; variance from
// the seed. cells_lo/hi are now MINIMUMS — the province grid additionally
// scales with radius (see below).
constexpr std::array<TypeTable, 4> kTypeTables = {{
    // EarthLike
    {500'000.0, 800'000.0, 0.60, 0.75, 100.0, 500.0, 8'000.0, 15'000.0, 5, 7},
    // Barren
    {150'000.0, 400'000.0, 0.65, 0.80, 0.0, 0.0, 0.0, 0.0, 3, 5},
    // Desert
    {300'000.0, 650'000.0, 0.60, 0.78, 0.0, 0.0, 4'000.0, 8'000.0, 4, 6},
    // Ice
    {250'000.0, 550'000.0, 0.62, 0.78, 0.0, 0.0, 5'000.0, 9'000.0, 4, 6},
}};

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

PlanetParams derive_planet_params(const core::Key& body_key,
                                  std::optional<PlanetType> forced_type) {
  const core::Key params_key = core::derive_named(body_key, name::PlanetParamsV1);
  const auto draw0 = core::draw_point(params_key, channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(params_key, channel::Params, 1, 0, 0);
  const auto draw2 = core::draw_point(params_key, channel::Params, 2, 0, 0);

  PlanetParams params;
  params.type = forced_type.value_or(static_cast<PlanetType>(pick(draw0[0], 4)));
  const TypeTable& table = kTypeTables[static_cast<std::size_t>(params.type)];

  params.radius_m = uniform(draw0[1], table.radius_lo_m, table.radius_hi_m);
  params.core_radius_m = params.radius_m * uniform(draw0[2], table.core_frac_lo, table.core_frac_hi);
  // Gravity loosely follows radius (60 km reference ~ 9.8) with jitter.
  params.gravity =
      Real(9.81) * (params.radius_m / Real(637'000.0)) * uniform(draw0[3], 0.9, 1.1);
  params.sea_level_m = uniform(draw1[0], table.sea_offset_lo_m, table.sea_offset_hi_m);
  params.atmosphere_height_m = uniform(draw1[1], table.atmo_lo_m, table.atmo_hi_m);
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
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer),
                "  \"sky_palette\": %u,\n  \"cells_per_face\": %u,\n  \"palette_id\": %u\n",
                sky_palette, cells_per_face, palette_id);
  out += buffer;
  out += "}\n";
  return out;
}

}  // namespace inf::gen
