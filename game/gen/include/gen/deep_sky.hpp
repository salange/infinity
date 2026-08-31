#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/galaxy.hpp"

namespace inf::gen {

// T0017 WP4: nebulae/v1 and star-clusters/v1 — discrete deep-sky
// entities on the Galaxy node, built with the bounded feature-entity
// pattern from T0015 WP5 (centre + bounding radius on a coarse spatial
// grid, zero cost outside the bound) so the codebase keeps ONE idiom for
// bounded content. Placement is weighted by the shared density model's
// DUST term, which hugs the spiral arms hardest — nebulae belong in
// arms, ellipticals get essentially none, and that falls out of the
// geometry rather than a per-type knob. General interstellar dust is NOT
// an entity: it stays continuous in GalaxyDensity::dust.

enum class NebulaType : std::uint8_t {
  Emission = 0,
  Reflection = 1,
  Dark = 2,
  Planetary = 3,
  SupernovaRemnant = 4,
};

const char* to_string(NebulaType type);

struct Nebula {
  Dir3 center_m;          // galactocentric
  det::Real radius_m;     // bounding radius
  NebulaType type{NebulaType::Emission};
  det::Real color[3];     // characteristic emission/reflection tint
  det::Real opacity;      // [0, 1]
  std::uint64_t shape_seed{0};  // internal structure (T0018 renders it)
};

class NebulaField {
 public:
  NebulaField(const core::Key& galaxy_entity_key, const GalaxyParams& params);

  // Coarse placement grid: 2^kGridLevel cells per axis over the galaxy
  // bounding cube (same cube as the octree root).
  static constexpr int kGridLevel = 5;
  static constexpr int kMaxPerCell = 3;

  struct CellNebulae {
    Nebula items[kMaxPerCell];
    int count{0};
  };
  CellNebulae cell_nebulae(std::int64_t x, std::int64_t y, std::int64_t z) const;

  // Every nebula whose BOUND intersects the ball (bounded-entity query).
  void nebulae_in_ball(const Dir3& center_m, det::Real radius_m,
                       std::vector<Nebula>* out) const;

  double cell_size_m() const { return cell_size_m_; }

 private:
  core::Key nebulae_key_;
  GalaxyDensity density_;
  double cell_size_m_{0.0};
  double root_min_m_{0.0};
  double dust_reference_{0.0};  // dust density at a representative arm radius
};

// Open clusters ride the same grid idiom (young, disc, arm-correlated);
// globulars are an enumerable per-galaxy list distributed through the
// halo — old, spherical, not flattened to the disc.
struct StarCluster {
  Dir3 center_m;
  det::Real radius_m;
  bool globular{false};
  det::Real star_count;
  det::Real age_gyr;
  std::uint64_t seed{0};
};

class StarClusterField {
 public:
  StarClusterField(const core::Key& galaxy_entity_key, const GalaxyParams& params);

  static constexpr int kGridLevel = 5;
  static constexpr int kMaxPerCell = 2;

  struct CellClusters {
    StarCluster items[kMaxPerCell];
    int count{0};
  };
  // Open clusters of one grid cell.
  CellClusters cell_open_clusters(std::int64_t x, std::int64_t y, std::int64_t z) const;

  // Globulars: a small enumerable population (roughly 30-300, scaling
  // with galaxy mass), placed through the spherical halo.
  int globular_count() const { return globular_count_; }
  StarCluster globular(int index) const;

  void clusters_in_ball(const Dir3& center_m, det::Real radius_m,
                        std::vector<StarCluster>* out) const;

  double cell_size_m() const { return cell_size_m_; }

 private:
  core::Key clusters_key_;
  GalaxyDensity density_;
  double cell_size_m_{0.0};
  double root_min_m_{0.0};
  double dust_reference_{0.0};
  double galaxy_radius_m_{0.0};
  int globular_count_{0};
};

}  // namespace inf::gen
