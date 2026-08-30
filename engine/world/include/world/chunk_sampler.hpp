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
};

}  // namespace inf::world
