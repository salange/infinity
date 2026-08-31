#include "gen/terrain.hpp"

#include <bit>
#include <cmath>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "core/golden.hpp"
#include "gen/geo.hpp"

namespace inf::gen {

using det::Real;

TerrainField::TerrainField(const core::Key& body_key, const PlanetParams& planet)
    : planet_(planet), provinces_(body_key, planet), macro_(body_key),
      material_(body_key, planet), features_(body_key, planet),
      caves_(body_key, planet), drainage_(body_key, planet) {
  // terrain/v2 (T0015 WP1): the layer name is bumped because the output
  // now composes macro/v1 — per the seeding spec's versioning rule, a
  // behaviour change is a NEW name, never a silent redefinition.
  const core::Key terrain_key = core::derive_named(body_key, name::TerrainV2);
  elevation_lattice_ = core::lattice_key(terrain_key, channel::Lattice);
  detail_lattice_ = det::mix64(elevation_lattice_ ^ 0xD3A11E77E44A1EB5ULL);
  // Per-body anisotropy direction for the detail term (prevailing-wind /
  // lineation stand-in until a climate field provides one).
  {
    const std::uint64_t a = det::mix64(detail_lattice_ ^ 0xA717);
    const std::uint64_t b = det::mix64(detail_lattice_ ^ 0xB818);
    const Real ax(static_cast<double>(a >> 11U) * 0x1.0p-53 * 2.0 - 1.0);
    const Real ay(static_cast<double>(b >> 11U) * 0x1.0p-53 * 2.0 - 1.0);
    const Real az(0.35);
    const Real len = det::sqrt(ax * ax + ay * ay + az * az);
    detail_axis_ = Dir3{ax / len, ay / len, az / len};
  }
}

Real TerrainField::elevation_m(const Dir3& unit_dir) const {
  const CanonicalParams canonical = canonical_params(dir_to_face_uv(unit_dir));
  BlendedParams params = TerrainField::to_blended(canonical);
  return elevation_from_params(unit_dir, params, canonical.macro_rel);
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
                         blended.ruggedness,        blended.carving,
                         blended.terrace_amount,    blended.terrace_step_m,
                         blended.dune_amount,       Real(0.0)};
}

TerrainField::CanonicalParams TerrainField::canonical_params(const FaceUV& face_uv,
                                                             ParamCache* cache) const {
  const auto lattice = [&](std::uint32_t ci, std::uint32_t cj) {
    if (cache == nullptr) {
      return param_lattice_value(face_uv.face, ci, cj);
    }
    const std::uint64_t key = (static_cast<std::uint64_t>(face_uv.face) << 40U) |
                              (static_cast<std::uint64_t>(ci) << 20U) | cj;
    const auto it = cache->params.find(key);
    if (it != cache->params.end()) {
      return it->second;
    }
    const CanonicalParams value = param_lattice_value(face_uv.face, ci, cj);
    cache->params.emplace(key, value);
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
                         bilerp(&CanonicalParams::carving),
                         bilerp(&CanonicalParams::terrace_amount),
                         bilerp(&CanonicalParams::terrace_step_m),
                         bilerp(&CanonicalParams::dune_amount),
                         macro_.canonical_value(face_uv, cache != nullptr ? &cache->macro
                                                                          : nullptr)};
}

namespace {

// One place derives the noise controls from the blended province params —
// the value and derivative paths must never drift apart.
struct NoiseControls {
  FbmParams fbm;
  Real frequency;
  Real warp;
};

NoiseControls noise_controls(const BlendedParams& params, std::uint32_t cells_per_face,
                             double radius_m) {
  NoiseControls controls;
  // Noise domain: unit direction scaled so one lattice cell spans roughly
  // one-third of a province cell (features live inside provinces).
  controls.frequency = Real(3.0 * static_cast<double>(cells_per_face));
  // T0015 WP4: radius-dependent octave count so the finest octave lands
  // near ~3 m regardless of body size (the old fixed 6 bottomed out at
  // ~563 m — nothing at walking scale). Pure halving loop, no libm.
  {
    double wavelength = radius_m / (3.0 * static_cast<double>(cells_per_face));
    int octaves = 1;
    while (wavelength > 3.0 && octaves < 12) {
      wavelength *= 0.5;
      ++octaves;
    }
    controls.fbm.octaves = octaves < 6 ? 6 : octaves;
    // Keep the first six octaves at their v1 amplitudes; the added
    // fine octaves contribute walking-scale detail on top instead of diluting the
    // mountain-scale bands.
    controls.fbm.normalize_octaves = 6;
  }
  // Ruggedness drives per-octave persistence and crest sharpness;
  // carving drives domain warp (coastline/valley wander — Murray's trick).
  controls.fbm.gain = Real(0.4) + params.ruggedness * Real(0.25);
  controls.fbm.sharpness = params.ruggedness * Real(0.7);
  controls.fbm.octave0_damp = Real(0.5);
  controls.warp = params.carving * Real(0.8);
  return controls;
}

}  // namespace

Real TerrainField::elevation_from_params(const Dir3& unit_dir,
                                         const BlendedParams& params) const {
  return elevation_from_params(unit_dir, params,
                               macro_.canonical_value(dir_to_face_uv(unit_dir)));
}

// Shared evaluator: base composition (macro + attenuated province fBm),
// the analytic tangent gradient of the noise term, then the T0015 WP2
// erosion operators applied against that gradient:
//  - talus / slope-limiting (5.1): h -= k * max(0, |grad h| - tan(theta)) * L
//    with theta blended from ruggedness (sand ~34deg .. competent rock
//    ~50deg). Fakes thermal erosion end-states pointwise.
//  - gradient-oriented ravines (5.3, phasor-lite): a groove oscillation
//    whose crests run ALONG the downhill direction (phase coordinate is
//    the position projected on the tangent perpendicular to the slope),
//    jittered by the base noise, windowed to mid slopes. Pointwise,
//    resolution-independent, orientation from the analytic derivative —
//    the Grenier-2024 idea reduced to one oscillation.
// The RETURNED gradient covers the base term only (erosion terms are
// small local corrections; materials/talus consumers tolerate the
// approximation, and the provable derivative contract lives in
// elevation_base_from_params + the noise-level tests).
Real TerrainField::evaluate_elevation(const Dir3& unit_dir, const BlendedParams& params,
                                      Real macro_rel, Dir3* slope_out,
                                      ParamCache* cache) const {
  const NoiseControls controls = noise_controls(params, provinces_.cells_per_face(),
                                               planet_.radius_m.to_double());
  const world::NoiseD noise = world::warped_fbm3_d(
      elevation_lattice_, unit_dir.x * controls.frequency, unit_dir.y * controls.frequency,
      unit_dir.z * controls.frequency, controls.fbm, controls.warp);

  const Real macro_m = macro_rel * planet_.macro_amplitude_m;
  const Real above_sea = macro_m - planet_.sea_level_m;
  const Real t = det::clamp((above_sea + Real(2000.0)) / Real(2200.0), Real(0.0), Real(1.0));
  const Real smooth = t * t * (Real(3.0) - (t + t));
  const Real attenuation = Real(0.15) + Real(0.85) * smooth;
  Real height = macro_m +
                attenuation * (params.base_elevation_m + noise.value * params.relief_amplitude_m);

  // Tangent slope of the noise term, metres per metre.
  const Real k = attenuation * params.relief_amplitude_m * controls.frequency /
                 planet_.radius_m;
  const Dir3 gradient{noise.dx * k, noise.dy * k, noise.dz * k};
  const Real radial = dot(gradient, unit_dir);
  const Dir3 slope{gradient.x - unit_dir.x * radial, gradient.y - unit_dir.y * radial,
                   gradient.z - unit_dir.z * radial};
  if (slope_out != nullptr) {
    *slope_out = slope;
  }
  // Erosion reacts to a SMOOTHED slope: the full 12-octave gradient
  // wiggles at 3 m scale and would turn both operators into noise (the
  // Grenier paper likewise orients by a smoothed gradient). Four
  // unwarped octaves = the landform-scale flow field.
  world::FbmParams coarse_fbm = controls.fbm;
  coarse_fbm.octaves = 4;
  coarse_fbm.sharpness = Real(0.0);
  coarse_fbm.normalize_octaves = 0;
  const world::NoiseD flow =
      world::fbm3_d(elevation_lattice_, unit_dir.x * controls.frequency,
                    unit_dir.y * controls.frequency, unit_dir.z * controls.frequency,
                    coarse_fbm);
  const Dir3 flow_grad{flow.dx * k, flow.dy * k, flow.dz * k};
  const Real flow_radial = dot(flow_grad, unit_dir);
  const Dir3 flow_slope{flow_grad.x - unit_dir.x * flow_radial,
                        flow_grad.y - unit_dir.y * flow_radial,
                        flow_grad.z - unit_dir.z * flow_radial};
  const Real slope_mag = det::sqrt(dot(flow_slope, flow_slope));

  // --- talus (thermal erosion end-state) -------------------------------
  const Real tan_talus = Real(0.30) + params.ruggedness * Real(0.35);
  const Real excess = det::max(Real(0.0), slope_mag - tan_talus);
  height = height - det::min(excess, Real(1.2)) * Real(0.85) * Real(140.0);

  // --- gradient-oriented ravines (only above water, mid slopes) --------
  const Real window = det::clamp((slope_mag - Real(0.03)) / Real(0.06), Real(0.0), Real(1.0)) *
                      det::clamp((Real(0.9) - slope_mag) / Real(0.4), Real(0.0), Real(1.0)) *
                      det::clamp((height - planet_.sea_level_m) / Real(300.0), Real(0.0),
                                 Real(1.0));
  if (window > Real(0.001) && slope_mag > Real(1.0e-9)) {
    // Tangent direction perpendicular to the downhill direction.
    const Dir3 perp{unit_dir.y * flow_slope.z - unit_dir.z * flow_slope.y,
                    unit_dir.z * flow_slope.x - unit_dir.x * flow_slope.z,
                    unit_dir.x * flow_slope.y - unit_dir.y * flow_slope.x};
    const Real perp_len = det::sqrt(dot(perp, perp));
    const Real inv_len = Real(1.0) / perp_len;
    const Real radius = planet_.radius_m;
    // Phase coordinate: position (metres) projected across the flow.
    const Real px = unit_dir.x * radius;
    const Real py = unit_dir.y * radius;
    const Real pz = unit_dir.z * radius;
    const Real along_perp =
        (px * perp.x + py * perp.y + pz * perp.z) * inv_len;
    const Real jitter = flow.value * Real(2.2);
    const Real phase = along_perp * Real(6.28318530717958647692 / 190.0) + jitter;
    const Real wave = det::sin(phase);
    const Real groove = (Real(0.5) + Real(0.5) * wave);
    const Real cut = groove * groove * groove;
    height = height - cut * window * params.carving * Real(26.0);
  }

  // --- drainage/v1 (T0015 WP6): valley carve toward the river network.
  // Direction-only, like everything above. High-carving provinces
  // (Canyon, Canyonlands) cut disproportionately deep gorges (WP8); the
  // clamp keeps every valley floor ~3 m over the sea.
  if (drainage_.enabled()) {
    const Real above = height - planet_.sea_level_m;
    Real carve = drainage_.carve_m(unit_dir, above);
    carve = det::min(carve * (Real(1.0) + params.carving * Real(1.2)),
                     det::max(Real(0.0), above - Real(3.0)));
    height = height - carve;
  }

  // --- terraces / mesas / plateau scarps (T0015 WP8) ------------------
  // h' = lerp(h, banded(h), amount): flat treads over ~2/3 of each band,
  // a C1 riser over the rest — mesas on Desert, scarp-edged plateaus on
  // highlands, stepped canyon walls after the drainage cut. Amount and
  // step blend continuously across province borders, so the operator
  // fades in and out without seams.
  if (params.terrace_amount > Real(0.001) && params.terrace_step_m > Real(1.0)) {
    const Real step = det::max(params.terrace_step_m, Real(20.0));
    const Real bands = height / step;
    const Real base_band(std::floor(bands.to_double()));
    const Real f = bands - base_band;
    Real riser = det::clamp((f - Real(0.65)) / Real(0.35), Real(0.0), Real(1.0));
    riser = riser * riser * (Real(3.0) - (riser + riser));
    const Real terraced = (base_band + riser) * step;
    const Real amount = det::clamp(params.terrace_amount, Real(0.0), Real(1.0));
    height = height + (terraced - height) * amount;
  }

  // --- dunes (T0015 WP8) ----------------------------------------------
  // Asymmetric sawtooth ridges across the per-body wind axis (the WP4
  // anisotropy axis; a per-province wind is the documented upgrade):
  // a long shallow windward slope, a short slip face near the WP2 talus
  // angle, rows jittered by the coarse flow noise.
  if (params.dune_amount > Real(0.001)) {
    const Real radius = planet_.radius_m;
    const Real along = (unit_dir.x * detail_axis_.x + unit_dir.y * detail_axis_.y +
                        unit_dir.z * detail_axis_.z) *
                       radius;
    const Real cycles = along / Real(190.0) + flow.value * Real(1.3);
    const Real t = cycles - Real(std::floor(cycles.to_double()));
    const Real profile =
        t < Real(0.72) ? t / Real(0.72) : (Real(1.0) - t) / Real(0.28);
    height = height + params.dune_amount * Real(13.0) * (profile - Real(0.5));
  }

  // --- features/v1 (T0015 WP5): bounded surface entities, craters first.
  // A separately keyed layer reading this one's inputs — bodies without
  // features (EarthLike) skip it behind a single branch, so their output
  // and cost are untouched (extension-safety rule).
  if (features_.enabled()) {
    height = height + features_.height_offset_m(unit_dir,
                                                cache != nullptr ? &cache->features : nullptr);
  }
  return height;
}

Real TerrainField::elevation_base_from_params(const Dir3& unit_dir,
                                              const BlendedParams& params,
                                              Real macro_rel) const {
  const NoiseControls controls = noise_controls(params, provinces_.cells_per_face(),
                                               planet_.radius_m.to_double());
  const Real noise = warped_fbm3(elevation_lattice_, unit_dir.x * controls.frequency,
                                 unit_dir.y * controls.frequency,
                                 unit_dir.z * controls.frequency, controls.fbm,
                                 controls.warp);
  const Real macro_m = macro_rel * planet_.macro_amplitude_m;
  const Real above_sea = macro_m - planet_.sea_level_m;
  const Real t = det::clamp((above_sea + Real(2000.0)) / Real(2200.0), Real(0.0), Real(1.0));
  const Real smooth = t * t * (Real(3.0) - (t + t));
  const Real attenuation = Real(0.15) + Real(0.85) * smooth;
  return macro_m +
         attenuation * (params.base_elevation_m + noise * params.relief_amplitude_m);
}

Real TerrainField::elevation_from_params(const Dir3& unit_dir, const BlendedParams& params,
                                         Real macro_rel) const {
  return evaluate_elevation(unit_dir, params, macro_rel, nullptr);
}

Real TerrainField::elevation_from_params(const Dir3& unit_dir, const BlendedParams& params,
                                         Real macro_rel, ParamCache* cache) const {
  return evaluate_elevation(unit_dir, params, macro_rel, nullptr, cache);
}

TerrainField::ElevationD TerrainField::elevation_and_gradient(const Dir3& unit_dir) const {
  const CanonicalParams canonical = canonical_params(dir_to_face_uv(unit_dir));
  BlendedParams params = TerrainField::to_blended(canonical);
  ElevationD out;
  out.elevation_m = evaluate_elevation(unit_dir, params, canonical.macro_rel, &out.slope);
  return out;
}

Real TerrainField::detail_m(const Dir3& position_m) const {
  // T0015 WP4: the fixed +-2 m isotropic wobble becomes type-dependent —
  // dunes want a directional ripple, ice wants fracture lineation,
  // regolith wants coarser pitting. Anisotropy: compress the domain
  // along a per-body direction so features elongate across it.
  double amplitude;
  double inv_wavelength;
  double anisotropy;
  switch (planet_.type) {
    case PlanetType::Desert: amplitude = 2.6; inv_wavelength = 1.0 / 34.0; anisotropy = 0.8; break;
    case PlanetType::Ice: amplitude = 2.0; inv_wavelength = 1.0 / 26.0; anisotropy = 0.65; break;
    case PlanetType::Barren: amplitude = 2.8; inv_wavelength = 1.0 / 30.0; anisotropy = 0.0; break;
    case PlanetType::EarthLike:
    default: amplitude = 1.6; inv_wavelength = 1.0 / 20.0; anisotropy = 0.0; break;
  }
  Real x = position_m.x;
  Real y = position_m.y;
  Real z = position_m.z;
  if (anisotropy > 0.0) {
    const Real along = x * detail_axis_.x + y * detail_axis_.y + z * detail_axis_.z;
    const Real squeeze = along * Real(anisotropy);
    x = x - detail_axis_.x * squeeze;
    y = y - detail_axis_.y * squeeze;
    z = z - detail_axis_.z * squeeze;
  }
  const Real f(inv_wavelength);
  return gradient_noise3(detail_lattice_, x * f, y * f, z * f) * Real(amplitude);
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
  const Real density = (surface_r - radius) + detail_m(position_m);
  if (!caves_.enabled()) {
    return density;
  }
  CaveQuery query;
  gather_caves(unit_dir, nullptr, &query);
  return apply_caves(position_m, surface_r, density, query);
}

const CaveField::System* TerrainField::cached_system(const CellId& cell, ParamCache* cache,
                                                     CaveField::System* storage) const {
  const auto build = [&]() {
    // The system needs the surface radius at its anchor (and mouth) —
    // pure functions of direction, so both clients agree bit-exactly.
    const Real surface_anchor = planet_.radius_m + elevation_m(caves_.anchor_dir(cell));
    const Dir3 mouth_dir = caves_.mouth_probe_dir(cell, surface_anchor);
    const Real surface_mouth = planet_.radius_m + elevation_m(mouth_dir);
    return caves_.build_system(cell, surface_anchor, surface_mouth);
  };
  if (cache == nullptr) {
    if (!caves_.hosted(cell)) {
      return nullptr;
    }
    *storage = build();
    return storage;
  }
  const std::uint64_t packed = (static_cast<std::uint64_t>(cell.face) << 40U) |
                               (static_cast<std::uint64_t>(cell.ci) << 20U) | cell.cj;
  const auto it = cache->caves.find(packed);
  if (it != cache->caves.end()) {
    return it->second.hosted ? &it->second : nullptr;
  }
  const CaveField::System system = caves_.hosted(cell) ? build() : CaveField::System{};
  const auto& stored = cache->caves.emplace(packed, system).first->second;
  return stored.hosted ? &stored : nullptr;
}

void TerrainField::gather_caves(const Dir3& unit_dir, ParamCache* cache,
                                CaveQuery* out) const {
  out->count = 0;
  if (!caves_.enabled()) {
    return;
  }
  CellId cells[CaveField::kMaxCandidates];
  const int cell_count = caves_.candidates(unit_dir, cells);
  for (int c = 0; c < cell_count; ++c) {
    const CaveField::System* system =
        cached_system(cells[c], cache, &out->storage[out->count]);
    if (system != nullptr) {
      out->systems[out->count++] = system;
    }
  }
}

Real TerrainField::apply_caves(const Dir3& position_m, Real surface_r, Real density,
                               const CaveQuery& query) const {
  if (query.count == 0) {
    return density;
  }
  // Caves live in a shallow band under (and, at mouths, slightly above)
  // the surface — everything else skips the SDF work entirely.
  const Real r = det::sqrt(dot(position_m, position_m));
  if (r < surface_r - Real(CaveField::kDepthBudgetM + 60.0) || r > surface_r + Real(90.0)) {
    return density;
  }
  for (int i = 0; i < query.count; ++i) {
    density = smin(density, CaveField::system_sdf(*query.systems[i], position_m),
                   Real(CaveField::kSminM));
  }
  return density;
}

Real TerrainField::cave_depth_budget_m(const Dir3& unit_dir) const {
  if (!caves_.enabled()) {
    return Real(0.0);
  }
  CellId cells[CaveField::kMaxCandidates];
  const int cell_count = caves_.candidates(unit_dir, cells);
  const Real reach((CaveField::kBoundCapM + 40.0) / planet_.radius_m.to_double());
  for (int c = 0; c < cell_count; ++c) {
    if (!caves_.hosted(cells[c])) {
      continue;
    }
    if (chord_sq(unit_dir, caves_.anchor_dir(cells[c])) < reach * reach) {
      return Real(CaveField::kDepthBudgetM);
    }
  }
  return Real(0.0);
}

Real TerrainField::ground_radius_m(const Dir3& unit_dir) const {
  const Real surface = planet_.radius_m + elevation_m(unit_dir);
  // density(r) = (surface - r) + detail(dir * r); detail is bounded by
  // +-3 m, so the zero crossing lies within surface +- 6 m. Bisect on the
  // cheap detail-only expression (elevation already folded into surface).
  const auto density_at = [&](Real r) {
    const Dir3 position{unit_dir.x * r, unit_dir.y * r, unit_dir.z * r};
    return (surface - r) + detail_m(position);
  };

  // WP7 Blocker B: caves break the single-crossing assumption. When a
  // system's bound can meet this radial, scan down from above the surface
  // with a fixed step and return the TOPMOST crossing — a mouth column
  // then reports the tunnel floor instead of a roof to fall through.
  if (caves_.enabled()) {
    CaveQuery query;
    gather_caves(unit_dir, nullptr, &query);
    bool near_cave = false;
    for (int i = 0; i < query.count; ++i) {
      const CaveField::System& system = *query.systems[i];
      const Real along = dot(system.bound_center, unit_dir);
      const Dir3 off{system.bound_center.x - unit_dir.x * along,
                     system.bound_center.y - unit_dir.y * along,
                     system.bound_center.z - unit_dir.z * along};
      if (dot(off, off) < system.bound_m * system.bound_m) {
        near_cave = true;
        break;
      }
    }
    if (near_cave) {
      const auto cave_density_at = [&](Real r) {
        const Dir3 position{unit_dir.x * r, unit_dir.y * r, unit_dir.z * r};
        Real density = density_at(r);
        for (int i = 0; i < query.count; ++i) {
          density = smin(density, CaveField::system_sdf(*query.systems[i], position),
                         Real(CaveField::kSminM));
        }
        return density;
      };
      Real hi = surface + Real(15.0);
      Real lo = hi;
      bool found = false;
      for (int step = 1; step <= 170; ++step) {
        lo = surface + Real(15.0) - Real(3.0) * Real(static_cast<double>(step));
        if (cave_density_at(lo) > Real(0.0)) {
          found = true;
          break;
        }
        hi = lo;
      }
      if (!found) {
        return surface;  // bound bookkeeping guarantees solid by here
      }
      for (int i = 0; i < 20; ++i) {
        const Real mid = (lo + hi) * Real(0.5);
        if (cave_density_at(mid) > Real(0.0)) {
          lo = mid;
        } else {
          hi = mid;
        }
      }
      return (lo + hi) * Real(0.5);
    }
  }

  Real lo = surface - Real(6.0);   // below: expect solid (density > 0)
  Real hi = surface + Real(6.0);   // above: expect air (density < 0)
  if (density_at(lo) <= Real(0.0) || density_at(hi) >= Real(0.0)) {
    // Bracket failed (extreme detail constellation): widen once, then
    // fall back to the elevation surface.
    lo = surface - Real(12.0);
    hi = surface + Real(12.0);
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

Real TerrainField::ground_radius_below_m(const Dir3& unit_dir, Real from_r) const {
  const Real top = ground_radius_m(unit_dir);
  if (!caves_.enabled() || from_r >= top) {
    return top;  // above ground: the floor IS the topmost surface
  }
  // Below the topmost surface (inside a cave): first crossing under the
  // caller. Fixed-step scan + bisection, cave-aware density.
  CaveQuery query;
  gather_caves(unit_dir, nullptr, &query);
  const Real surface = planet_.radius_m + elevation_m(unit_dir);
  const auto cave_density_at = [&](Real r) {
    const Dir3 position{unit_dir.x * r, unit_dir.y * r, unit_dir.z * r};
    Real density = (surface - r) + detail_m(position);
    for (int i = 0; i < query.count; ++i) {
      density = smin(density, CaveField::system_sdf(*query.systems[i], position),
                     Real(CaveField::kSminM));
    }
    return density;
  };
  Real hi = from_r;
  Real lo = hi;
  bool found = false;
  for (int step = 1; step <= 200; ++step) {
    lo = from_r - Real(2.5) * Real(static_cast<double>(step));
    if (cave_density_at(lo) > Real(0.0)) {
      found = true;
      break;
    }
    hi = lo;
  }
  if (!found) {
    return top;
  }
  for (int i = 0; i < 20; ++i) {
    const Real mid = (lo + hi) * Real(0.5);
    if (cave_density_at(mid) > Real(0.0)) {
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
      BlendedParams params = TerrainField::to_blended(canonical);
      const Real surface_r =
          field.planet().radius_m +
          field.elevation_from_params(dir, params, canonical.macro_rel, &param_cache);
      TerrainField::CaveQuery cave_query;
      field.gather_caves(dir, &param_cache, &cave_query);

      for (int gz = lo; gz <= hi; ++gz) {
        const Real fz(static_cast<double>(gz) / ChunkGrid::kVoxels);
        const Real r = det::lerp(grid.r0, grid.r1, fz);
        const Dir3 position{dir.x * r, dir.y * r, dir.z * r};
        Real density = r <= field.planet().core_radius_m
                           ? Real(1.0e9)
                           : field.apply_caves(position, surface_r,
                                               (surface_r - r) + field.detail_m(position),
                                               cave_query);
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
