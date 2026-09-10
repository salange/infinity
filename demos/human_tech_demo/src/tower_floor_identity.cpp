#include "tower_floor_identity.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace cb {
namespace {
bool same_material(const inf::city::MaterialDesc &a,
                   const inf::city::MaterialDesc &b) {
  return a.name == b.name && a.base_color.x == b.base_color.x &&
         a.base_color.y == b.base_color.y && a.base_color.z == b.base_color.z &&
         a.roughness == b.roughness && a.metallic == b.metallic &&
         a.emissive == b.emissive && a.normal_strength == b.normal_strength &&
         a.albedo_set == b.albedo_set && a.uv_scale == b.uv_scale &&
         a.flags == b.flags && a.tint2.x == b.tint2.x &&
         a.tint2.y == b.tint2.y && a.tint2.z == b.tint2.z &&
         a.room_w == b.room_w && a.room_h == b.room_h && a.room_d == b.room_d &&
         a.lit_probability == b.lit_probability;
}
} // namespace

inf::city::Mat TowerFloorMaterials::material(inf::city::Scene &scene,
                                             inf::city::Mat source,
                                             float floor_height) {
  if (!std::isfinite(floor_height) || floor_height <= 0)
    throw std::invalid_argument(
        "tower floor height must be finite and positive");
  if (std::size_t(source) >= scene.materials.size())
    throw std::out_of_range(
        "tower shaft material is outside the scene palette");
  const auto original = scene.materials[source];
  // Flag128 reuses the room fields for optical parameters, not room dimensions.
  if (!(original.flags & inf::city::kMatGlass) || (original.flags & 128u))
    return source;
  for (const auto &entry : entries_)
    if (entry.source == source && entry.floor_height == floor_height &&
        same_material(entry.original, original))
      return entry.shaft;
  // The renderer packs 16-bit material indices; transport reserves 65535.
  if (scene.materials.size() >= 65535)
    throw std::length_error(
        "tower shaft material exceeds the renderer palette");
  const auto shaft = static_cast<inf::city::Mat>(scene.materials.size());
  auto copy = original;
  copy.room_h = floor_height;
  copy.name += " / shaft storey " + std::to_string(floor_height) + " m";
  scene.materials.push_back(std::move(copy));
  entries_.push_back({source, shaft, floor_height, original});
  return shaft;
}

TowerFloorSpan build_floor_aligned_tower(inf::city::Scene &scene,
                                         TowerFloorMaterials &materials,
                                         inf::city::TowerSpec spec,
                                         inf::city::Vec2 centre, float base_y,
                                         inf::city::Rng rng, int detail) {
  if (!std::isfinite(base_y))
    throw std::invalid_argument("tower floor datum must be finite");
  const auto original = spec.glass;
  spec.glass = materials.material(scene, original, spec.floor_h);
  TowerFloorSpan span{scene.opaque.vertices.size(), 0, 0, original, spec.glass};
  inf::city::build_tower(scene, spec, centre, base_y, rng, detail);
  span.end_vertex = scene.opaque.vertices.size();
  if (spec.glass != original)
    for (auto i = span.first_vertex; i < span.end_vertex; ++i) {
      auto &vertex = scene.opaque.vertices[i];
      if (vertex.material == spec.glass) {
        // UV holds facade pattern parameters in the analytic path. Only the
        // metric room coordinate receives the building's physical floor datum.
        vertex.aux.y -= base_y;
        ++span.adjusted_vertices;
      }
    }
  return span;
}
} // namespace cb
