#pragma once

#include <cstdint>
#include <vector>

#include "core/chunk_addr.hpp"
#include "core/det/real.hpp"
#include "world/cubesphere.hpp"

namespace inf::world {

// Chunk geometry: cube-sphere curvilinear grid (spec section 6). A chunk
// at (face, lod, i, j, shell) covers a (u, v) quad of the face quadtree and
// a radial span; voxel corners live at (u_i, v_j, r_k). Grid resolution is
// kVoxels cells => kCorners^3 density samples per chunk.
struct ChunkGrid {
  static constexpr std::uint32_t kVoxels = 32;
  static constexpr std::uint32_t kCorners = kVoxels + 1;

  core::ChunkAddr addr;
  det::Real u0, u1, v0, v1;      // face-uv span
  det::Real r0, r1;              // radial span (meters from planet center)

  // Chunk grid from an address: uv span from the face quadtree cell,
  // radial span shell*thickness relative to the planet surface radius,
  // with thickness equal to the chunk's lateral arc length (cubic-ish).
  static ChunkGrid from_addr(const core::ChunkAddr& addr, det::Real surface_radius_m);

  // World position (planet-local meters) of grid corner (gx, gy, gz):
  // gx, gy along u/v, gz along the radial axis. Accepts coordinates
  // outside [0, kVoxels] (padded ring, linear extrapolation in uv/r).
  Dir3 corner_position(int gx, int gy, int gz) const;
  // Fractional-coordinate variant (transition-cell geometry).
  Dir3 corner_position_f(double gx, double gy, double gz) const;
};

struct PaddedDensity {
  static constexpr std::uint32_t kPadded = ChunkGrid::kCorners + 2;
  std::vector<det::Real> values;  // x-major over kPadded^3

  det::Real at(int gx, int gy, int gz) const {  // grid coords, -1-based
    return values[(static_cast<std::size_t>(gz + 1) * kPadded +
                   static_cast<std::size_t>(gy + 1)) *
                      kPadded +
                  static_cast<std::size_t>(gx + 1)];
  }
};

}  // namespace inf::world
