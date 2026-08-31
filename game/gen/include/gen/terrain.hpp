#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/key.hpp"
#include "gen/geo.hpp"
#include "gen/names.hpp"
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

  // Elevation with explicit (pre-blended) province parameters; the
  // canonical macro value is fetched internally.
  det::Real elevation_from_params(const Dir3& unit_dir, const BlendedParams& params) const;
  // Same with a FROZEN macro value (chunk sampling passes the canonical
  // per-column value; tests freeze it for differencing).
  det::Real elevation_from_params(const Dir3& unit_dir, const BlendedParams& params,
                                  det::Real macro_rel) const;

  const MacroField& macro() const { return macro_; }

  // Elevation plus the SURFACE-TANGENT gradient (terrain-stack v2 WP0):
  // the 3D noise gradient projected onto the tangent plane at unit_dir
  // and divided by the planet radius, i.e. metres of elevation change per
  // metre walked. Feeds talus operators, slope-directed erosion and
  // slope-based materials. Covers the noise term only — the province
  // parameter fields vary on >=10 km scales and are treated as locally
  // constant.
  struct ElevationD {
    det::Real elevation_m;
    Dir3 slope;  // tangent vector, |slope| = m/m grade
  };
  ElevationD elevation_and_gradient(const Dir3& unit_dir) const;

  // Canonical terrain parameters: the full province blend sampled on a
  // FIXED global lattice (kParamLatticeLod uv grid per cube face) and
  // bilinearly interpolated. A pure function of direction — every chunk,
  // at every lod, and every ground query computes bit-identical values at
  // the same point, which is what makes lod seams exactly crack-free.
  static constexpr std::uint32_t kParamLatticeLod = 6;
  static constexpr std::uint32_t kParamLatticeCells = 1U << kParamLatticeLod;  // per face edge

  struct CanonicalParams {
    det::Real relief_amplitude_m;
    det::Real base_elevation_m;
    det::Real ruggedness;
    det::Real carving;
    // Canonical macro/v1 value (dimensionless, own lod-7 lattice).
    det::Real macro_rel;
  };

  // True blended sample at one lattice corner (ci, cj in [0, cells]).
  // Province scalars only — macro rides its own (finer) lattice.
  CanonicalParams param_lattice_value(std::uint8_t face, std::uint32_t ci,
                                      std::uint32_t cj) const;
  // Bilinear canonical parameters at a face-uv position (province lattice
  // + the macro lattice). The optional cache memoizes lattice-corner
  // samples (per-chunk sampling); the arithmetic is identical with or
  // without it.
  struct ParamCache {
    std::unordered_map<std::uint64_t, CanonicalParams> params;
    MacroField::Cache macro;
  };
  CanonicalParams canonical_params(const FaceUV& face_uv, ParamCache* cache = nullptr) const;

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
  MacroField macro_;
  std::uint64_t elevation_lattice_;
  std::uint64_t detail_lattice_;
};


// Densities at all corner samples of a chunk grid, x-major:
// index = (gz * kCorners + gy) * kCorners + gx.
std::vector<det::Real> sample_chunk_density(const TerrainField& field, const ChunkGrid& grid);

// Padded variant: one extra sample ring on every side (kPadded^3 values,
// grid coordinates -1..kVoxels+1), used for gradient normals in meshing.
// Inner samples are computed with the identical op sequence as
// sample_chunk_density (golden hashes stay valid on the inner slice).

PaddedDensity sample_chunk_density_padded(const TerrainField& field, const ChunkGrid& grid);

// Golden fingerprint of a chunk's density grid (mesh input — the hashed
// artifact per T0005; the mesh itself is render-side and never hashed).
std::uint64_t hash_chunk_density(const TerrainField& field, const ChunkGrid& grid);

}  // namespace inf::gen
