#pragma once

#include <cstdint>

#include "world/chunk_grid.hpp"
#include "world/cubesphere.hpp"
#include "world/mesher.hpp"

namespace inf::world {

// The game's density source for one body, consumed by the streaming
// machinery. Implementations must be pure/deterministic and thread-safe
// for concurrent const calls (workers sample in parallel).
class ChunkSampler {
 public:
  virtual ~ChunkSampler() = default;

  // Nominal surface radius (meters) — chunk grids derive from it.
  virtual double radius_m() const = 0;

  // Padded density grid for a chunk (kPadded^3 samples).
  virtual PaddedDensity sample_padded(const ChunkGrid& grid) const = 0;

  // Surface elevation above the nominal radius in a direction — used to
  // pick the radial shells that contain the surface.
  virtual double surface_elevation_m(const Dir3& unit_dir) const = 0;

  // Elevation RANGE over a column: the min/max surface elevation the
  // streamer must cover for a column whose centre is `center` and whose
  // face-uv half-extent is `half_uv` (cube-face units). A single centre
  // sample misses bowls and ridges inside the column (crater floors on
  // fine columns went unmeshed — black holes in the ground), so
  // implementations probe the column's corners too. Default: the centre.
  virtual void surface_elevation_range_m(const Dir3& center, std::uint8_t face,
                                         double u_center, double v_center, double half_uv,
                                         double* lo_m, double* hi_m) const {
    (void)face;
    (void)u_center;
    (void)v_center;
    (void)half_uv;
    const double e = surface_elevation_m(center);
    *lo_m = e;
    *hi_m = e;
  }

  // Underground volumes the surface band misses: radial intervals (metres
  // relative to the nominal radius, like surface_elevation_m) where the
  // field carves voids along this column. The streamer meshes shells
  // covering exactly these intervals — never a blanket depth budget,
  // whose shell count diverges cubically with lod. Returns the interval
  // count written to out (at most max_intervals).
  struct DepthInterval {
    double lo_m;
    double hi_m;
  };
  virtual int underground_intervals(const Dir3&, DepthInterval* /*out*/,
                                    int /*max_intervals*/) const {
    return 0;
  }

  // Surface materials for a freshly meshed chunk: chooses the chunk's
  // four-material palette and fills every vertex's four weights from its
  // position and normal. Runs on the worker that meshed the chunk, so
  // implementations may build per-chunk caches. Default: no materials
  // (palette all zero — the renderer's flat path).
  virtual void classify_mesh(ChunkMesh& /*mesh*/) const {}
};

}  // namespace inf::world
