#include "gen/deep_sky.hpp"

#include <cmath>

#include "core/det/trig.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

double uniform(std::uint64_t word, double lo, double hi) {
  return lo + (hi - lo) * u01(word).to_double();
}

// A representative "arm radius" sample for normalizing dust weights: mid
// disc, in the plane. The same point for every consumer, so weights are
// comparable across fields.
Dir3 dust_reference_point(const GalaxyDensity& density) {
  return Dir3{density.radius_m() * Real(0.45), Real(0.0), Real(0.0)};
}

}  // namespace

const char* to_string(NebulaType type) {
  switch (type) {
    case NebulaType::Emission: return "Emission";
    case NebulaType::Reflection: return "Reflection";
    case NebulaType::Dark: return "Dark";
    case NebulaType::Planetary: return "Planetary";
    case NebulaType::SupernovaRemnant: return "SupernovaRemnant";
  }
  return "?";
}

NebulaField::NebulaField(const core::Key& galaxy_entity_key, const GalaxyParams& params)
    : nebulae_key_(core::derive_named(galaxy_entity_key, name::NebulaeV1)),
      density_(params) {
  const double root = params.diameter_ly.to_double() * kLightYearM * 1.1;
  cell_size_m_ = root / static_cast<double>(1 << kGridLevel);
  root_min_m_ = -0.5 * root;
  dust_reference_ = density_.dust(dust_reference_point(density_)).to_double();
}

NebulaField::CellNebulae NebulaField::cell_nebulae(std::int64_t x, std::int64_t y,
                                                   std::int64_t z) const {
  CellNebulae out;
  if (dust_reference_ <= 0.0) {
    return out;  // an elliptical: no dust, no nebulae — geometry, not a knob
  }
  const Dir3 center{Real(root_min_m_ + (static_cast<double>(x) + 0.5) * cell_size_m_),
                    Real(root_min_m_ + (static_cast<double>(y) + 0.5) * cell_size_m_),
                    Real(root_min_m_ + (static_cast<double>(z) + 0.5) * cell_size_m_)};
  // Arm weighting: expected count follows the DUST density (strongest arm
  // contrast in the model) at the cell centre.
  const double weight = density_.dust(center).to_double() / dust_reference_;
  const double lambda = 1.1 * weight;
  if (lambda < 1.0e-4) {
    return out;
  }
  const core::Key cell_key = core::derive_child(nebulae_key_, kind::Nebula, x, y, z);
  const auto count_draw = core::draw_point(cell_key, channel::Params, 0, 0, 0);
  int count = static_cast<int>(lambda);
  if (u01(count_draw[0]).to_double() < lambda - static_cast<double>(count)) {
    ++count;
  }
  count = count > kMaxPerCell ? kMaxPerCell : count;
  const double ly = kLightYearM;
  for (int i = 0; i < count; ++i) {
    const auto draw_a = core::draw_point(cell_key, channel::Params, i, 1, 0);
    const auto draw_b = core::draw_point(cell_key, channel::Params, i, 2, 0);
    Nebula& nebula = out.items[out.count++];
    // Importance placement: the grid cell is ~kly-sized while the dust
    // layer is tens of ly thin and arm-shaped — a deterministic
    // 8-candidate tournament on the dust field puts the nebula IN an arm
    // instead of merely in an armful cell.
    // z comes from the dust VERTICAL PROFILE (Laplace at the dust scale
    // height, clamped to the cell): the dust layer is tens of ly thin
    // while the cell is ~a kly tall — uniform z would strand nebulae far
    // above the rift they are supposed to inhabit.
    const double h_dust =
        density_.params().dust_scale_height_ly.to_double() * kLightYearM;
    const double cell_z_lo = center.z.to_double() - 0.5 * cell_size_m_;
    const double cell_z_hi = center.z.to_double() + 0.5 * cell_size_m_;
    Dir3 best_pos = center;
    double best_dust = -1.0;
    for (std::uint64_t candidate = 0; candidate < 8; ++candidate) {
      const auto draw_c =
          core::draw_point(cell_key, channel::Params, i, 3, candidate);
      const double u_z = u01(draw_c[2]).to_double() * (1.0 - 1.0e-9) + 1.0e-9;
      const double sign = (draw_c[2] & 1U) != 0U ? 1.0 : -1.0;
      double z_neb =
          sign * 2.0 * h_dust * (-det::fast_log(Real(u_z)).to_double());
      z_neb = z_neb < cell_z_lo ? cell_z_lo : (z_neb > cell_z_hi ? cell_z_hi : z_neb);
      const Dir3 pos{
          center.x + Real((u01(draw_c[0]).to_double() - 0.5) * cell_size_m_),
          center.y + Real((u01(draw_c[1]).to_double() - 0.5) * cell_size_m_),
          Real(z_neb)};
      const double dust = density_.dust(pos).to_double();
      if (dust > best_dust) {
        best_dust = dust;
        best_pos = pos;
      }
    }
    nebula.center_m = best_pos;
    const std::uint32_t roll = static_cast<std::uint32_t>(draw_a[3] >> 40U) % 100U;
    // Emission 30, reflection 20, dark 25, planetary 15, remnant 10.
    if (roll < 30) {
      nebula.type = NebulaType::Emission;
      nebula.radius_m = Real(uniform(draw_b[0], 30.0, 150.0) * ly);
      nebula.color[0] = Real(0.9);
      nebula.color[1] = Real(uniform(draw_b[1], 0.2, 0.45));
      nebula.color[2] = Real(uniform(draw_b[2], 0.3, 0.55));
      nebula.opacity = Real(uniform(draw_b[3], 0.25, 0.6));
    } else if (roll < 50) {
      nebula.type = NebulaType::Reflection;
      nebula.radius_m = Real(uniform(draw_b[0], 10.0, 50.0) * ly);
      nebula.color[0] = Real(uniform(draw_b[1], 0.35, 0.55));
      nebula.color[1] = Real(uniform(draw_b[2], 0.5, 0.7));
      nebula.color[2] = Real(0.95);
      nebula.opacity = Real(uniform(draw_b[3], 0.15, 0.4));
    } else if (roll < 75) {
      nebula.type = NebulaType::Dark;
      nebula.radius_m = Real(uniform(draw_b[0], 25.0, 120.0) * ly);
      nebula.color[0] = Real(0.05);
      nebula.color[1] = Real(0.04);
      nebula.color[2] = Real(0.05);
      nebula.opacity = Real(uniform(draw_b[3], 0.5, 0.9));
    } else if (roll < 90) {
      nebula.type = NebulaType::Planetary;
      nebula.radius_m = Real(uniform(draw_b[0], 0.5, 3.0) * ly);
      nebula.color[0] = Real(uniform(draw_b[1], 0.3, 0.6));
      nebula.color[1] = Real(0.85);
      nebula.color[2] = Real(uniform(draw_b[2], 0.5, 0.8));
      nebula.opacity = Real(uniform(draw_b[3], 0.3, 0.7));
    } else {
      nebula.type = NebulaType::SupernovaRemnant;
      nebula.radius_m = Real(uniform(draw_b[0], 5.0, 30.0) * ly);
      nebula.color[0] = Real(0.85);
      nebula.color[1] = Real(uniform(draw_b[1], 0.4, 0.6));
      nebula.color[2] = Real(uniform(draw_b[2], 0.6, 0.9));
      nebula.opacity = Real(uniform(draw_b[3], 0.2, 0.5));
    }
    nebula.shape_seed = draw_b[0] ^ (draw_a[3] << 1U);
  }
  return out;
}

void NebulaField::nebulae_in_ball(const Dir3& center_m, det::Real radius_m,
                                  std::vector<Nebula>* out) const {
  const double radius = radius_m.to_double();
  const double extent = static_cast<double>(1 << kGridLevel);
  const auto to_cell = [&](double v) {
    double u = (v - root_min_m_) / cell_size_m_;
    u = u < 0.0 ? 0.0 : (u >= extent ? extent - 1.0 : u);
    return static_cast<std::int64_t>(u);
  };
  // One-cell margin: a nebula's centre stays inside its cell, its bound
  // can reach one cell over (max radius 150 ly << cell size).
  const std::int64_t x0 = to_cell(center_m.x.to_double() - radius) - 1;
  const std::int64_t x1 = to_cell(center_m.x.to_double() + radius) + 1;
  const std::int64_t y0 = to_cell(center_m.y.to_double() - radius) - 1;
  const std::int64_t y1 = to_cell(center_m.y.to_double() + radius) + 1;
  const std::int64_t z0 = to_cell(center_m.z.to_double() - radius) - 1;
  const std::int64_t z1 = to_cell(center_m.z.to_double() + radius) + 1;
  const auto limit = static_cast<std::int64_t>(extent) - 1;
  for (std::int64_t z = z0 < 0 ? 0 : z0; z <= (z1 > limit ? limit : z1); ++z) {
    for (std::int64_t y = y0 < 0 ? 0 : y0; y <= (y1 > limit ? limit : y1); ++y) {
      for (std::int64_t x = x0 < 0 ? 0 : x0; x <= (x1 > limit ? limit : x1); ++x) {
        const CellNebulae cell = cell_nebulae(x, y, z);
        for (int i = 0; i < cell.count; ++i) {
          const Nebula& nebula = cell.items[i];
          const double dx = nebula.center_m.x.to_double() - center_m.x.to_double();
          const double dy = nebula.center_m.y.to_double() - center_m.y.to_double();
          const double dz = nebula.center_m.z.to_double() - center_m.z.to_double();
          const double reach = radius + nebula.radius_m.to_double();
          if (dx * dx + dy * dy + dz * dz <= reach * reach) {
            out->push_back(nebula);
          }
        }
      }
    }
  }
}

StarClusterField::StarClusterField(const core::Key& galaxy_entity_key,
                                   const GalaxyParams& params)
    : clusters_key_(core::derive_named(galaxy_entity_key, name::StarClustersV1)),
      density_(params) {
  const double root = params.diameter_ly.to_double() * kLightYearM * 1.1;
  cell_size_m_ = root / static_cast<double>(1 << kGridLevel);
  root_min_m_ = -0.5 * root;
  galaxy_radius_m_ = params.diameter_ly.to_double() * 0.5 * kLightYearM;
  dust_reference_ = density_.dust(dust_reference_point(density_)).to_double();
  // Globular count scales weakly with mass (Milky Way ~150 at 6e10).
  const auto draw = core::draw_point(clusters_key_, channel::Params, 0, 0, 0);
  const double mass_scale =
      std::sqrt(params.total_mass_suns.to_double() / 6.0e10);
  globular_count_ = static_cast<int>(150.0 * mass_scale * uniform(draw[0], 0.6, 1.4));
  globular_count_ = globular_count_ < 8 ? 8 : (globular_count_ > 500 ? 500
                                                                     : globular_count_);
}

StarClusterField::CellClusters StarClusterField::cell_open_clusters(
    std::int64_t x, std::int64_t y, std::int64_t z) const {
  CellClusters out;
  if (dust_reference_ <= 0.0) {
    return out;  // no disc, no open clusters
  }
  const Dir3 center{Real(root_min_m_ + (static_cast<double>(x) + 0.5) * cell_size_m_),
                    Real(root_min_m_ + (static_cast<double>(y) + 0.5) * cell_size_m_),
                    Real(root_min_m_ + (static_cast<double>(z) + 0.5) * cell_size_m_)};
  const double weight = density_.dust(center).to_double() / dust_reference_;
  const double lambda = 0.8 * weight;
  if (lambda < 1.0e-4) {
    return out;
  }
  const core::Key cell_key =
      core::derive_child(clusters_key_, kind::StarCluster, x, y, z);
  const auto count_draw = core::draw_point(cell_key, channel::Params, 0, 0, 0);
  int count = static_cast<int>(lambda);
  if (u01(count_draw[0]).to_double() < lambda - static_cast<double>(count)) {
    ++count;
  }
  count = count > kMaxPerCell ? kMaxPerCell : count;
  for (int i = 0; i < count; ++i) {
    const auto draw = core::draw_point(cell_key, channel::Params, i, 1, 0);
    StarCluster& cluster = out.items[out.count++];
    cluster.globular = false;
    cluster.center_m =
        Dir3{center.x + Real((u01(draw[0]).to_double() - 0.5) * cell_size_m_),
             center.y + Real((u01(draw[1]).to_double() - 0.5) * cell_size_m_),
             center.z + Real((u01(draw[2]).to_double() - 0.5) * cell_size_m_ * 0.3)};
    cluster.radius_m = Real(uniform(draw[3], 5.0, 25.0) * kLightYearM);
    cluster.star_count = Real(uniform(draw[3] >> 17U, 100.0, 2000.0));
    cluster.age_gyr = Real(uniform(draw[0] >> 23U, 0.05, 1.5));  // young
    cluster.seed = draw[2];
  }
  return out;
}

StarCluster StarClusterField::globular(int index) const {
  const auto draw =
      core::draw_point(clusters_key_, channel::Params,
                       static_cast<std::uint64_t>(index) + 1, 2, 0);
  StarCluster cluster;
  cluster.globular = true;
  // Halo placement: concentrated toward the centre, spherical — the |z|
  // distribution follows the same law as x and y, unlike anything in the
  // disc.
  const double radius = galaxy_radius_m_ * 0.95 *
                        std::sqrt(u01(draw[0]).to_double()) *
                        std::sqrt(u01(draw[0] >> 7U).to_double());
  const double cos_theta = 2.0 * u01(draw[1]).to_double() - 1.0;
  const double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);
  Real sine(0.0);
  Real cosine(0.0);
  det::fast_sin_cos(Real(6.283185307179586 * u01(draw[2]).to_double()), &sine, &cosine);
  cluster.center_m = Dir3{Real(radius * sin_theta * cosine.to_double()),
                          Real(radius * sin_theta * sine.to_double()),
                          Real(radius * cos_theta)};
  cluster.radius_m = Real(uniform(draw[3], 8.0, 40.0) * kLightYearM);
  cluster.star_count = Real(uniform(draw[3] >> 19U, 5.0e4, 8.0e5));
  cluster.age_gyr = Real(uniform(draw[1] >> 13U, 9.0, 13.0));  // old
  cluster.seed = draw[2];
  return cluster;
}

void StarClusterField::clusters_in_ball(const Dir3& center_m, det::Real radius_m,
                                        std::vector<StarCluster>* out) const {
  const double radius = radius_m.to_double();
  const double extent = static_cast<double>(1 << kGridLevel);
  const auto to_cell = [&](double v) {
    double u = (v - root_min_m_) / cell_size_m_;
    u = u < 0.0 ? 0.0 : (u >= extent ? extent - 1.0 : u);
    return static_cast<std::int64_t>(u);
  };
  const std::int64_t x0 = to_cell(center_m.x.to_double() - radius) - 1;
  const std::int64_t x1 = to_cell(center_m.x.to_double() + radius) + 1;
  const std::int64_t y0 = to_cell(center_m.y.to_double() - radius) - 1;
  const std::int64_t y1 = to_cell(center_m.y.to_double() + radius) + 1;
  const std::int64_t z0 = to_cell(center_m.z.to_double() - radius) - 1;
  const std::int64_t z1 = to_cell(center_m.z.to_double() + radius) + 1;
  const auto limit = static_cast<std::int64_t>(extent) - 1;
  for (std::int64_t z = z0 < 0 ? 0 : z0; z <= (z1 > limit ? limit : z1); ++z) {
    for (std::int64_t y = y0 < 0 ? 0 : y0; y <= (y1 > limit ? limit : y1); ++y) {
      for (std::int64_t x = x0 < 0 ? 0 : x0; x <= (x1 > limit ? limit : x1); ++x) {
        const CellClusters cell = cell_open_clusters(x, y, z);
        for (int i = 0; i < cell.count; ++i) {
          const StarCluster& cluster = cell.items[i];
          const double dx = cluster.center_m.x.to_double() - center_m.x.to_double();
          const double dy = cluster.center_m.y.to_double() - center_m.y.to_double();
          const double dz = cluster.center_m.z.to_double() - center_m.z.to_double();
          const double reach = radius + cluster.radius_m.to_double();
          if (dx * dx + dy * dy + dz * dz <= reach * reach) {
            out->push_back(cluster);
          }
        }
      }
    }
  }
  for (int i = 0; i < globular_count_; ++i) {
    const StarCluster cluster = globular(i);
    const double dx = cluster.center_m.x.to_double() - center_m.x.to_double();
    const double dy = cluster.center_m.y.to_double() - center_m.y.to_double();
    const double dz = cluster.center_m.z.to_double() - center_m.z.to_double();
    const double reach = radius + cluster.radius_m.to_double();
    if (dx * dx + dy * dy + dz * dz <= reach * reach) {
      out->push_back(cluster);
    }
  }
}

}  // namespace inf::gen
