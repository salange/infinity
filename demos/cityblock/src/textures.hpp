#pragma once
// Material texture sets → three RGBA8 texture arrays (albedo sRGB,
// tangent normal, ARM = ao/roughness/height) with CPU mip chains, plus
// procedural fallbacks for missing files and generated utility textures
// (leaf clusters, lane paint).
#include <cstdint>
#include <string>
#include <vector>

#include "gpu.hpp"

namespace cb {

struct TextureSetSpec {
  std::string name;  // directory under assets/textures
  // Procedural fallback: base colour, roughness, and a pattern id.
  float fallback_rgb[3]{0.5f, 0.5f, 0.5f};
  float fallback_roughness{0.7f};
  int fallback_pattern{0};  // 0 flat, 1 concrete noise, 2 metal brushed, 3 stone tiles, 4 grass, 5 asphalt
};

struct MaterialArrays {
  Texture albedo;  // RGBA8 sRGB
  Texture normal;  // RGBA8 linear (xyz + 1)
  Texture arm;     // RGBA8 linear: r ao, g roughness, b height, a 1
  std::uint32_t size{1024};
  std::vector<std::string> names;  // layer i = names[i]
  int layer_of(const std::string& name) const;
};

// Loads every set (missing files → procedural fallback) and uploads.
MaterialArrays load_material_arrays(Gpu& gpu, const std::string& assets_dir,
                                    const std::vector<TextureSetSpec>& sets, std::uint32_t size,
                                    bool verbose);

// Generated RGBA8 leaf-cluster texture (alpha = coverage), sRGB colours.
std::vector<std::uint8_t> make_leaf_texture(std::uint32_t size, std::uint32_t seed);

}  // namespace cb
