#pragma once

#include <cstdint>
#include <vector>

#include "gen/terrain.hpp"

namespace inf::gen {

// Chunk mesh in chunk-relative f32 (the one core->render conversion
// boundary, spec section 8). Flat-shaded triangle soup in v0 (M3);
// vertex sharing + Transvoxel transition cells land in M4.
struct ChunkMesh {
  // Chunk origin in planet-local meters (doubles; render folds this into
  // the camera-relative transform before any f32 sees planet magnitudes).
  double origin[3]{0.0, 0.0, 0.0};
  // Interleaved [px py pz nx ny nz] per vertex, 3 vertices per triangle.
  std::vector<float> vertices;

  std::size_t triangle_count() const { return vertices.size() / 18; }
};

// Marching cubes (classic tables) over a chunk's padded density grid.
// Normals are density-gradient normals (smooth, chunk-seam-consistent);
// the padded ring supplies boundary gradients.
ChunkMesh mesh_chunk(const ChunkGrid& grid, const PaddedDensity& padded);

}  // namespace inf::gen
