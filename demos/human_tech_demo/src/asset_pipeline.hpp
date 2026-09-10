#pragma once
// Authored, reusable metre-scale resources for this standalone visual scene.
// Binary meshes are compiled from glTF; instances never duplicate vertex data.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "city/scene.hpp"
namespace cb {
using namespace inf::city;
struct MeshResource {
  std::string name;
  Mesh mesh;
};
struct AssetLibrary {
  std::vector<MaterialDesc> materials;
  std::vector<MeshResource> resources;
  std::string source_sha256;
};
struct AssetInstance {
  std::uint32_t resource{0};
  Vec3 translation{};
  float yaw{0};
  Vec3 scale{1,1,1};
  Vec3 tint{1,1,1};
};
// Rejects malformed, unsupported or incomplete resources with a descriptive error.
// Vertex material ids are shifted by material_base on import.
AssetLibrary load_asset_library(const std::filesystem::path& path, std::uint32_t material_base = 0);
std::uint32_t asset_resource(const AssetLibrary& library, std::string_view name);
Vec3 asset_transform_point(const AssetInstance& instance, Vec3 p);
Vec3 asset_transform_normal(const AssetInstance& instance, Vec3 n);
struct Scene;
void add_asset_instance(Scene& scene, std::string_view name, Vec3 position,
                        float yaw = 0, Vec3 scale = {1,1,1}, Vec3 tint = {1,1,1});
inline void add_asset_instance(Scene& scene, std::string_view name, Vec3 position,
                              float yaw, float uniform_scale) {
  add_asset_instance(scene, name, position, yaw, {uniform_scale,uniform_scale,uniform_scale});
}
}  // namespace cb
