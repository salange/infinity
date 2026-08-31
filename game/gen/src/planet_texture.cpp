#include "gen/planet_texture.hpp"

#include <cmath>
#include <cstring>

namespace inf::gen {

namespace {

// float32 -> float16 (round to nearest even). Inputs are finite and in
// [-1, 1]; cosmetic path, platform floats are fine here.
std::uint16_t to_half(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const std::uint32_t sign = (bits >> 16U) & 0x8000U;
  const std::int32_t exponent =
      static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
  std::uint32_t mantissa = bits & 0x7FFFFFU;
  if (exponent <= 0) {
    return static_cast<std::uint16_t>(sign);  // flush tiny to zero
  }
  if (exponent >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7BFFU);  // clamp to max finite
  }
  const std::uint32_t rounded = mantissa + 0x1000U;
  std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10U) |
                       ((rounded >> 13U) & 0x3FFU);
  if (rounded & 0x800000U) {  // mantissa rounding overflowed into exponent
    half = sign | ((static_cast<std::uint32_t>(exponent) + 1U) << 10U);
  }
  return static_cast<std::uint16_t>(half);
}

struct Rgb {
  float r, g, b;
};

// material/v1 albedos — mirrors the WGSL palette in the RHI shader.
Rgb material_albedo(Material id) {
  switch (id) {
    case Material::Rock: return {0.42f, 0.38f, 0.34f};
    case Material::Regolith: return {0.46f, 0.44f, 0.41f};
    case Material::Sand: return {0.78f, 0.68f, 0.47f};
    case Material::Grass: return {0.28f, 0.43f, 0.20f};
    case Material::Snow: return {0.92f, 0.94f, 0.97f};
    case Material::IceSheet: return {0.70f, 0.80f, 0.90f};
    case Material::Seabed: return {0.34f, 0.35f, 0.29f};
    case Material::Scree: return {0.35f, 0.32f, 0.29f};
    default: return {0.55f, 0.52f, 0.45f};
  }
}

}  // namespace

PlanetTexture bake_planet_texture(const TerrainField& field, std::uint32_t face_size) {
  PlanetTexture out;
  out.face_size = face_size;
  const PlanetParams& planet = field.planet();
  const double radius = planet.radius_m.to_double();
  const double sea = planet.sea_level_m.to_double();
  const bool ocean_world = planet.type == PlanetType::EarthLike;
  const bool frozen_sea =
      planet.type == PlanetType::Ice && planet.land_fraction.to_double() < 0.999;
  // Per-planet palette shift, same mapping the app feeds the shader.
  const float shift =
      static_cast<float>(static_cast<double>(planet.palette_id % 201U) / 100.0 - 1.0);

  // ONE body-scoped cache for the whole bake: the province blend is the
  // cost (48.7 us cold vs 1.14 us warm per sample), and every face of
  // this body shares the same canonical lattice.
  TerrainField::ParamCache cache;

  const auto texel_dir = [&](std::uint8_t face, std::uint32_t x, std::uint32_t y) {
    const double n = static_cast<double>(face_size);
    const det::Real u((static_cast<double>(x) + 0.5) / n * 2.0 - 1.0);
    const det::Real v((static_cast<double>(y) + 0.5) / n * 2.0 - 1.0);
    return face_uv_to_dir(FaceUV{face, u, v});
  };

  // Pass 1: heights (metres relative to the nominal radius), seas
  // flattened to their level so the displaced impostor shows a calm
  // ocean surface, not the seabed.
  std::vector<float> height_m[6];
  std::vector<float> raw_m[6];  // pre-flattening, for water depth tint
  float amp = 1.0f;
  for (std::uint8_t face = 0; face < 6; ++face) {
    height_m[face].resize(static_cast<std::size_t>(face_size) * face_size);
    raw_m[face].resize(height_m[face].size());
    for (std::uint32_t y = 0; y < face_size; ++y) {
      for (std::uint32_t x = 0; x < face_size; ++x) {
        const Dir3 dir = texel_dir(face, x, y);
        const auto canonical = field.canonical_params(dir_to_face_uv(dir), &cache);
        const BlendedParams params = TerrainField::to_blended(canonical);
        float h = static_cast<float>(
            field.elevation_from_params(dir, params, canonical.macro_rel, &cache)
                .to_double());
        raw_m[face][static_cast<std::size_t>(y) * face_size + x] = h;
        if ((ocean_world || frozen_sea) && h < static_cast<float>(sea)) {
          h = static_cast<float>(sea);
        }
        height_m[face][static_cast<std::size_t>(y) * face_size + x] = h;
        const float mag = h < 0.0f ? -h : h;
        if (mag > amp) {
          amp = mag;
        }
      }
    }
  }
  out.height_amp_m = amp;

  // Pass 2: normalize heights; classify materials with grid-derived
  // normals (slope drives the rock override) and bake albedo, water
  // depth-tinted on ocean worlds.
  for (std::uint8_t face = 0; face < 6; ++face) {
    auto& face_out = out.faces[face];
    face_out.height_half.resize(height_m[face].size());
    face_out.rgba.resize(height_m[face].size() * 4);
    const auto h_at = [&](std::int32_t x, std::int32_t y) {
      x = x < 0 ? 0 : (x >= static_cast<std::int32_t>(face_size)
                           ? static_cast<std::int32_t>(face_size) - 1
                           : x);
      y = y < 0 ? 0 : (y >= static_cast<std::int32_t>(face_size)
                           ? static_cast<std::int32_t>(face_size) - 1
                           : y);
      return height_m[face][static_cast<std::size_t>(y) * face_size + x];
    };
    for (std::uint32_t y = 0; y < face_size; ++y) {
      for (std::uint32_t x = 0; x < face_size; ++x) {
        const std::size_t index = static_cast<std::size_t>(y) * face_size + x;
        const float h = height_m[face][index];
        face_out.height_half[index] = to_half(h / amp);

        const Dir3 dir = texel_dir(face, x, y);
        const double dx = dir.x.to_double();
        const double dy = dir.y.to_double();
        const double dz = dir.z.to_double();
        Rgb albedo;
        if ((ocean_world || frozen_sea) &&
            raw_m[face][index] < static_cast<float>(sea)) {
          if (frozen_sea) {
            albedo = material_albedo(Material::IceSheet);
          } else {
            // Depth-tinted open water (matches the ocean impostor hues).
            const float depth = static_cast<float>(sea) - raw_m[face][index];
            const float t = depth / (depth + 900.0f);
            albedo = {0.10f + (0.02f - 0.10f) * t, 0.30f + (0.10f - 0.30f) * t,
                      0.50f + (0.24f - 0.50f) * t};
          }
        } else {
          // Grid-space normal so steep texels classify as rock/scree.
          const double arc = radius * 1.57 / static_cast<double>(face_size);
          const double su = (h_at(static_cast<std::int32_t>(x) + 1,
                                  static_cast<std::int32_t>(y)) -
                             h_at(static_cast<std::int32_t>(x) - 1,
                                  static_cast<std::int32_t>(y))) /
                            (2.0 * arc);
          const double sv = (h_at(static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y) + 1) -
                             h_at(static_cast<std::int32_t>(x),
                                  static_cast<std::int32_t>(y) - 1)) /
                            (2.0 * arc);
          const double slope_sq = su * su + sv * sv;
          const double inv = 1.0 / std::sqrt(1.0 + slope_sq);
          // Normal tilted off the radial by the slope magnitude — the
          // classifier only reads 1 - dot(n, radial), so direction of
          // tilt is irrelevant.
          double nx = dx * inv;
          double ny = dy * inv;
          double nz = dz * inv;
          const double tilt = std::sqrt(slope_sq) * inv;
          nz += tilt;  // any off-radial component of the right magnitude
          const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
          nx /= len;
          ny /= len;
          nz /= len;
          const double r = radius + static_cast<double>(h);
          const VertexMaterial vm =
              field.material().classify(dx * r, dy * r, dz * r, nx, ny, nz);
          const Rgb a0 = material_albedo(vm.mat0);
          const Rgb a1 = material_albedo(vm.mat1);
          const float blend = vm.blend < 0.0f ? 0.0f : (vm.blend > 1.0f ? 1.0f : vm.blend);
          albedo = {a0.r + (a1.r - a0.r) * blend, a0.g + (a1.g - a0.g) * blend,
                    a0.b + (a1.b - a0.b) * blend};
        }
        // Per-planet palette shift (same hue rotation as the terrain).
        albedo.r *= 1.0f + shift * 0.10f;
        albedo.g *= 1.0f + shift * 0.02f;
        albedo.b *= 1.0f - shift * 0.08f;
        const auto to_byte = [](float value) {
          const float scaled = value * 255.0f + 0.5f;
          return static_cast<std::uint8_t>(scaled < 0.0f ? 0.0f
                                                         : (scaled > 255.0f ? 255.0f
                                                                            : scaled));
        };
        face_out.rgba[index * 4 + 0] = to_byte(albedo.r);
        face_out.rgba[index * 4 + 1] = to_byte(albedo.g);
        face_out.rgba[index * 4 + 2] = to_byte(albedo.b);
        face_out.rgba[index * 4 + 3] = 255;
      }
    }
  }
  return out;
}

}  // namespace inf::gen
