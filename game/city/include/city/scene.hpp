#pragma once
// A generated city scene: materials, geometry (opaque + foliage), point
// lights, draw ranges with detail levels. Pure function of its keys.
#include <cstdint>
#include <string>
#include <vector>

#include "city/math.hpp"
#include "city/mesh.hpp"

namespace inf::city {

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

// A contiguous index range of the opaque mesh with bounds, optionally one of
// several detail levels of the same object (the renderer picks by camera
// distance and culls by the frustum).
struct DrawRange {
  std::uint32_t first{0}, count{0};
  Vec3 centre;
  float radius{0.0f};          // 0 = unbounded (never culled)
  int lod_group{-1};           // objects sharing a group are alternatives
  int lod_level{0};            // 0 = finest
  float lod_max_distance{1e30f};  // draw this level while camera distance < this
  bool has_fine{false};        // a block whose buildings are also registered individually in `fine`
};

struct Scene {
  std::vector<MaterialDesc> materials;
  Mesh opaque;
  Mesh foliage;
  std::vector<DrawRange> draws;  // finalised by finalize_draws(); unregistered geometry becomes static ranges
  // Per-building sub-ranges of block ranges (sorted by first): the
  // granularity of occlusion culling (T0022 B.1). A block-sized range's
  // bounding sphere always reaches into the sky; a building's does not.
  std::vector<DrawRange> fine;
  int lod_groups{0};
  // Registers [first, end) of the opaque index buffer as one drawable.
  void register_range(std::uint32_t first, std::uint32_t end, Vec3 centre, float radius, int lod_group = -1,
                      int lod_level = 0, float lod_max_distance = 1e30f, bool has_fine = false);
  void register_fine(std::uint32_t first, std::uint32_t end, Vec3 centre, float radius);
  void finalize_draws();
  std::vector<PointLight> lights;  // on at night
  Vec3 camera_position{0, 40, 200};
  Vec3 camera_target{0, 60, 0};
  std::string city_size;
  float city_radius{0.0f};
  int stats_blocks{0}, stats_towers{0}, stats_standards{0}, stats_plazas{0};
};


}  // namespace inf::city
