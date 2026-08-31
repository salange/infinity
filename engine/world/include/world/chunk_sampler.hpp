#pragma once

#include "world/chunk_grid.hpp"
#include "world/cubesphere.hpp"

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
};

}  // namespace inf::world
