#pragma once

#include "gen/terrain.hpp"
#include "world/chunk_sampler.hpp"

namespace inf::gen {

// Adapts Infinity's TerrainField to the engine's streaming interface.
class TerrainSampler final : public world::ChunkSampler {
 public:
  explicit TerrainSampler(const TerrainField& field) : field_(field) {}

  double radius_m() const override { return field_.planet().radius_m.to_double(); }
  world::PaddedDensity sample_padded(const world::ChunkGrid& grid) const override {
    return sample_chunk_density_padded(field_, grid);
  }
  double surface_elevation_m(const world::Dir3& unit_dir) const override {
    return field_.elevation_m(unit_dir).to_double();
  }

 private:
  const TerrainField& field_;
};

}  // namespace inf::gen
