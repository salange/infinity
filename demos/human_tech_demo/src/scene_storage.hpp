#pragma once
#include "scene.hpp"

namespace cb {
// Call only after all CPU consumers and that mesh's GPU upload finish.
// Bounds and all external scene metadata remain unchanged.
inline std::uint64_t release_mesh_storage(Mesh &mesh) {
  const auto released = std::uint64_t(mesh.vertices.capacity()) * sizeof(Vertex) +
                        std::uint64_t(mesh.indices.capacity()) * sizeof(std::uint32_t);
  std::vector<Vertex>().swap(mesh.vertices);
  std::vector<std::uint32_t>().swap(mesh.indices);
  return released;
}
// Idempotent final cleanup; set_scene now consumes meshes individually after
// transport voxelization, reflection ranges, resource bounds and GPU upload.
inline std::uint64_t release_scene_mesh_storage(Scene &scene) {
  std::uint64_t released = 0;
  auto release = [&](Mesh &mesh) {
    released += release_mesh_storage(mesh);
  };
  release(scene.opaque);
  release(scene.foliage);
  for (auto &resource : scene.asset_library.resources)
    release(resource.mesh);
  return released;
}
} // namespace cb
