#pragma once

#include <cstdint>
#include <vector>

#include "gen/height_modifier.hpp"
#include "gen/sites.hpp"

namespace inf::gen {

// T0020 WP5: civil/v1 (design section 14.5) — the terrain under the
// settlement. A post-terrain height modifier and a material hint, inside
// each site's bound and along road corridors:
//  - plateau: blend the base elevation toward the site datum with a
//    smoothstep from radius 1.0 to 0.75 (terraces: toward the step);
//  - roads: within half the road width, blend to the centreline
//    elevation; shoulders over one width;
//  - never below sea: sites are placed above sea; roads never lower;
//  - material: paving/plating ids by race inside the site, disturbed
//    soil in the outer ring, urban albedo + night light for the bake.
// Immutable after construction: the sampler's worker threads read it.
class CivilField final : public HeightModifier {
 public:
  CivilField(const SiteField& sites, const TerrainField& field);

  det::Real modify(const Dir3& unit_dir, det::Real base_m, const BaseEval& base_at) const override;
  Urban urban(const Dir3& unit_dir) const override;
  bool near(const Dir3& unit_dir) const override;

  // The candidate provinces around a direction (the 3x3-ish probe stencil
  // used for both sites and roads).
  static constexpr int kMaxCandidates = 12;
  int candidates(const Dir3& unit_dir, std::uint32_t out[kMaxCandidates]) const;

  struct RoadSegment {
    Dir3 a, b;
    float width_m;
    float elevation_a_m;  // base terrain at the endpoints (road grade)
    float elevation_b_m;
  };
  const std::vector<RoadSegment>& road_segments() const { return segments_; }

 private:
  struct SiteHit {
    const Site* site;
    double dist_m;  // planar distance from the centre
    double x, y;    // site-local
  };
  bool find_site(const Dir3& unit_dir, SiteHit* hit) const;
  // Nearest road segment within `reach_m`, with the interpolated
  // centreline elevation and lateral distance.
  bool find_road(const Dir3& unit_dir, double* lateral_m, double* width_m,
                 double* centre_elevation_m) const;

  const SiteField& sites_;
  const TerrainField& field_;
  std::uint32_t n_{0};
  double radius_m_{0.0};
  double sea_m_{0.0};
  bool has_sea_{false};
  int material_family_{0};
  std::vector<RoadSegment> segments_;
  // Per province: indices of segments that can reach it.
  std::vector<std::vector<std::uint32_t>> province_segments_;
};

}  // namespace inf::gen
