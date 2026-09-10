#pragma once
#include "scene.hpp"
#include <string_view>

namespace cb::canal_detail {
using Material = std::uint32_t;
inline const Vec3 kUp{0, 1, 0};

inline Material named(const Scene& scene, std::string_view name, Material fallback) {
  for (Material i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name) return i;
  return fallback;  // The explicit procedural-only scene has no authored kit.
}
inline Material material(Scene& scene, MaterialDesc value) {
  for (Material i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == value.name) return i;
  scene.materials.push_back(std::move(value));
  return static_cast<Material>(scene.materials.size() - 1);
}
struct Palette {
  Material stone, waterline, joints, bronze, wood, soil, light;
  explicit Palette(Scene& scene) {
    auto desc = scene.materials[M_MARBLE_WHITE];
    desc.name = "canal honed grey limestone";
    // A dry, diffuse grey mineral face. Compensation is for the pinned marble
    // albedo only; the stone keeps its own veins and small normal variation.
    desc.base_color = scene.reviewed_material_maps
                          ? Vec3{.81f, .79f, .67f}
                          : Vec3{.51f, .50f, .47f};
    desc.flags = kMatTriplanar; desc.roughness = .92f;
    desc.normal_strength = .25f; desc.uv_scale = 1.5f; desc.metallic = 0;
    stone = material(scene, desc);
    desc.name = "canal submerged mineral course";
    desc.base_color = scene.reviewed_material_maps
                          ? Vec3{.34f, .39f, .34f}
                          : Vec3{.21f, .24f, .24f};
    desc.roughness = .75f; desc.normal_strength = .32f;
    waterline = material(scene, desc);
    joints = named(scene, "gasket_charcoal", M_DARK_METAL);
    bronze = named(scene, "bronze_satin", M_BRONZE);
    wood = named(scene, "wood_oiled", M_PANEL_WARM);
    soil = named(scene, "soil_mulch", M_SOIL);
    desc = {}; desc.name = "canal warm shielded diffuser";
    desc.base_color = {1, .77f, .42f}; desc.tint2 = {1, .69f, .32f};
    desc.flags = kMatEmissive; desc.emissive = 1.6f; desc.roughness = .52f;
    light = material(scene, desc);
  }
};

} // namespace cb::canal_detail
