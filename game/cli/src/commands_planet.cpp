// Planet inspection commands: dump-planet (JSON payloads), province-map
// (equirectangular PNGs), hash-planet (golden report). Visualization-only
// math (the equirect projection) may use platform trig — this is a
// cosmetic tool; all sampled values come from gen's deterministic API.

#include "commands_planet.hpp"

#include <algorithm>
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
#include "gen/terrain.hpp"

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
  const gen::PlanetParams planet = gen::derive_planet_params(body, forced);
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
  const gen::PlanetParams planet = gen::derive_planet_params(body, forced);
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

int cmd_terrain_map(const core::Seed128& seed, const char* type_text, const char* out_prefix) {
  const auto forced = parse_type(type_text);
  if (type_text != nullptr && !forced.has_value()) {
    std::fprintf(stderr, "unknown type: %s\n", type_text);
    return 1;
  }
  const gen::BodyHandle body = body_for(seed);
  const gen::PlanetParams planet = gen::derive_planet_params(body, forced);
  const gen::TerrainField field(body.entity, planet);
  const double sea = planet.sea_level_m.to_double();
  const double amp = planet.macro_amplitude_m.to_double();

  constexpr int kWidth = 1024;
  constexpr int kHeight = 512;
  constexpr double kPi = 3.14159265358979323846;
  std::vector<unsigned char> map(static_cast<std::size_t>(kWidth) * kHeight * 3);
  std::vector<double> heights(static_cast<std::size_t>(kWidth) * kHeight);
  double lo = 1e30;
  double hi = -1e30;
  double land_area = 0.0;
  double total_area = 0.0;

  gen::TerrainField::ParamCache cache;
  for (int y = 0; y < kHeight; ++y) {
    const double lat = kPi * (0.5 - (y + 0.5) / kHeight);
    const double weight = std::cos(lat);
    for (int x = 0; x < kWidth; ++x) {
      const double lon = 2.0 * kPi * ((x + 0.5) / kWidth) - kPi;
      const gen::Dir3 dir = latlon_dir(lat, lon);
      const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir), &cache);
      gen::BlendedParams params = gen::TerrainField::to_blended(canonical);
      const double h =
          field.elevation_from_params(dir, params, canonical.macro_rel).to_double();
      heights[static_cast<std::size_t>(y) * kWidth + x] = h;
      lo = std::min(lo, h);
      hi = std::max(hi, h);
      // Area-weighted land fraction (equirect oversamples the poles).
      if (h > sea) {
        land_area += weight;
      }
      total_area += weight;
    }
  }

  // Color relative to the measured span so dry worlds (sea below the
  // minimum) still show relief instead of clipping to peak white.
  const double land_base = std::max(sea, lo);
  const double land_scale = std::max(1.0, std::min(hi - land_base, 2.5 * amp));
  const double ocean_scale = std::max(1.0, std::min(land_base - lo, 2.5 * amp));
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const double h = heights[static_cast<std::size_t>(y) * kWidth + x];
      const double rel = h - sea;
      Rgb color{};
      if (rel < 0.0) {
        // Ocean: deeper = darker blue.
        const double depth = std::min(1.0, -rel / ocean_scale);
        color.r = static_cast<unsigned char>(20.0 + 30.0 * (1.0 - depth));
        color.g = static_cast<unsigned char>(50.0 + 70.0 * (1.0 - depth));
        color.b = static_cast<unsigned char>(110.0 + 100.0 * (1.0 - depth));
      } else {
        // Land: shore green -> highland brown -> peak white.
        const double t = std::min(1.0, (h - land_base) / land_scale);
        if (t < 0.5) {
          const double s = t / 0.5;
          color.r = static_cast<unsigned char>(70.0 + s * 90.0);
          color.g = static_cast<unsigned char>(130.0 - s * 30.0);
          color.b = static_cast<unsigned char>(60.0 + s * 20.0);
        } else {
          const double s = (t - 0.5) / 0.5;
          color.r = static_cast<unsigned char>(160.0 + s * 90.0);
          color.g = static_cast<unsigned char>(100.0 + s * 150.0);
          color.b = static_cast<unsigned char>(80.0 + s * 170.0);
        }
      }
      const std::size_t pixel = (static_cast<std::size_t>(y) * kWidth + x) * 3;
      map[pixel + 0] = color.r;
      map[pixel + 1] = color.g;
      map[pixel + 2] = color.b;
    }
  }

  const std::string name = std::string(out_prefix) + "-terrain.png";
  if (stbi_write_png(name.c_str(), kWidth, kHeight, 3, map.data(), kWidth * 3) == 0) {
    std::fprintf(stderr, "failed to write %s\n", name.c_str());
    return 1;
  }
  std::printf(
      "wrote %s  type=%s pattern=%s target_land=%.3f measured_land=%.3f sea=%.0fm "
      "span=[%.0f, %.0f]m\n",
      name.c_str(), gen::to_string(planet.type), gen::to_string(planet.macro_pattern),
      planet.land_fraction.to_double(), land_area / total_area, sea, lo, hi);
  return 0;
}

int cmd_macro_stats(int seed_count) {
  int pattern_counts[4] = {0, 0, 0, 0};
  double worst = 0.0;
  double mean_abs = 0.0;
  int measured = 0;
  constexpr double kPi = 3.14159265358979323846;
  for (int s = 1; s <= seed_count; ++s) {
    const gen::BodyHandle body = body_for(core::Seed128{0, static_cast<std::uint64_t>(s)});
    const gen::PlanetParams planet =
        gen::derive_planet_params(body, gen::PlanetType::EarthLike);
    ++pattern_counts[static_cast<int>(planet.macro_pattern)];
    const gen::TerrainField field(body.entity, planet);
    const double sea = planet.sea_level_m.to_double();
    // Independent direction set (not the solve set): 64x32 lat/lon,
    // cos(lat)-weighted.
    gen::TerrainField::ParamCache cache;
    double land = 0.0;
    double total = 0.0;
    for (int y = 0; y < 32; ++y) {
      const double lat = kPi * (0.5 - (y + 0.5) / 32.0);
      const double weight = std::cos(lat);
      for (int x = 0; x < 64; ++x) {
        const double lon = 2.0 * kPi * ((x + 0.5) / 64.0) - kPi;
        const gen::Dir3 dir = latlon_dir(lat, lon);
        const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir), &cache);
        gen::BlendedParams params = gen::TerrainField::to_blended(canonical);
        const double h =
            field.elevation_from_params(dir, params, canonical.macro_rel).to_double();
        if (h > sea) {
          land += weight;
        }
        total += weight;
      }
    }
    const double error = land / total - planet.land_fraction.to_double();
    mean_abs += std::abs(error);
    worst = std::max(worst, std::abs(error));
    ++measured;
    std::printf("seed=%d pattern=%-14s target=%.3f measured=%.3f err=%+.3f\n", s,
                gen::to_string(planet.macro_pattern), planet.land_fraction.to_double(),
                land / total, error);
  }
  std::printf("patterns: Supercontinent=%d FewContinents=%d Archipelago=%d Fractured=%d\n",
              pattern_counts[0], pattern_counts[1], pattern_counts[2], pattern_counts[3]);
  std::printf("land-fraction error: mean=%.4f worst=%.4f over %d seeds\n",
              mean_abs / measured, worst, measured);
  return 0;
}

int cmd_terrain_stats(const core::Seed128& seed) {
  // T0015 WP4 acceptance: mean |dz| per horizontal run at walking scales.
  const gen::BodyHandle body = body_for(seed);
  for (std::uint32_t t = 0; t < 4; ++t) {
    const auto type = static_cast<gen::PlanetType>(t);
    const gen::PlanetParams planet = gen::derive_planet_params(body, type);
    const gen::TerrainField field(body.entity, planet);
    const double radius = planet.radius_m.to_double();
    gen::TerrainField::ParamCache cache;
    const int kSteps = 1200;
    const double step_m = 5.0;
    const double step_rad = step_m / radius;
    double sum5 = 0.0, sum20 = 0.0, sum100 = 0.0;
    int n5 = 0, n20 = 0, n100 = 0;
    std::vector<double> h(kSteps);
    for (int arc = 0; arc < 3; ++arc) {
      const double a = 0.7 + arc * 1.9;
      gen::Dir3 d{det::Real(std::cos(a)), det::Real(std::sin(a)), det::Real(0.2 * arc)};
      const double dl = std::sqrt(d.x.to_double() * d.x.to_double() +
                                  d.y.to_double() * d.y.to_double() +
                                  d.z.to_double() * d.z.to_double());
      gen::Dir3 dir{det::Real(d.x.to_double() / dl), det::Real(d.y.to_double() / dl),
                    det::Real(d.z.to_double() / dl)};
      // Tangent via cross with z-ish axis.
      double tx = -dir.y.to_double(), ty = dir.x.to_double(), tz = 0.0;
      const double tl = std::sqrt(tx * tx + ty * ty + tz * tz);
      tx /= tl; ty /= tl;
      for (int i = 0; i < kSteps; ++i) {
        const double ang = step_rad * i;
        const double c = std::cos(ang), sn = std::sin(ang);
        const double px = dir.x.to_double() * c + tx * sn;
        const double py = dir.y.to_double() * c + ty * sn;
        const double pz = dir.z.to_double() * c + tz * sn;
        const double pl = std::sqrt(px * px + py * py + pz * pz);
        const gen::Dir3 pd{det::Real(px / pl), det::Real(py / pl), det::Real(pz / pl)};
        const auto canonical = field.canonical_params(gen::dir_to_face_uv(pd), &cache);
        gen::BlendedParams params = gen::TerrainField::to_blended(canonical);
        // Full surface height incl. the 3D detail term at the surface.
        const gen::Dir3 pos{det::Real(pd.x.to_double() * radius),
                            det::Real(pd.y.to_double() * radius),
                            det::Real(pd.z.to_double() * radius)};
        h[static_cast<std::size_t>(i)] =
            field.elevation_from_params(pd, params, canonical.macro_rel).to_double() +
            field.detail_m(pos).to_double();
      }
      for (int i = 1; i < kSteps; ++i) { sum5 += std::abs(h[i] - h[i - 1]); ++n5; }
      for (int i = 4; i < kSteps; i += 4) { sum20 += std::abs(h[i] - h[i - 4]); ++n20; }
      for (int i = 20; i < kSteps; i += 20) { sum100 += std::abs(h[i] - h[i - 20]); ++n100; }
    }
    std::printf("%-10s mean|dz|: 5m=%.2f  20m=%.2f  100m=%.2f\n", gen::to_string(type),
                sum5 / n5, sum20 / n20, sum100 / n100);
  }
  return 0;
}

}  // namespace inf::cli

// --- T0019: surface maps and tile dumps -----------------------------------

#include "gen/climate.hpp"
#include "gen/life.hpp"
#include "gen/material.hpp"
#include "tex/tiles.hpp"

namespace inf::cli {

namespace {

// Distinct colours for material ids (index = Material value).
constexpr Rgb kMaterialColors[] = {
    {0, 0, 0},          // None
    {130, 125, 120},    // RockGranite
    {50, 48, 48},       // RockBasalt
    {160, 120, 85},     // RockSandstone
    {105, 105, 110},    // RockShale
    {120, 110, 100},    // Scree
    {95, 110, 80},      // CliffMossy
    {135, 130, 122},    // RegolithFine
    {110, 105, 98},     // RegolithRubble
    {120, 115, 108},    // Gravel
    {140, 128, 115},    // Pebbles
    {220, 190, 130},    // SandDune
    {205, 185, 140},    // SandBeach
    {120, 105, 85},     // SandWet
    {170, 140, 100},    // SoilDry
    {90, 70, 50},       // SoilMud
    {110, 85, 60},      // SoilLoam
    {85, 70, 45},       // ForestFloor
    {150, 105, 60},     // DeadLeaves
    {70, 140, 50},      // Grass
    {110, 150, 70},     // Meadow
    {60, 120, 60},      // Moss
    {240, 242, 248},    // Snow
    {230, 235, 245},    // SnowDrift
    {180, 178, 175},    // SnowDirty
    {170, 205, 230},    // IceSheet
    {160, 160, 165},    // Permafrost
    {40, 25, 20},       // LavaRock
    {160, 90, 170},     // MicrobialMat
    {170, 170, 110},    // LichenCrust
    {120, 220, 230},    // CrystalField
    {230, 200, 50},     // Sulfur
    {150, 100, 55},     // TholinDust
    {160, 70, 40},      // RedBed
    {235, 230, 220},    // SaltFlat
    {130, 170, 185},    // AmmoniaSlush
    {95, 95, 75},       // Seabed
};
static_assert(sizeof(kMaterialColors) / sizeof(kMaterialColors[0]) == gen::kMaterialCount,
              "material colour table out of sync");

constexpr Rgb kBiomeColors[] = {
    {225, 225, 235},  // PolarDesert
    {150, 170, 150},  // Tundra
    {40, 90, 60},     // BorealForest
    {180, 190, 90},   // TemperateGrassland
    {60, 130, 50},    // TemperateForest
    {20, 100, 70},    // TemperateRainforest
    {160, 150, 80},   // Shrubland
    {200, 170, 70},   // Savanna
    {230, 200, 120},  // HotDesert
    {70, 150, 40},    // TropicalSeasonalForest
    {20, 120, 30},    // TropicalRainforest
    {140, 140, 150},  // Alpine
};

}  // namespace

int cmd_surface_map(const core::Seed128& seed, const char* type_text, const char* out_prefix) {
  const auto forced = parse_type(type_text);
  if (type_text != nullptr && !forced.has_value()) {
    std::fprintf(stderr, "unknown type: %s\n", type_text);
    return 1;
  }
  const gen::BodyHandle body = body_for(seed);
  const gen::PlanetParams planet = gen::derive_planet_params(body, forced);
  const gen::TerrainField field(body.entity, planet);
  const double radius = planet.radius_m.to_double();

  constexpr int kWidth = 1024;
  constexpr int kHeight = 512;
  constexpr double kPi = 3.14159265358979323846;
  const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
  std::vector<unsigned char> climate_map(pixels * 3);
  std::vector<unsigned char> biome_map(pixels * 3);
  std::vector<unsigned char> material_map(pixels * 3);
  std::vector<unsigned char> life_map(pixels * 3);
  std::vector<double> heights(pixels);
  std::uint64_t material_histogram[gen::kMaterialCount] = {};

  gen::TerrainField::ParamCache cache;
  // Heights first so the material pass can use a real slope from the
  // grid (like the far-view baker).
  for (int y = 0; y < kHeight; ++y) {
    const double lat = kPi * (0.5 - (y + 0.5) / kHeight);
    for (int x = 0; x < kWidth; ++x) {
      const double lon = 2.0 * kPi * ((x + 0.5) / kWidth) - kPi;
      const gen::Dir3 dir = latlon_dir(lat, lon);
      const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir), &cache);
      const gen::BlendedParams params = gen::TerrainField::to_blended(canonical);
      heights[static_cast<std::size_t>(y) * kWidth + x] =
          field.elevation_from_params(dir, params, canonical.macro_rel, &cache).to_double();
    }
  }
  for (int y = 0; y < kHeight; ++y) {
    const double lat = kPi * (0.5 - (y + 0.5) / kHeight);
    const double cos_lat = std::max(0.05, std::cos(lat));
    for (int x = 0; x < kWidth; ++x) {
      const double lon = 2.0 * kPi * ((x + 0.5) / kWidth) - kPi;
      const gen::Dir3 dir = latlon_dir(lat, lon);
      const std::size_t pixel = static_cast<std::size_t>(y) * kWidth + x;
      const double h = heights[pixel];
      // Grid slope -> a tilted normal (the classifier only reads the tilt).
      const double arc_x = radius * 2.0 * kPi / kWidth * cos_lat;
      const double arc_y = radius * kPi / kHeight;
      const double hx = heights[static_cast<std::size_t>(y) * kWidth + (x + 1) % kWidth] -
                        heights[static_cast<std::size_t>(y) * kWidth + (x + kWidth - 1) % kWidth];
      const double hy = heights[static_cast<std::size_t>(std::min(y + 1, kHeight - 1)) * kWidth + x] -
                        heights[static_cast<std::size_t>(std::max(y - 1, 0)) * kWidth + x];
      const double su = hx / (2.0 * arc_x);
      const double sv = hy / (2.0 * arc_y);
      const double tilt = std::sqrt(su * su + sv * sv);
      const double inv = 1.0 / std::sqrt(1.0 + tilt * tilt);
      const double dx = dir.x.to_double();
      const double dy = dir.y.to_double();
      const double dz = dir.z.to_double();
      double nx = dx * inv;
      double ny = dy * inv;
      double nz = dz * inv + tilt * inv;
      const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= len;
      ny /= len;
      nz /= len;
      const double r = radius + h;
      const gen::MaterialInputs in =
          field.material_inputs(dx * r, dy * r, dz * r, nx, ny, nz, &cache);
      const gen::VertexMaterial vm = field.material().classify(in);

      // Climate: red = temperature (200..320 K), green = humidity, blue = frozen.
      climate_map[pixel * 3 + 0] =
          static_cast<unsigned char>(std::clamp((in.climate.temperature_k - 200.0) / 120.0, 0.0, 1.0) * 255.0);
      climate_map[pixel * 3 + 1] = static_cast<unsigned char>(in.climate.humidity * 255.0);
      climate_map[pixel * 3 + 2] = in.climate.frozen ? 200 : 30;
      const Rgb bc = kBiomeColors[static_cast<std::size_t>(in.biome.primary)];
      const Rgb bs = kBiomeColors[static_cast<std::size_t>(in.biome.secondary)];
      const double bb = in.biome.blend;
      biome_map[pixel * 3 + 0] = static_cast<unsigned char>(bc.r + (bs.r - bc.r) * bb);
      biome_map[pixel * 3 + 1] = static_cast<unsigned char>(bc.g + (bs.g - bc.g) * bb);
      biome_map[pixel * 3 + 2] = static_cast<unsigned char>(bc.b + (bs.b - bc.b) * bb);
      const Rgb m0 = kMaterialColors[static_cast<std::size_t>(vm.mat0)];
      const Rgb m1 = kMaterialColors[static_cast<std::size_t>(vm.mat1)];
      const double mb = vm.blend;
      const bool sea = planet.type == gen::PlanetType::EarthLike &&
                       h < planet.sea_level_m.to_double();
      material_map[pixel * 3 + 0] = sea ? 20 : static_cast<unsigned char>(m0.r + (m1.r - m0.r) * mb);
      material_map[pixel * 3 + 1] = sea ? 40 : static_cast<unsigned char>(m0.g + (m1.g - m0.g) * mb);
      material_map[pixel * 3 + 2] = sea ? 90 : static_cast<unsigned char>(m0.b + (m1.b - m0.b) * mb);
      ++material_histogram[static_cast<std::size_t>(vm.mat0)];
      const double patch = 0.5;
      const double cover = gen::life_coverage(field.life(), in.climate, in.slope,
                                              in.height_above_sea_m, patch);
      life_map[pixel * 3 + 0] = static_cast<unsigned char>(cover * 255.0);
      life_map[pixel * 3 + 1] = static_cast<unsigned char>(in.climate.t01 * 255.0);
      life_map[pixel * 3 + 2] = static_cast<unsigned char>(in.climate.h01 * 255.0);
    }
  }
  const std::string names[4] = {std::string(out_prefix) + "-climate.png",
                                std::string(out_prefix) + "-biome.png",
                                std::string(out_prefix) + "-material.png",
                                std::string(out_prefix) + "-life.png"};
  const std::vector<unsigned char>* maps[4] = {&climate_map, &biome_map, &material_map, &life_map};
  for (int i = 0; i < 4; ++i) {
    if (stbi_write_png(names[i].c_str(), kWidth, kHeight, 3, maps[i]->data(), kWidth * 3) == 0) {
      std::fprintf(stderr, "failed to write %s\n", names[i].c_str());
      return 1;
    }
  }
  const gen::LifeParams& life = field.life();
  std::printf("type=%s radius=%.0f flux=%.2f star=%.0fK age=%.1fGyr meanT=%.1fK meanH=%.2f\n",
              gen::to_string(planet.type), radius, planet.flux_rel.to_double(),
              planet.star_temperature_k.to_double(), planet.star_age_gyr.to_double(),
              field.climate().mean_temperature_k(), field.climate().mean_humidity());
  std::printf("life: habitable=%d occupied=%d chemistry=%s stage=%s\n", life.habitable ? 1 : 0,
              life.occupied ? 1 : 0, gen::to_string(life.chemistry), gen::to_string(life.stage));
  std::printf("materials (primary, %% of map):");
  for (std::uint32_t m = 1; m < gen::kMaterialCount; ++m) {
    if (material_histogram[m] > 0) {
      std::printf(" %s=%.1f", gen::material_info(static_cast<gen::Material>(m)).name,
                  100.0 * static_cast<double>(material_histogram[m]) / static_cast<double>(pixels));
    }
  }
  std::printf("\nwrote %s %s %s %s\n", names[0].c_str(), names[1].c_str(), names[2].c_str(),
              names[3].c_str());
  return 0;
}

int cmd_tile_dump(const char* out_dir, int size) {
  std::size_t count = 0;
  const char* const* names = tex::known_tile_names(&count);
  for (std::size_t i = 0; i < count; ++i) {
    const tex::Tile tile = tex::generate_tile(names[i], static_cast<std::uint32_t>(size), 0x51EDULL + i);
    const std::string albedo_name = std::string(out_dir) + "/" + names[i] + "-albedo.png";
    const std::string normal_name = std::string(out_dir) + "/" + names[i] + "-normal.png";
    if (stbi_write_png(albedo_name.c_str(), size, size, 4, tile.albedo.data(), size * 4) == 0 ||
        stbi_write_png(normal_name.c_str(), size, size, 4, tile.normal.data(), size * 4) == 0) {
      std::fprintf(stderr, "failed to write %s\n", albedo_name.c_str());
      return 1;
    }
    std::printf("%-16s mean=(%.2f %.2f %.2f)\n", names[i], static_cast<double>(tile.mean_albedo[0]),
                static_cast<double>(tile.mean_albedo[1]), static_cast<double>(tile.mean_albedo[2]));
  }
  return 0;
}

int cmd_life_stats(int seed_count) {
  std::uint64_t stage_hist[4][7] = {};
  std::uint64_t chem_hist[4][5] = {};
  std::uint64_t habitable[4] = {};
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    const auto type = static_cast<gen::PlanetType>(type_index);
    for (int seed = 0; seed < seed_count; ++seed) {
      const gen::BodyHandle body = body_for(core::Seed128{0, static_cast<std::uint64_t>(seed) + 1});
      const gen::PlanetParams planet = gen::derive_planet_params(body, type);
      const gen::MacroField macro(body.entity);
      const gen::ClimateField climate(body.entity, planet, macro);
      const gen::LifeParams life = gen::derive_life(body.entity, planet, climate);
      habitable[type_index] += life.habitable ? 1 : 0;
      ++stage_hist[type_index][static_cast<std::size_t>(life.stage)];
      ++chem_hist[type_index][static_cast<std::size_t>(life.chemistry)];
    }
  }
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    std::printf("%-9s habitable=%5.1f%% stages:", gen::to_string(static_cast<gen::PlanetType>(type_index)),
                100.0 * static_cast<double>(habitable[type_index]) / seed_count);
    for (int st = 0; st < 7; ++st) {
      std::printf(" %s=%.1f", gen::to_string(static_cast<gen::LifeStage>(st)),
                  100.0 * static_cast<double>(stage_hist[type_index][st]) / seed_count);
    }
    std::printf("\n          chemistry:");
    for (int c = 0; c < 5; ++c) {
      std::printf(" %s=%.1f", gen::to_string(static_cast<gen::LifeChemistry>(c)),
                  100.0 * static_cast<double>(chem_hist[type_index][c]) / seed_count);
    }
    std::printf("\n");
  }
  return 0;
}

}  // namespace inf::cli
