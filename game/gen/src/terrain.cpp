#include "gen/terrain.hpp"

#include <bit>
#include <cmath>

#include "core/det/mix.hpp"
#include "core/golden.hpp"
#include "gen/geo.hpp"

namespace inf::gen {

using det::Real;

TerrainField::TerrainField(const core::Key& body_key, const PlanetParams& planet)
    : planet_(planet), provinces_(body_key, planet) {
  const core::Key terrain_key = core::derive_named(body_key, name::TerrainV1);
  elevation_lattice_ = core::lattice_key(terrain_key, channel::Lattice);
  detail_lattice_ = det::mix64(elevation_lattice_ ^ 0xD3A11E77E44A1EB5ULL);
}

Real TerrainField::elevation_m(const Dir3& unit_dir) const {
  const CanonicalParams canonical = canonical_params(dir_to_face_uv(unit_dir));
  BlendedParams params{};
  params.relief_amplitude_m = canonical.relief_amplitude_m;
  params.base_elevation_m = canonical.base_elevation_m;
  params.ruggedness = canonical.ruggedness;
  params.carving = canonical.carving;
  return elevation_from_params(unit_dir, params);
}

TerrainField::CanonicalParams TerrainField::param_lattice_value(std::uint8_t face,
                                                               std::uint32_t ci,
                                                               std::uint32_t cj) const {
  const auto cells = static_cast<double>(kParamLatticeCells);
  const Real u(-1.0 + 2.0 * static_cast<double>(ci) / cells);
  const Real v(-1.0 + 2.0 * static_cast<double>(cj) / cells);
  const Dir3 dir = face_uv_to_dir(FaceUV{face, u, v});
  const BlendedParams blended = provinces_.sample(dir);
  return CanonicalParams{blended.relief_amplitude_m, blended.base_elevation_m,
                         blended.ruggedness, blended.carving};
}

TerrainField::CanonicalParams TerrainField::canonical_params(const FaceUV& face_uv,
                                                             ParamCache* cache) const {
  const auto lattice = [&](std::uint32_t ci, std::uint32_t cj) {
    if (cache == nullptr) {
      return param_lattice_value(face_uv.face, ci, cj);
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(face_uv.face) << 40U) |
                              (static_cast<std::uint64_t>(ci) << 20U) | cj;
    const auto it = cache->find(key);
    if (it != cache->end()) {
      return it->second;
    }
    const CanonicalParams value = param_lattice_value(face_uv.face, ci, cj);
    cache->emplace(key, value);
    return value;
  };
  const auto cells = static_cast<double>(kParamLatticeCells);
  auto locate = [&](Real coord, std::uint32_t* cell, Real* frac) {
    const double scaled = (coord.to_double() + 1.0) * 0.5 * cells;
    double base = std::floor(scaled);
    if (base < 0.0) base = 0.0;
    if (base > cells - 1.0) base = cells - 1.0;
    *cell = static_cast<std::uint32_t>(base);
    *frac = Real(scaled - base);
  };
  std::uint32_t ci = 0;
  std::uint32_t cj = 0;
  Real fu(0.0);
  Real fv(0.0);
  locate(face_uv.u, &ci, &fu);
  locate(face_uv.v, &cj, &fv);
  const CanonicalParams p00 = lattice(ci, cj);
  const CanonicalParams p10 = lattice(ci + 1, cj);
  const CanonicalParams p01 = lattice(ci, cj + 1);
  const CanonicalParams p11 = lattice(ci + 1, cj + 1);
  auto bilerp = [&](Real CanonicalParams::* member) {
    const Real a = det::lerp(p00.*member, p10.*member, fu);
    const Real b = det::lerp(p01.*member, p11.*member, fu);
    return det::lerp(a, b, fv);
  };
  return CanonicalParams{bilerp(&CanonicalParams::relief_amplitude_m),
                         bilerp(&CanonicalParams::base_elevation_m),
                         bilerp(&CanonicalParams::ruggedness),
                         bilerp(&CanonicalParams::carving)};
}

Real TerrainField::elevation_from_params(const Dir3& unit_dir,
                                         const BlendedParams& params) const {
  // Noise domain: unit direction scaled so one lattice cell spans roughly
  // one-third of a province cell (features live inside provinces).
  const Real frequency(3.0 * static_cast<double>(provinces_.cells_per_face()));

  FbmParams fbm;
  fbm.octaves = 6;
  // Ruggedness drives per-octave persistence and crest sharpness;
  // carving drives domain warp (coastline/valley wander — Murray's trick).
  fbm.gain = Real(0.4) + params.ruggedness * Real(0.25);
  fbm.sharpness = params.ruggedness * Real(0.7);
  fbm.octave0_damp = Real(0.5);
  const Real warp = params.carving * Real(0.8);

  const Real noise = warped_fbm3(elevation_lattice_, unit_dir.x * frequency,
                                 unit_dir.y * frequency, unit_dir.z * frequency, fbm, warp);
  return params.base_elevation_m + noise * params.relief_amplitude_m;
}

Real TerrainField::detail_m(const Dir3& position_m) const {
  const Real kilo(0.001);
  return gradient_noise3(detail_lattice_, position_m.x * kilo * Real(50.0),
                         position_m.y * kilo * Real(50.0), position_m.z * kilo * Real(50.0)) *
         Real(2.0);
}

Real TerrainField::density(const Dir3& position_m) const {
  const Real radius_sq = dot(position_m, position_m);
  const Real radius = det::sqrt(radius_sq);
  if (radius <= planet_.core_radius_m) {
    // Impenetrable core: solid with a wide margin.
    return Real(1.0e9);
  }
  const Dir3 unit_dir{position_m.x / radius, position_m.y / radius, position_m.z / radius};
  const Real surface_r = planet_.radius_m + elevation_m(unit_dir);
  return (surface_r - radius) + detail_m(position_m);
}

Real TerrainField::ground_radius_m(const Dir3& unit_dir) const {
  const Real surface = planet_.radius_m + elevation_m(unit_dir);
  // density(r) = (surface - r) + detail(dir * r); detail is bounded by
  // +-2 m, so the zero crossing lies within surface +- 4 m. Bisect on the
  // cheap detail-only expression (elevation already folded into surface).
  const auto density_at = [&](Real r) {
    const Dir3 position{unit_dir.x * r, unit_dir.y * r, unit_dir.z * r};
    return (surface - r) + detail_m(position);
  };
  Real lo = surface - Real(4.0);   // below: expect solid (density > 0)
  Real hi = surface + Real(4.0);   // above: expect air (density < 0)
  if (density_at(lo) <= Real(0.0) || density_at(hi) >= Real(0.0)) {
    // Bracket failed (extreme detail constellation): widen once, then
    // fall back to the elevation surface.
    lo = surface - Real(8.0);
    hi = surface + Real(8.0);
    if (density_at(lo) <= Real(0.0) || density_at(hi) >= Real(0.0)) {
      return surface;
    }
  }
  for (int i = 0; i < 24; ++i) {
    const Real mid = (lo + hi) * Real(0.5);
    if (density_at(mid) > Real(0.0)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return (lo + hi) * Real(0.5);
}


namespace {

// Shared implementation: samples grid coords [lo, hi] inclusive per axis
// (lo = -1 for the padded variant). The inner samples' op sequence is
// independent of the range, so padded and unpadded agree bit-exactly.
std::vector<Real> sample_range(const TerrainField& field, const ChunkGrid& grid, int lo,
                               int hi) {
  const auto count = static_cast<std::size_t>(hi - lo + 1);
  // Canonical global param lattice (see TerrainField::canonical_params):
  // every chunk computes bit-identical parameter values at a given world
  // direction, so densities agree exactly across all chunk/lod seams.
  TerrainField::ParamCache param_cache;

  std::vector<Real> densities(count * count * count, Real(0.0));
  // Elevation depends only on direction: one evaluation per (gx, gy)
  // column, reused by all radial layers.
  for (int gy = lo; gy <= hi; ++gy) {
    for (int gx = lo; gx <= hi; ++gx) {
      const Real fx(static_cast<double>(gx) / ChunkGrid::kVoxels);
      const Real fy(static_cast<double>(gy) / ChunkGrid::kVoxels);
      const Real u = det::lerp(grid.u0, grid.u1, fx);
      const Real v = det::lerp(grid.v0, grid.v1, fy);
      const FaceUV face_uv{grid.addr.face, u, v};
      const Dir3 dir = face_uv_to_dir(face_uv);

      const TerrainField::CanonicalParams canonical =
          field.canonical_params(face_uv, &param_cache);
      BlendedParams params{};
      params.relief_amplitude_m = canonical.relief_amplitude_m;
      params.base_elevation_m = canonical.base_elevation_m;
      params.ruggedness = canonical.ruggedness;
      params.carving = canonical.carving;
      const Real surface_r = field.planet().radius_m + field.elevation_from_params(dir, params);

      for (int gz = lo; gz <= hi; ++gz) {
        const Real fz(static_cast<double>(gz) / ChunkGrid::kVoxels);
        const Real r = det::lerp(grid.r0, grid.r1, fz);
        const Dir3 position{dir.x * r, dir.y * r, dir.z * r};
        Real density = r <= field.planet().core_radius_m
                           ? Real(1.0e9)
                           : (surface_r - r) + field.detail_m(position);
        densities[(static_cast<std::size_t>(gz - lo) * count +
                   static_cast<std::size_t>(gy - lo)) *
                      count +
                  static_cast<std::size_t>(gx - lo)] = density;
      }
    }
  }
  return densities;
}

}  // namespace

std::vector<Real> sample_chunk_density(const TerrainField& field, const ChunkGrid& grid) {
  return sample_range(field, grid, 0, static_cast<int>(ChunkGrid::kVoxels));
}

PaddedDensity sample_chunk_density_padded(const TerrainField& field, const ChunkGrid& grid) {
  PaddedDensity padded;
  padded.values = sample_range(field, grid, -1, static_cast<int>(ChunkGrid::kVoxels) + 1);
  return padded;
}

std::uint64_t hash_chunk_density(const TerrainField& field, const ChunkGrid& grid) {
  core::GoldenHash hash;
  for (const Real density : sample_chunk_density(field, grid)) {
    hash.feed(std::bit_cast<std::uint64_t>(density.to_double()));
  }
  return hash.value();
}

}  // namespace inf::gen
