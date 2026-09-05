#pragma once
// The generated city block: materials, geometry (opaque + foliage), lights
// and a camera start. Pure function of the seed (Philox keys, rng.hpp).
#include <cstdint>
#include <string>
#include <vector>

#include "math.hpp"
#include "mesh.hpp"
#include "textures.hpp"

namespace cb {

// Material flags (mirrored in shaders/common.wgsl).
enum : std::uint32_t {
  kMatGlass = 1u,        // interior-mapped reflective glazing
  kMatEmissive = 2u,     // self-lit surface (lamps, signage); colour in tint2
  kMatPlanarXZ = 4u,     // uv from world xz (ground planes)
  kMatFoliage = 8u,      // alpha-tested leaf texture
  kMatTriplanar = 16u,   // uv from the dominant world plane
  kMatNightOnly = 32u,   // emissive only at night
};

struct MaterialDesc {
  std::string name;
  Vec3 base_color{1, 1, 1};  // tint × albedo texture
  float roughness{0.6f};
  float metallic{0.0f};
  float emissive{0.0f};
  float normal_strength{1.0f};
  std::string albedo_set;  // texture set name ("" = flat)
  float uv_scale{2.0f};    // metres per texture repeat
  std::uint32_t flags{0};
  Vec3 tint2{1, 1, 1};     // glass transmission tint / emissive colour
  float room_w{4.5f}, room_h{3.6f}, room_d{6.0f}, lit_probability{0.55f};
};

struct PointLight {
  Vec3 position;
  float radius{12.0f};
  Vec3 color{1.0f, 0.85f, 0.6f};
  float intensity{1.0f};
};

struct Scene {
  std::vector<MaterialDesc> materials;
  Mesh opaque;
  Mesh foliage;
  std::vector<PointLight> lights;  // on at night
  Vec3 camera_position{0, 40, 200};
  Vec3 camera_target{0, 60, 0};
  std::vector<TextureSetSpec> texture_sets() const;
};

struct SceneParams {
  std::string seed{"83"};
  bool context_buildings{true};
  int detail{2};  // 0 coarse … 2 full
};

Scene generate_scene(const SceneParams& params);

}  // namespace cb
