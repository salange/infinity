#include "gen/terrain.hpp"

#include <bit>

#include "core/det/mix.hpp"
#include "core/golden.hpp"
#include "gen/noise.hpp"

namespace inf::gen {

using det::Real;

TerrainField::TerrainField(const core::Key& body_key, const PlanetParams& planet)
    : planet_(planet), provinces_(body_key, planet) {
  const core::Key terrain_key = core::derive_named(body_key, core::NameId::TerrainV1);
  elevation_lattice_ = core::lattice_key(terrain_key, core::Channel::Lattice);
  detail_lattice_ = det::mix64(elevation_lattice_ ^ 0xD3A11E77E44A1EB5ULL);
}

Real TerrainField::elevation_m(const Dir3& unit_dir) const {
  return elevation_from_params(unit_dir, provinces_.sample(unit_dir));
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

ChunkGrid ChunkGrid::from_addr(const core::ChunkAddr& addr, const PlanetParams& planet) {
  ChunkGrid grid;
  grid.addr = addr;
  const auto cells = static_cast<double>(std::uint64_t{1} << addr.lod);
  const double u_lo = -1.0 + 2.0 * static_cast<double>(addr.i) / cells;
  const double v_lo = -1.0 + 2.0 * static_cast<double>(addr.j) / cells;
  const double span = 2.0 / cells;
  grid.u0 = Real(u_lo);
  grid.u1 = Real(u_lo + span);
  grid.v0 = Real(v_lo);
  grid.v1 = Real(v_lo + span);

  // Lateral arc length of the chunk at surface radius ~ radius * span
  // (uv is roughly angle-linear per face). Radial thickness matches it.
  const Real thickness = planet.radius_m * Real(span);
  const Real shell(static_cast<double>(addr.shell));
  grid.r0 = planet.radius_m + shell * thickness - thickness * Real(0.5);
  grid.r1 = grid.r0 + thickness;
  return grid;
}

Dir3 ChunkGrid::corner_position(int gx, int gy, int gz) const {
  const Real fx(static_cast<double>(gx) / kVoxels);
  const Real fy(static_cast<double>(gy) / kVoxels);
  const Real fz(static_cast<double>(gz) / kVoxels);
  const Real u = det::lerp(u0, u1, fx);
  const Real v = det::lerp(v0, v1, fy);
  const Real r = det::lerp(r0, r1, fz);
  const Dir3 dir = face_uv_to_dir(FaceUV{addr.face, u, v});
  return Dir3{dir.x * r, dir.y * r, dir.z * r};
}

namespace {

// Shared implementation: samples grid coords [lo, hi] inclusive per axis
// (lo = -1 for the padded variant). The inner samples' op sequence is
// independent of the range, so padded and unpadded agree bit-exactly.
std::vector<Real> sample_range(const TerrainField& field, const ChunkGrid& grid, int lo,
                               int hi) {
  const auto count = static_cast<std::size_t>(hi - lo + 1);

  // Province parameters at the chunk's four uv-corners. Corners are shared
  // between neighboring chunks (same directions => same values), and
  // bilinear interpolation along a shared edge depends only on that edge's
  // endpoints — so the interpolated field is continuous across chunk
  // borders by construction. Provinces vary at kilometer scale; a chunk is
  // tens of meters wide, so the interpolation error is negligible.
  BlendedParams corner_params[2][2];
  for (int cu = 0; cu < 2; ++cu) {
    for (int cv = 0; cv < 2; ++cv) {
      const Dir3 dir = face_uv_to_dir(FaceUV{grid.addr.face, cu == 0 ? grid.u0 : grid.u1,
                                             cv == 0 ? grid.v0 : grid.v1});
      corner_params[cu][cv] = field.provinces().sample(dir);
    }
  }
  const auto bilerp = [&](Real BlendedParams::* member, Real fx, Real fy) {
    const Real a = det::lerp(corner_params[0][0].*member, corner_params[1][0].*member, fx);
    const Real b = det::lerp(corner_params[0][1].*member, corner_params[1][1].*member, fx);
    return det::lerp(a, b, fy);
  };

  std::vector<Real> densities(count * count * count, Real(0.0));
  // Elevation depends only on direction: one evaluation per (gx, gy)
  // column, reused by all radial layers.
  for (int gy = lo; gy <= hi; ++gy) {
    for (int gx = lo; gx <= hi; ++gx) {
      const Real fx(static_cast<double>(gx) / ChunkGrid::kVoxels);
      const Real fy(static_cast<double>(gy) / ChunkGrid::kVoxels);
      const Real u = det::lerp(grid.u0, grid.u1, fx);
      const Real v = det::lerp(grid.v0, grid.v1, fy);
      const Dir3 dir = face_uv_to_dir(FaceUV{grid.addr.face, u, v});

      BlendedParams params{};
      params.relief_amplitude_m = bilerp(&BlendedParams::relief_amplitude_m, fx, fy);
      params.base_elevation_m = bilerp(&BlendedParams::base_elevation_m, fx, fy);
      params.ruggedness = bilerp(&BlendedParams::ruggedness, fx, fy);
      params.carving = bilerp(&BlendedParams::carving, fx, fy);
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
