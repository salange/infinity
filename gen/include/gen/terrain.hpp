#pragma once

#include <cstdint>
#include <vector>

#include "core/chunk_addr.hpp"
#include "core/key.hpp"
#include "gen/cubesphere.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

// terrain/v1 (prototype-v0 spec sections 3-4): the planet's density field.
// density(p) > 0 = solid, < 0 = air; the surface is the zero isosurface.
// Roughly: (radius + elevation(dir)) - |p| + detail3d(p), clamped solid
// below the impenetrable core. A pure function of position and keys —
// evaluated on demand, never stored.
class TerrainField {
 public:
  TerrainField(const core::Key& body_key, const PlanetParams& planet);

  // Surface elevation (meters, relative to planet radius) in a direction.
  // Blends province parameters and shapes fBm by them.
  det::Real elevation_m(const Dir3& unit_dir) const;

  // Elevation with explicit (pre-blended) province parameters — the fast
  // path used by the chunk sampler, which interpolates province params
  // bilinearly from the chunk's shared corners instead of running the full
  // province blend per column.
  det::Real elevation_from_params(const Dir3& unit_dir, const BlendedParams& params) const;

  // 3D detail term at a planet-local position (meters).
  det::Real detail_m(const Dir3& position_m) const;

  // Radius of the terrain surface (the density zero crossing) along the
  // radial through unit_dir — the ground under a ship/player. One
  // elevation evaluation plus a cheap bisection over the detail term, so
  // it matches the meshed isosurface to sub-voxel precision.
  det::Real ground_radius_m(const Dir3& unit_dir) const;

  // Signed density (meters-ish) at a planet-local position given in
  // meters. Positive = solid.
  det::Real density(const Dir3& position_m) const;

  const PlanetParams& planet() const { return planet_; }
  const ProvinceField& provinces() const { return provinces_; }

 private:
  PlanetParams planet_;
  ProvinceField provinces_;
  std::uint64_t elevation_lattice_;
  std::uint64_t detail_lattice_;
};

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
  static ChunkGrid from_addr(const core::ChunkAddr& addr, const PlanetParams& planet);

  // World position (planet-local meters) of grid corner (gx, gy, gz):
  // gx, gy along u/v, gz along the radial axis. Accepts coordinates
  // outside [0, kVoxels] (padded ring, linear extrapolation in uv/r).
  Dir3 corner_position(int gx, int gy, int gz) const;
};

// Densities at all corner samples of a chunk grid, x-major:
// index = (gz * kCorners + gy) * kCorners + gx.
std::vector<det::Real> sample_chunk_density(const TerrainField& field, const ChunkGrid& grid);

// Padded variant: one extra sample ring on every side (kPadded^3 values,
// grid coordinates -1..kVoxels+1), used for gradient normals in meshing.
// Inner samples are computed with the identical op sequence as
// sample_chunk_density (golden hashes stay valid on the inner slice).
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

PaddedDensity sample_chunk_density_padded(const TerrainField& field, const ChunkGrid& grid);

// Golden fingerprint of a chunk's density grid (mesh input — the hashed
// artifact per T0005; the mesh itself is render-side and never hashed).
std::uint64_t hash_chunk_density(const TerrainField& field, const ChunkGrid& grid);

}  // namespace inf::gen
