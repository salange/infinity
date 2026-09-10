#pragma once
#include "scene.hpp"

namespace cb {
// Call only after Renderer::set_scene has consumed geometry for GPU upload,
// transport voxelization, reflection ranges and resource bounds. Camera,
// materials, transforms, statistics and all authored metadata remain usable.
inline std::uint64_t release_scene_mesh_storage(Scene &scene) {
  std::uint64_t released = 0;
  auto release = [&](Mesh &mesh) {
    released += std::uint64_t(mesh.vertices.capacity()) * sizeof(Vertex) +
                std::uint64_t(mesh.indices.capacity()) * sizeof(std::uint32_t);
    std::vector<Vertex>().swap(mesh.vertices);
    std::vector<std::uint32_t>().swap(mesh.indices);
  };
  release(scene.opaque);
  release(scene.foliage);
  for (auto &resource : scene.asset_library.resources)
    release(resource.mesh);
  return released;
}
} // namespace cb
