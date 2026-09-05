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

}  // namespace

PlanetTexture bake_planet_texture(const TerrainField& field, std::uint32_t face_size,
                                  const float* albedo_table) {
  PlanetTexture out;
  out.face_size = face_size;
  const PlanetParams& planet = field.planet();
  const double radius = planet.radius_m.to_double();
  const double sea = planet.sea_level_m.to_double();
  const bool ocean_world = planet.type == PlanetType::EarthLike;
  const bool frozen_sea =
      planet.type == PlanetType::Ice && planet.land_fraction.to_double() < 0.999;
  // Material colours: the loaded tile means when the app supplies them,
  // else the registry means — both under the planet's palette tints.
  // Tile means are sRGB-encoded like the tiles; the far-view albedo map
  // is sampled as linear, so decode (x^2.2 ~ x*x*(0.8 + 0.2x), no libm).
  const auto decode = [](float x) {
    x = x < 0.0f ? 0.0f : x;
    return x * x * (0.8f + 0.2f * x);
  };
  const auto material_rgb = [&](Material id) {
    Rgb out;
    float tint[3];
    field.material().tint(id, tint);
    const auto index = static_cast<std::size_t>(id);
    if (albedo_table != nullptr && index < kMaterialCount) {
      out = {decode(albedo_table[index * 3]) * tint[0], decode(albedo_table[index * 3 + 1]) * tint[1],
             decode(albedo_table[index * 3 + 2]) * tint[2]};
    } else {
      const MaterialInfo& info = material_info(id);
      out = {decode(info.albedo[0]) * tint[0], decode(info.albedo[1]) * tint[1],
             decode(info.albedo[2]) * tint[2]};
    }
    return out;
  };

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
            albedo = material_rgb(Material::IceSheet);
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
          // Weight-averaged material colour: a texel covers hundreds of
          // metres, so it shows the MIX the rules produce, not a hard
          // top-two pick (which read as blocky patches from orbit).
          double weights[kMaterialCount];
          field.material_weights(dx * r, dy * r, dz * r, nx, ny, nz, &cache, weights);
          double total = 0.0;
          double acc[3] = {0.0, 0.0, 0.0};
          for (std::uint32_t m = 1; m < kMaterialCount; ++m) {
            if (weights[m] <= 0.0) {
              continue;
            }
            const Rgb c = material_rgb(static_cast<Material>(m));
            acc[0] += weights[m] * c.r;
            acc[1] += weights[m] * c.g;
            acc[2] += weights[m] * c.b;
            total += weights[m];
          }
          if (total > 0.0) {
            albedo = {static_cast<float>(acc[0] / total), static_cast<float>(acc[1] / total),
                      static_cast<float>(acc[2] / total)};
          } else {
            albedo = material_rgb(Material::RockGranite);
          }
        }
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
