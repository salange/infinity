#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/key.hpp"
#include "gen/biome.hpp"
#include "gen/caves.hpp"
#include "gen/climate.hpp"
#include "gen/drainage.hpp"
#include "gen/features.hpp"
#include "gen/geo.hpp"
#include "gen/life.hpp"
#include "gen/names.hpp"
#include "gen/material.hpp"
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
  // The PRE-EROSION composition (macro + attenuated province fBm) — the
  // term whose analytic gradient is exact. Testing/diagnostic hook for
  // the WP0 derivative contract; gameplay uses elevation_from_params.
  det::Real elevation_base_from_params(const Dir3& unit_dir, const BlendedParams& params,
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
    det::Real terrace_amount;
    det::Real terrace_step_m;
    det::Real dune_amount;
    // Canonical macro/v1 value (dimensionless, own lod-7 lattice).
    det::Real macro_rel;
    // Dominant province archetype at the nearest lattice corner (T0019:
    // material/v2 reads it; discrete, so it rides along un-blended).
    Archetype dominant_archetype{Archetype::Flats};
  };

  // The province-parameter view of a canonical sample (macro_rel rides
  // separately) — the one place that copies the channels.
  static BlendedParams to_blended(const CanonicalParams& canonical) {
    BlendedParams params{};
    params.relief_amplitude_m = canonical.relief_amplitude_m;
    params.base_elevation_m = canonical.base_elevation_m;
    params.ruggedness = canonical.ruggedness;
    params.carving = canonical.carving;
    params.terrace_amount = canonical.terrace_amount;
    params.terrace_step_m = canonical.terrace_step_m;
    params.dune_amount = canonical.dune_amount;
    return params;
  }

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
    FeatureField::Cache features;
    CaveField::Cache caves;
  };
  CanonicalParams canonical_params(const FaceUV& face_uv, ParamCache* cache = nullptr) const;

  // elevation_from_params with the chunk cache (memoizes feature-cell
  // contents in addition to the param lattice; identical arithmetic).
  det::Real elevation_from_params(const Dir3& unit_dir, const BlendedParams& params,
                                  det::Real macro_rel, ParamCache* cache) const;

  // 3D detail term at a planet-local position (meters).
  det::Real detail_m(const Dir3& position_m) const;

  // Radius of the TOPMOST terrain surface crossing along the radial
  // through unit_dir — the ground under a ship. Without caves near the
  // radial this is one elevation evaluation plus a cheap bisection over
  // the detail term; near a cave system it becomes a fixed-step downward
  // scan so a mouth column reports the tunnel floor, never a cave roof
  // to fall through (T0015 WP7 Blocker B).
  det::Real ground_radius_m(const Dir3& unit_dir) const;

  // First surface crossing at or below from_r — the floor under a WALKING
  // player. On the open surface it equals ground_radius_m; inside a cave
  // it is the tunnel floor rather than the terrain surface overhead.
  det::Real ground_radius_below_m(const Dir3& unit_dir, det::Real from_r) const;

  // Nearby cave systems for one radial, gathered ONCE per column/query
  // (candidate probing + host draws are too hot for per-voxel work):
  // pointers either into the cache or into local storage.
  struct CaveQuery {
    const CaveField::System* systems[CaveField::kMaxCandidates];
    int count{0};
    CaveField::System storage[CaveField::kMaxCandidates];
  };
  void gather_caves(const Dir3& unit_dir, ParamCache* cache, CaveQuery* out) const;

  // Cave-aware carve: the smooth-min of the terrain density with every
  // gathered system's SDF. density passes through unchanged when no
  // system's bound reaches the position.
  det::Real apply_caves(const Dir3& position_m, det::Real surface_r, det::Real density,
                        const CaveQuery& query) const;

  // Extra meshing depth the chunk streamer needs below the surface at a
  // direction: 0 almost everywhere, CaveField::kDepthBudgetM for columns
  // whose radial can meet a cave system (WP7 Blocker A).
  det::Real cave_depth_budget_m(const Dir3& unit_dir) const;

  const CaveField& caves() const { return caves_; }
  const DrainageField& drainage() const { return drainage_; }

  // Signed density (meters-ish) at a planet-local position given in
  // meters. Positive = solid.
  det::Real density(const Dir3& position_m) const;

  const PlanetParams& planet() const { return planet_; }
  const ProvinceField& provinces() const { return provinces_; }
  const ClimateField& climate() const { return climate_; }
  const LifeParams& life() const { return life_; }
  const MaterialField& material() const { return material_; }
  const FeatureField& features() const { return features_; }

  // material/v2 (T0019): the surface material pair for a mesh vertex
  // given in planet-local metres with its unit normal. Gathers every
  // classifier input from the layers (canonical params, procedural
  // surface elevation, climate, biome) — one place, so chunks, the
  // far-view baker and the CLI maps agree by construction. The cache
  // memoizes the lattice corners exactly like canonical_params.
  VertexMaterial classify_vertex(double px, double py, double pz, double nx, double ny,
                                 double nz, ParamCache* cache = nullptr) const;
  // Per-material weights at a vertex (mesh classification, far view).
  void material_weights(double px, double py, double pz, double nx, double ny, double nz,
                        ParamCache* cache, double out[kMaterialCount]) const;
  // The gathered inputs themselves (CLI maps, tests).
  MaterialInputs material_inputs(double px, double py, double pz, double nx, double ny,
                                 double nz, ParamCache* cache = nullptr) const;

 private:
  PlanetParams planet_;
  ProvinceField provinces_;
  MacroField macro_;
  ClimateField climate_;
  LifeParams life_;
  MaterialField material_;
  FeatureField features_;
  CaveField caves_;
  DrainageField drainage_;

  const CaveField::System* cached_system(const CellId& cell, ParamCache* cache,
                                         CaveField::System* storage) const;
  det::Real evaluate_elevation(const Dir3& unit_dir, const BlendedParams& params,
                               det::Real macro_rel, Dir3* slope_out,
                               ParamCache* cache = nullptr) const;

  std::uint64_t elevation_lattice_;
  std::uint64_t detail_lattice_;
  std::uint64_t meso_lattice_{0};
  Dir3 detail_axis_{det::Real(0.0), det::Real(0.0), det::Real(1.0)};
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
