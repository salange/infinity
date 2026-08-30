#pragma once

#include <algorithm>
#include <cmath>

#include "gen/terrain.hpp"
#include "world/chunk_sampler.hpp"
#include "world/edit_store.hpp"

namespace inf::gen {

// Adapts Infinity's TerrainField to the engine's streaming interface,
// folding the player-diff overlay (M7) into every padded grid so meshes
// show craters/built material. Workers call this concurrently; the edit
// store is thread-safe.
class TerrainSampler final : public world::ChunkSampler {
 public:
  explicit TerrainSampler(const TerrainField& field,
                          const world::EditStore* edits = nullptr)
      : field_(field), edits_(edits) {}

  double radius_m() const override { return field_.planet().radius_m.to_double(); }

  world::PaddedDensity sample_padded(const world::ChunkGrid& grid) const override {
    world::PaddedDensity padded = sample_chunk_density_padded(field_, grid);
    if (edits_ == nullptr || edits_->size() == 0) {
      return padded;
    }
    // Ball around the padded grid (curvilinear, so bound by its corners).
    constexpr int kHi = static_cast<int>(world::ChunkGrid::kVoxels) + 1;
    const int mid = static_cast<int>(world::ChunkGrid::kVoxels) / 2;
    const world::Dir3 center = grid.corner_position(mid, mid, mid);
    double bound_sq = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
      const world::Dir3 p = grid.corner_position((corner & 1) != 0 ? kHi : -1,
                                                 (corner & 2) != 0 ? kHi : -1,
                                                 (corner & 4) != 0 ? kHi : -1);
      const double dx = p.x.to_double() - center.x.to_double();
      const double dy = p.y.to_double() - center.y.to_double();
      const double dz = p.z.to_double() - center.z.to_double();
      bound_sq = std::max(bound_sq, dx * dx + dy * dy + dz * dz);
    }
    det::Fixed64 ball[3] = {det::Fixed64::from_double(center.x.to_double()),
                            det::Fixed64::from_double(center.y.to_double()),
                            det::Fixed64::from_double(center.z.to_double())};
    const auto hits = edits_->overlapping(ball, det::Fixed64::from_double(std::sqrt(bound_sq)));
    if (hits.empty()) {
      return padded;
    }
    for (int gz = -1; gz <= kHi; ++gz) {
      for (int gy = -1; gy <= kHi; ++gy) {
        for (int gx = -1; gx <= kHi; ++gx) {
          const world::Dir3 p = grid.corner_position(gx, gy, gz);
          const std::size_t index =
              (static_cast<std::size_t>(gz + 1) * world::PaddedDensity::kPadded +
               static_cast<std::size_t>(gy + 1)) *
                  world::PaddedDensity::kPadded +
              static_cast<std::size_t>(gx + 1);
          padded.values[index] =
              det::Real(world::apply_edits(padded.values[index].to_double(), hits,
                                           p.x.to_double(), p.y.to_double(),
                                           p.z.to_double()));
        }
      }
    }
    return padded;
  }

  double surface_elevation_m(const world::Dir3& unit_dir) const override {
    return field_.elevation_m(unit_dir).to_double();
  }

 private:
  const TerrainField& field_;
  const world::EditStore* edits_;
};

}  // namespace inf::gen
