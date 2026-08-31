#include "gen/galaxy_octree.hpp"

#include <cmath>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

// Deterministic Poisson draw: exact inversion for small lambda, normal
// approximation above (fixed iteration bounds — never
// iterate-to-tolerance).
std::uint32_t poisson(double lambda, std::uint64_t word_a, std::uint64_t word_b) {
  if (lambda <= 0.0) {
    return 0;
  }
  if (lambda < 16.0) {
    const double limit = det::fast_exp(Real(-lambda)).to_double();
    double product = u01(word_a).to_double();
    std::uint32_t count = 0;
    // Feed fresh uniforms by remixing; capped at 96 iterations (P(miss)
    // is astronomically small for lambda < 16).
    std::uint64_t state = word_b;
    while (product > limit && count < 96) {
      ++count;
      state = det::mix64(state + 0x9E3779B97F4A7C15ULL);
      product *= u01(state).to_double();
    }
    return count;
  }
  // Box-Muller normal approximation, clamped at 0.
  const double u1 = u01(word_a).to_double() + 1.0e-12;
  const double u2 = u01(word_b).to_double();
  const double radius = std::sqrt(-2.0 * det::fast_log(Real(u1)).to_double());
  Real sine(0.0);
  Real cosine(0.0);
  det::fast_sin_cos(Real(6.283185307179586 * u2), &sine, &cosine);
  const double value = lambda + std::sqrt(lambda) * radius * cosine.to_double();
  if (value <= 0.0) {
    return 0U;
  }
  if (value >= 4.0e9) {  // clamp: a count this size is a caller error anyway
    return 4000000000U;
  }
  return static_cast<std::uint32_t>(value + 0.5);
}

// Two-slope stellar luminosity function in log10-fraction space:
//   M in [5, 12]: log10 F = -0.18 * (12 - M)      (dwarf regime)
//   M <  5:       log10 F = -1.26 - 0.5 * (5 - M) (bright tail)
// F(12+) = 1. Roughly: 5% of stars brighter than the Sun, ~5e-5 brighter
// than a B star, ~5e-8 brighter than a supergiant.
double log10_fraction(double m) {
  if (m >= 12.0) {
    return 0.0;
  }
  if (m >= 5.0) {
    return -0.18 * (12.0 - m);
  }
  return -1.26 - 0.5 * (5.0 - m);
}

constexpr double kLn10 = 2.302585092994046;

}  // namespace

GalaxyOctree::GalaxyOctree(const core::Key& galaxy_entity_key, const GalaxyParams& params)
    : systems_key_(core::derive_named(galaxy_entity_key, name::GalaxySystemsV1)),
      density_(params) {
  // The root cube covers the disc plus halo margin.
  root_size_m_ = params.diameter_ly.to_double() * kLightYearM * 1.1;
  root_min_m_ = -0.5 * root_size_m_;
}

double GalaxyOctree::cell_size_m(std::int32_t level) const {
  return root_size_m_ / static_cast<double>(std::uint64_t{1} << level);
}

Dir3 GalaxyOctree::cell_min_m(const CellId& cell) const {
  const double s = cell_size_m(cell.level);
  return Dir3{Real(root_min_m_ + static_cast<double>(cell.x) * s),
              Real(root_min_m_ + static_cast<double>(cell.y) * s),
              Real(root_min_m_ + static_cast<double>(cell.z) * s)};
}

Dir3 GalaxyOctree::cell_center_m(const CellId& cell) const {
  const double s = cell_size_m(cell.level);
  const Dir3 lo = cell_min_m(cell);
  return Dir3{lo.x + Real(0.5 * s), lo.y + Real(0.5 * s), lo.z + Real(0.5 * s)};
}

core::Key GalaxyOctree::cell_key(const CellId& cell) const {
  return core::derive_child(systems_key_, kind::System, cell.x, cell.y, cell.z,
                            cell.level);
}

det::Real GalaxyOctree::expected_mass_suns(const CellId& cell) const {
  // Disc: 3x3 planar midpoint quadrature of the model's OWN analytic
  // z-column integral (a coarse cell spans many scale heights; sampling
  // z would miss the thin disc entirely). Spheroid: 3x3x3 midpoint
  // quadrature — compact terms, resolved once cells approach the bulge
  // scale, deliberately underestimated on the handful of top-level cells
  // (the unresolved core renders from the density integral, not from
  // systems).
  const double s = cell_size_m(cell.level);
  const Dir3 lo = cell_min_m(cell);
  const Real z0 = lo.z;
  const Real z1 = lo.z + Real(s);
  double column_sum = 0.0;
  for (int ix = 0; ix < 3; ++ix) {
    for (int iy = 0; iy < 3; ++iy) {
      column_sum += density_
                        .disc_column_mass(lo.x + Real((ix + 0.5) / 3.0 * s),
                                          lo.y + Real((iy + 0.5) / 3.0 * s), z0, z1)
                        .to_double();
    }
  }
  double spheroid_sum = 0.0;
  for (int ix = 0; ix < 3; ++ix) {
    for (int iy = 0; iy < 3; ++iy) {
      for (int iz = 0; iz < 3; ++iz) {
        const Dir3 p{lo.x + Real((ix + 0.5) / 3.0 * s), lo.y + Real((iy + 0.5) / 3.0 * s),
                     lo.z + Real((iz + 0.5) / 3.0 * s)};
        spheroid_sum += density_.spheroid(p).to_double();
      }
    }
  }
  return Real(column_sum * (1.0 / 9.0) * s * s +
              spheroid_sum * (1.0 / 27.0) * s * s * s);
}

det::Real GalaxyOctree::expected_systems(const CellId& cell) const {
  return Real(expected_mass_suns(cell).to_double() / kMeanSystemMassSuns);
}

bool GalaxyOctree::is_leaf(const CellId& cell) const {
  if (cell.level >= kMaxLevel) {
    return true;
  }
  return expected_systems(cell).to_double() <= kLeafOccupancy;
}

bool GalaxyOctree::occupied(const CellId& cell) const {
  const double lambda = expected_systems(cell).to_double();
  if (lambda <= 0.0) {
    return false;
  }
  const double p_occ = 1.0 - det::fast_exp(Real(-lambda)).to_double();
  const auto draw = core::draw_point(cell_key(cell), channel::Params, 0, 0, 0);
  return u01(draw[0]).to_double() < p_occ;
}

Dir3 GalaxyOctree::system_position_m(const CellId& cell) const {
  const auto draw = core::draw_point(cell_key(cell), channel::Params, 1, 0, 0);
  const double s = cell_size_m(cell.level);
  const Dir3 lo = cell_min_m(cell);
  return Dir3{lo.x + Real((0.04 + 0.92 * u01(draw[0]).to_double()) * s),
              lo.y + Real((0.04 + 0.92 * u01(draw[1]).to_double()) * s),
              lo.z + Real((0.04 + 0.92 * u01(draw[2]).to_double()) * s)};
}

GalaxyOctree::CellId GalaxyOctree::leaf_at(const Dir3& p_m) const {
  CellId cell{0, 0, 0, 0};
  const double inv = 1.0 / root_size_m_;
  double ux = (p_m.x.to_double() - root_min_m_) * inv;
  double uy = (p_m.y.to_double() - root_min_m_) * inv;
  double uz = (p_m.z.to_double() - root_min_m_) * inv;
  ux = ux < 0.0 ? 0.0 : (ux >= 1.0 ? 0.9999999999 : ux);
  uy = uy < 0.0 ? 0.0 : (uy >= 1.0 ? 0.9999999999 : uy);
  uz = uz < 0.0 ? 0.0 : (uz >= 1.0 ? 0.9999999999 : uz);
  while (!is_leaf(cell)) {
    const auto scale = static_cast<double>(std::uint64_t{1} << (cell.level + 1));
    cell = CellId{static_cast<std::int64_t>(ux * scale),
                  static_cast<std::int64_t>(uy * scale),
                  static_cast<std::int64_t>(uz * scale), cell.level + 1};
  }
  return cell;
}

void GalaxyOctree::systems_in_ball(const Dir3& center_m, det::Real radius_m,
                                   std::size_t max_out, std::vector<CellId>* out) const {
  const double radius = radius_m.to_double();
  const double cx = center_m.x.to_double();
  const double cy = center_m.y.to_double();
  const double cz = center_m.z.to_double();
  std::vector<CellId> stack;
  stack.push_back(CellId{0, 0, 0, 0});
  while (!stack.empty() && out->size() < max_out) {
    const CellId cell = stack.back();
    stack.pop_back();
    // Cube-ball rejection.
    const double s = cell_size_m(cell.level);
    const Dir3 lo = cell_min_m(cell);
    double dist_sq = 0.0;
    const double lox = lo.x.to_double();
    const double loy = lo.y.to_double();
    const double loz = lo.z.to_double();
    const auto axis = [&](double c, double lo_v) {
      const double hi_v = lo_v + s;
      const double d = c < lo_v ? lo_v - c : (c > hi_v ? c - hi_v : 0.0);
      dist_sq += d * d;
    };
    axis(cx, lox);
    axis(cy, loy);
    axis(cz, loz);
    if (dist_sq > radius * radius) {
      continue;
    }
    if (expected_systems(cell).to_double() <= 0.0) {
      continue;
    }
    if (is_leaf(cell)) {
      if (occupied(cell)) {
        const Dir3 pos = system_position_m(cell);
        const double dx = pos.x.to_double() - cx;
        const double dy = pos.y.to_double() - cy;
        const double dz = pos.z.to_double() - cz;
        if (dx * dx + dy * dy + dz * dz <= radius * radius) {
          out->push_back(cell);
        }
      }
      continue;
    }
    for (int child = 0; child < 8; ++child) {
      stack.push_back(CellId{cell.x * 2 + (child & 1), cell.y * 2 + ((child >> 1) & 1),
                             cell.z * 2 + ((child >> 2) & 1), cell.level + 1});
    }
  }
}

det::Real GalaxyOctree::luminous_fraction(det::Real abs_mag_limit) {
  const double lf = log10_fraction(abs_mag_limit.to_double());
  return det::fast_exp(Real(lf * kLn10));
}

std::uint32_t GalaxyOctree::luminous_count(const CellId& cell,
                                           det::Real abs_mag_limit) const {
  // Quantise the limit (0.5 mag) so the draw is stable and cacheable as
  // the viewer moves; offset keeps the counter index positive.
  const auto quantised =
      static_cast<std::int64_t>(std::floor(abs_mag_limit.to_double() * 2.0 + 0.5));
  const std::uint64_t sub = static_cast<std::uint64_t>(quantised + 64);
  const double fraction =
      luminous_fraction(Real(static_cast<double>(quantised) * 0.5)).to_double();
  const double lambda = expected_systems(cell).to_double() * fraction;
  const auto draw = core::draw_point(cell_key(cell), channel::Params, sub, 1, 0);
  return poisson(lambda, draw[0], draw[1]);
}

GalaxyOctree::StarSummary GalaxyOctree::luminous_star(const CellId& cell,
                                                      det::Real abs_mag_limit,
                                                      std::uint32_t index) const {
  const auto quantised =
      static_cast<std::int64_t>(std::floor(abs_mag_limit.to_double() * 2.0 + 0.5));
  const std::uint64_t sub = static_cast<std::uint64_t>(quantised + 64);
  const auto draw =
      core::draw_point(cell_key(cell), channel::Params, sub, 2, index + 1);
  const double s = cell_size_m(cell.level);
  const Dir3 lo = cell_min_m(cell);
  StarSummary star;
  star.position_m = Dir3{lo.x + Real(u01(draw[0]).to_double() * s),
                         lo.y + Real(u01(draw[1]).to_double() * s),
                         lo.z + Real(u01(draw[2]).to_double() * s)};
  // Magnitude from the truncated bright tail: invert the two-slope
  // luminosity function at u * F(limit).
  const double q_mag = static_cast<double>(quantised) * 0.5;
  const double u = u01(draw[3]).to_double() * (1.0 - 1.0e-12) + 1.0e-12;
  const double lf_limit = log10_fraction(q_mag);
  const double lf = lf_limit + det::fast_log(Real(u)).to_double() / kLn10;
  double mag;
  if (lf >= -1.26) {
    mag = 12.0 + lf / 0.18;
  } else {
    mag = 5.0 + (lf + 1.26) / 0.5;
  }
  star.abs_mag = Real(mag);
  // Rough main-sequence colour: blue supergiants ~25 000 K down to red
  // dwarfs below 3 000 K.
  double temperature =
      25000.0 * det::fast_exp(Real(-0.12 * (mag + 8.0))).to_double();
  temperature = temperature < 2600.0 ? 2600.0 : (temperature > 40000.0 ? 40000.0
                                                                       : temperature);
  star.temperature_k = Real(temperature);
  return star;
}

Dir3 home_system_position_m(const GalaxyParams& params) {
  // Mid-disc, slightly above the plane — the Sun-like vantage: inside
  // the disc for a real Milky Way band, far enough out for structure.
  const double radius = params.diameter_ly.to_double() * 0.5 * kLightYearM;
  const double height = params.thin_scale_height_ly.to_double() * kLightYearM;
  return Dir3{Real(0.55 * radius), Real(0.0), Real(0.08 * height)};
}

}  // namespace inf::gen
