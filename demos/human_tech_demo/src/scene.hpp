#pragma once
// Fixed visual set, composed with the shared cosmetic city geometry kit.
#include "asset_pipeline.hpp"
#include "city_roofs.hpp"
#include "tower_floor_identity.hpp"
#include "city/materials.hpp"
#include "city/scene.hpp"
#include <string>
namespace cb {
using namespace inf::city;
struct Scene : inf::city::Scene {
  TowerFloorMaterials tower_floor_materials;
  bool reviewed_material_maps{false};
  std::vector<AuthoredRoof> authored_roofs;
  std::vector<RoofObstruction> roof_obstructions;
  AssetLibrary asset_library;
  std::vector<AssetInstance> asset_instances;
  float camera_fov_degrees{0};
  bool has_lighting_override{false};
  Vec3 sun_direction{}, sun_irradiance{};
  float lighting_exposure{1};
};
struct SceneParams {
  bool reviewed_material_maps{false};
  std::string shot{"aerial"};
  std::string seed{"83"};
  bool far_patterns{true};
  int detail{2};
  std::string asset;
  std::string asset_kit;
};
bool shot_camera(const std::string &shot, Vec3 &position, Vec3 &target);
float shot_fov_degrees(const std::string &shot);
std::string scene_layout_manifest();
std::string scene_layout_manifest(const std::string &seed, bool authored_arrival_district = true);
Scene generate_scene(const SceneParams &params);
} // namespace cb
