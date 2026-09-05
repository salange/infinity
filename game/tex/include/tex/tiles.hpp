#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inf::tex {

// Procedural surface tiles (T0019 WP6): seamless PBR tiles generated from
// periodic noise, one generator per material family. They are the
// fallback for every material when the CC0 library is absent, and the
// first step toward per-planet baked material graphs (the C stage).
// Cosmetic code: platform floats are fine, nothing here feeds gameplay.
//
// Layout matches the renderer's material library:
//   albedo: RGBA8 = linear-ish sRGB colour + height in alpha
//   normal: RGBA8 = tangent-space normal xy (0..255 -> -1..1), roughness,
//           ambient occlusion — or, for emissive materials, the emissive
//           mask in alpha.
struct Tile {
  std::uint32_t size{0};
  std::vector<std::uint8_t> albedo;
  std::vector<std::uint8_t> normal;
  float mean_albedo[3]{0.5f, 0.5f, 0.5f};
  bool emissive{false};
};

// Generates the tile for a material registry name (gen/material.hpp:
// "rock_granite", "sand_dune", "crystal_field", ...). Unknown names get
// a generic rock. `seed` varies the pattern, `size` is a power of two.
Tile generate_tile(const std::string& material_name, std::uint32_t size, std::uint64_t seed);

// Derives normal/roughness/ao from a height field already in albedo.a
// (helper shared with the image loader when a source set lacks maps).
void finish_tile_from_height(Tile& tile, float normal_strength, float roughness_base,
                             float roughness_variation);

// All registry names the generator knows (for tools).
const char* const* known_tile_names(std::size_t* count);

}  // namespace inf::tex
