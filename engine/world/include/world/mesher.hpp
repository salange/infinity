#pragma once

#include <cstdint>
#include <vector>

#include "world/chunk_grid.hpp"

namespace inf::world {

// Chunk mesh in chunk-relative f32 (the one core->render conversion
// boundary, spec section 8). Flat-shaded triangle soup in v0 (M3);
// vertex sharing + Transvoxel transition cells land in M4.
struct ChunkMesh {
  // Chunk origin in planet-local meters (doubles; render folds this into
  // the camera-relative transform before any f32 sees planet magnitudes).
  double origin[3]{0.0, 0.0, 0.0};
  // Interleaved [px py pz nx ny nz w0 w1 w2 w3] per vertex, 3 vertices
  // per triangle: geometry plus the vertex's weights over the chunk's
  // four-material palette. The mesher writes geometry and zeros the
  // weights; the sampler's classify_mesh hook fills weights and palette
  // on the worker. Weights interpolate continuously across every
  // triangle, so material transitions never follow triangle edges.
  static constexpr std::size_t kVertexFloats = 10;
  static constexpr std::size_t kPaletteSize = 4;
  std::uint8_t palette[kPaletteSize]{0, 0, 0, 0};  // material ids (0 = unused)
  std::vector<float> vertices;

  std::size_t vertex_count() const { return vertices.size() / kVertexFloats; }
  std::size_t triangle_count() const { return vertices.size() / (3 * kVertexFloats); }
};

// Which lateral faces border a COARSER neighbor column (Transvoxel
// transition faces). Radial neighbors are always same-lod, so only the
// four lateral faces can transition.
enum TransitionFace : std::uint8_t {
  kTransitionUMinus = 1U << 0,  // gx = 0
  kTransitionUPlus = 1U << 1,   // gx = kVoxels
  kTransitionVMinus = 1U << 2,  // gy = 0
  kTransitionVPlus = 1U << 3,   // gy = kVoxels
};
using TransitionMask = std::uint8_t;

// Transvoxel meshing (Lengyel 2010; vendored MIT tables) over a chunk's
// padded density grid: modified marching cubes for regular cells, with
// the boundary sample layer retreated by half a cell on transition faces
// and transition cells stitching to the coarser neighbor's face — the
// low-res side of a transition cell reproduces exactly the crossing
// pattern the coarse chunk generates on the shared face. Normals are
// density-gradient normals (smooth, chunk-seam-consistent).
ChunkMesh mesh_chunk(const ChunkGrid& grid, const PaddedDensity& padded,
                     TransitionMask transitions = 0);

}  // namespace inf::world
