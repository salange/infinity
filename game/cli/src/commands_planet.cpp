// Planet inspection commands: dump-planet (JSON payloads), province-map
// (equirectangular PNGs), hash-planet (golden report). Visualization-only
// math (the equirect projection) may use platform trig — this is a
// cosmetic tool; all sampled values come from gen's deterministic API.

#include "commands_planet.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <stb_image_write.h>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/golden.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::cli {

namespace {

gen::BodyHandle body_for(const core::Seed128& seed) { return gen::default_body(seed); }

std::optional<gen::PlanetType> parse_type(const char* text) {
  if (text == nullptr) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto type = static_cast<gen::PlanetType>(i);
    if (std::strcmp(text, gen::to_string(type)) == 0) {
      return type;
    }
  }
  return std::nullopt;
}

struct Rgb {
  unsigned char r, g, b;
};

// One color family per archetype (index = Archetype value).
constexpr Rgb kArchetypeColors[] = {
    {110, 160, 90},   // Flats
    {140, 180, 100},  // RollingHills
    {150, 150, 160},  // Alpine
    {170, 120, 80},   // Canyon
    {130, 140, 90},   // HighlandPlateau
    {120, 115, 110},  // RegolithPlains
    {90, 85, 85},     // Cratered
    {150, 140, 130},  // Highlands
    {210, 170, 110},  // Dunes
    {190, 120, 80},   // Mesas
    {160, 90, 60},    // Canyonlands
    {220, 230, 240},  // GlacialShield
    {160, 200, 220},  // CrevasseField
    {120, 160, 200},  // RidgeField
};

gen::Dir3 latlon_dir(double lat, double lon) {
  const double cos_lat = std::cos(lat);
  return gen::Dir3{det::Real(cos_lat * std::cos(lon)), det::Real(cos_lat * std::sin(lon)),
                   det::Real(std::sin(lat))};
}

}  // namespace

int cmd_dump_planet(const core::Seed128& seed, const char* type_text) {
  const auto forced = parse_type(type_text);
  if (type_text != nullptr && !forced.has_value()) {
    std::fprintf(stderr, "unknown type: %s (EarthLike|Barren|Desert|Ice)\n", type_text);
    return 1;
  }
  const gen::BodyHandle body = body_for(seed);
  const gen::PlanetParams planet = gen::derive_planet_params(body.params, forced);
  const gen::ProvinceField field(body.entity, planet);
  std::printf("{\n\"planet\": %s,\n\"provinces\": %s}\n", planet.to_json().c_str(),
              field.table_to_json().c_str());
  return 0;
}

int cmd_province_map(const core::Seed128& seed, const char* type_text, const char* out_prefix) {
  const auto forced = parse_type(type_text);
  if (type_text != nullptr && !forced.has_value()) {
    std::fprintf(stderr, "unknown type: %s\n", type_text);
    return 1;
  }
  const gen::BodyHandle body = body_for(seed);
  const gen::PlanetParams planet = gen::derive_planet_params(body.params, forced);
  const gen::ProvinceField field(body.entity, planet);

  constexpr int kWidth = 768;
  constexpr int kHeight = 384;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<unsigned char> id_map(static_cast<std::size_t>(kWidth) * kHeight * 3);
  std::vector<unsigned char> relief_map(static_cast<std::size_t>(kWidth) * kHeight);

  // Relief scale for the grayscale map: max plausible amplitude.
  const double relief_scale = 2200.0;

  for (int y = 0; y < kHeight; ++y) {
    const double lat = kPi * (0.5 - (y + 0.5) / kHeight);
    for (int x = 0; x < kWidth; ++x) {
      const double lon = 2.0 * kPi * ((x + 0.5) / kWidth) - kPi;
      const gen::BlendedParams blended = field.sample(latlon_dir(lat, lon));

      const auto archetype_index = static_cast<std::size_t>(blended.dominant_archetype);
      Rgb color = kArchetypeColors[archetype_index];
      // Subtle per-province shading so distinct provinces of one archetype
      // stay tellable-apart.
      const gen::ProvinceParams cell = field.cell_params(blended.dominant);
      const int shade = static_cast<int>(cell.palette_shift % 33U) - 16;
      auto adjust = [&](unsigned char channel) {
        const int value = channel + shade;
        return static_cast<unsigned char>(value < 0 ? 0 : (value > 255 ? 255 : value));
      };
      const std::size_t pixel = (static_cast<std::size_t>(y) * kWidth + x);
      id_map[pixel * 3 + 0] = adjust(color.r);
      id_map[pixel * 3 + 1] = adjust(color.g);
      id_map[pixel * 3 + 2] = adjust(color.b);

      double relief = blended.relief_amplitude_m.to_double() / relief_scale;
      relief = relief < 0.0 ? 0.0 : (relief > 1.0 ? 1.0 : relief);
      relief_map[pixel] = static_cast<unsigned char>(relief * 255.0);
    }
  }

  const std::string id_name = std::string(out_prefix) + "-provinces.png";
  const std::string relief_name = std::string(out_prefix) + "-relief.png";
  if (stbi_write_png(id_name.c_str(), kWidth, kHeight, 3, id_map.data(), kWidth * 3) == 0 ||
      stbi_write_png(relief_name.c_str(), kWidth, kHeight, 1, relief_map.data(), kWidth) == 0) {
    std::fprintf(stderr, "failed to write PNGs (%s, %s)\n", id_name.c_str(), relief_name.c_str());
    return 1;
  }
  std::printf("wrote %s and %s (type=%s, N=%u)\n", id_name.c_str(), relief_name.c_str(),
              gen::to_string(planet.type), planet.cells_per_face);
  return 0;
}

int cmd_hash_planet() {
  std::fputs(gen::hash_planet_report().c_str(), stdout);
  return 0;
}

}  // namespace inf::cli
