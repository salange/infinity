#include "gen/planet.hpp"

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
constexpr std::array<TypeTable, 4> kTypeTables = {{
    // EarthLike
    {55'000.0, 85'000.0, 0.60, 0.75, 100.0, 500.0, 8'000.0, 15'000.0, 5, 7},
    // Barren
    {40'000.0, 70'000.0, 0.65, 0.80, 0.0, 0.0, 0.0, 0.0, 3, 5},
    // Desert
    {45'000.0, 80'000.0, 0.60, 0.78, 0.0, 0.0, 4'000.0, 8'000.0, 4, 6},
    // Ice
    {45'000.0, 75'000.0, 0.62, 0.78, 0.0, 0.0, 5'000.0, 9'000.0, 4, 6},
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
  const core::Key params_key = core::derive_named(body_key, core::NameId::PlanetParamsV1);
  const auto draw0 = core::draw_point(params_key, core::Channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(params_key, core::Channel::Params, 1, 0, 0);
  const auto draw2 = core::draw_point(params_key, core::Channel::Params, 2, 0, 0);

  PlanetParams params;
  params.type = forced_type.value_or(static_cast<PlanetType>(pick(draw0[0], 4)));
  const TypeTable& table = kTypeTables[static_cast<std::size_t>(params.type)];

  params.radius_m = uniform(draw0[1], table.radius_lo_m, table.radius_hi_m);
  params.core_radius_m = params.radius_m * uniform(draw0[2], table.core_frac_lo, table.core_frac_hi);
  // Gravity loosely follows radius (60 km reference ~ 9.8) with jitter.
  params.gravity =
      Real(9.81) * (params.radius_m / Real(60'000.0)) * uniform(draw0[3], 0.9, 1.1);
  params.sea_level_m = uniform(draw1[0], table.sea_offset_lo_m, table.sea_offset_hi_m);
  params.atmosphere_height_m = uniform(draw1[1], table.atmo_lo_m, table.atmo_hi_m);
  params.sky_palette = static_cast<std::uint32_t>(draw1[2] >> 40U);
  params.cells_per_face = table.cells_lo + pick(draw1[3], table.cells_hi - table.cells_lo + 1);
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
