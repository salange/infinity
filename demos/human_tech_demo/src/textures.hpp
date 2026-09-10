#pragma once
// Material texture sets → three RGBA8 texture arrays (albedo sRGB,
// tangent normal, ARM = ao/roughness/height) with CPU mip chains, plus
// procedural fallbacks for missing files and generated utility textures
// (leaf clusters, lane paint).
#include <cstdint>
#include <string>
#include <vector>

#include "gpu.hpp"
#include "materials.hpp"

namespace cb {

struct MaterialArrays {
  Texture albedo; // RGBA8 sRGB
  Texture normal; // RGBA8 linear (xyz + 1)
  Texture arm;    // RGBA8 linear: r ao, g roughness, b height, a 1
  std::uint32_t size{1024};
  std::vector<std::string> names; // layer i = names[i]
  std::vector<bool>
      file_sets; // complete decoded color, normal and roughness maps
  int layer_of(const std::string &name) const;
  bool has_file_set(const std::string &name) const;
};

// Loads every set (missing files → procedural fallback) and uploads.
MaterialArrays load_material_arrays(Gpu &gpu, const std::string &assets_dir,
                                    const std::vector<TextureSetSpec> &sets,
                                    std::uint32_t size, bool verbose);

// Generated RGBA8 leaf-cluster texture (alpha = coverage), sRGB colours.
std::vector<std::uint8_t> make_leaf_texture(std::uint32_t size,
                                            std::uint32_t seed);

} // namespace cb
