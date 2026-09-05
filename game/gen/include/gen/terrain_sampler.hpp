#pragma once

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

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

  // material/v2 on the worker: one ParamCache per chunk (the province
  // blend is the cost; a chunk spans a handful of lattice cells).
  // material/v2 on the worker: one ParamCache per chunk (the province
  // blend is the cost; a chunk spans a handful of lattice cells).
  //
  // Per-chunk PALETTE: the four materials with the most weight over the
  // chunk's vertices; each vertex stores its normalised weights over that
  // palette. Weights interpolate continuously across every triangle and
  // across LOD levels, so no transition can follow a triangle edge (a
  // per-triangle pair made hard-edged patches at every pair switch).
  void classify_mesh(world::ChunkMesh& mesh) const override {
    TerrainField::ParamCache cache;
    constexpr std::size_t kStride = world::ChunkMesh::kVertexFloats;
    const std::size_t count = mesh.vertices.size() / kStride;
    if (count == 0) {
      return;
    }
    std::vector<double> all(count * kMaterialCount);
    double total[kMaterialCount] = {};
    for (std::size_t v = 0; v < count; ++v) {
      const float* p = mesh.vertices.data() + v * kStride;
      double* w = all.data() + v * kMaterialCount;
      field_.material_weights(mesh.origin[0] + static_cast<double>(p[0]),
                              mesh.origin[1] + static_cast<double>(p[1]),
                              mesh.origin[2] + static_cast<double>(p[2]),
                              static_cast<double>(p[3]), static_cast<double>(p[4]),
                              static_cast<double>(p[5]), &cache, w);
      // Presence matters more than mass: a material that dominates a few
      // vertices must make the palette even if it is rare in the chunk.
      double vmax = 0.0;
      for (std::size_t m = 1; m < kMaterialCount; ++m) {
        vmax = w[m] > vmax ? w[m] : vmax;
      }
      if (vmax > 0.0) {
        for (std::size_t m = 1; m < kMaterialCount; ++m) {
          total[m] += w[m] / vmax;
        }
      }
    }
    std::size_t palette[world::ChunkMesh::kPaletteSize] = {0, 0, 0, 0};
    for (std::size_t slot = 0; slot < world::ChunkMesh::kPaletteSize; ++slot) {
      double best = 0.0;
      std::size_t pick = 0;
      for (std::size_t m = 1; m < kMaterialCount; ++m) {
        if (total[m] > best) {
          best = total[m];
          pick = m;
        }
      }
      palette[slot] = pick;
      if (pick != 0) {
        total[pick] = -1.0;  // taken
      }
      mesh.palette[slot] = static_cast<std::uint8_t>(pick);
    }
    for (std::size_t v = 0; v < count; ++v) {
      float* p = mesh.vertices.data() + v * kStride;
      const double* w = all.data() + v * kMaterialCount;
      double sum = 0.0;
      double ws[world::ChunkMesh::kPaletteSize];
      for (std::size_t slot = 0; slot < world::ChunkMesh::kPaletteSize; ++slot) {
        ws[slot] = palette[slot] != 0 ? w[palette[slot]] : 0.0;
        sum += ws[slot];
      }
      for (std::size_t slot = 0; slot < world::ChunkMesh::kPaletteSize; ++slot) {
        p[6 + slot] = sum > 0.0 ? static_cast<float>(ws[slot] / sum) : (slot == 0 ? 1.0f : 0.0f);
      }
    }
  }

  double surface_elevation_m(const world::Dir3& unit_dir) const override {
    return field_.elevation_m(unit_dir).to_double();
  }

  // Centre + four corners of the column, through the shared column cache
  // (the province lattice makes the extra probes ~1 us each).
  void surface_elevation_range_m(const world::Dir3& center, std::uint8_t face, double u_center,
                                 double v_center, double half_uv, double* lo_m,
                                 double* hi_m) const override {
    const std::lock_guard<std::mutex> lock(cave_cache_mutex_);
    const auto sample = [&](const world::Dir3& dir) {
      const auto canonical = field_.canonical_params(dir_to_face_uv(dir), &cave_cache_);
      const BlendedParams params = TerrainField::to_blended(canonical);
      return field_.elevation_from_params(dir, params, canonical.macro_rel, &cave_cache_)
          .to_double();
    };
    double lo = sample(center);
    double hi = lo;
    for (int corner = 0; corner < 4; ++corner) {
      const double u = u_center + ((corner & 1) != 0 ? half_uv : -half_uv) * 0.98;
      const double v = v_center + ((corner & 2) != 0 ? half_uv : -half_uv) * 0.98;
      const double e = sample(face_uv_to_dir(FaceUV{face, det::Real(u), det::Real(v)}));
      lo = std::min(lo, e);
      hi = std::max(hi, e);
    }
    *lo_m = lo;
    *hi_m = hi;
  }

  int underground_intervals(const world::Dir3& unit_dir, DepthInterval* out,
                            int max_intervals) const override {
    if (!field_.caves().enabled()) {
      return 0;
    }
    TerrainField::CaveQuery query;
    {
      // The cave cache is shared across columns; the streamer calls this
      // from its update pass, workers never do.
      const std::lock_guard<std::mutex> lock(cave_cache_mutex_);
      field_.gather_caves(unit_dir, &cave_cache_, &query);
    }
    int count = 0;
    const double radius = field_.planet().radius_m.to_double();
    for (int s = 0; s < query.count && count < max_intervals; ++s) {
      double lo[CaveField::kMaxNodes + 4];
      double hi[CaveField::kMaxNodes + 4];
      const int n = CaveField::radial_intervals(*query.systems[s], unit_dir, lo, hi,
                                                CaveField::kMaxNodes + 4);
      for (int i = 0; i < n && count < max_intervals; ++i) {
        out[count].lo_m = lo[i] - radius;  // relative to the nominal radius,
        out[count].hi_m = hi[i] - radius;  // matching surface_elevation_m
        ++count;
      }
    }
    return count;
  }

 private:
  const TerrainField& field_;
  const world::EditStore* edits_;
  mutable TerrainField::ParamCache cave_cache_;
  mutable std::mutex cave_cache_mutex_;
};

}  // namespace inf::gen
