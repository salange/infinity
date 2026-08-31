#include "gen/features.hpp"

#include "gen/names.hpp"

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

// Feature cells subdivide the province grid: fine enough that a cell is a
// local neighbourhood, coarse enough that the largest crater (bounded
// below) still fits the stencil.
constexpr std::uint32_t kCellsPerProvinceCell = 6;

// Stencil bookkeeping in units of one feature cell (uv size 2/n, chord
// size 0.94/n..2/n across the cube-sphere — same convention as the
// province kernel, whose coverage argument this copies):
//  - kMaxReachCells: no feature influence may extend beyond this chord
//    distance from the feature's origin.
//  - kCellMarginCells: the origin's whole cell must lie inside probe
//    coverage so a probe is guaranteed to land in it (max cell diagonal).
//  - probes at kProbeStepCells spacing: diagonal spacing 0.6*sqrt(2) =
//    0.85 stays below the smallest cell chord (0.94), so no covered cell
//    slips between probes.
constexpr double kMaxReachCells = 1.0;
constexpr double kCellMarginCells = 1.6;
constexpr double kProbeStepCells = 0.6;
constexpr int kStencilHalf = 5;
// Ejecta extend to 2.5x the bowl radius; that outermost reach is the
// feature bound the stencil must contain.
constexpr double kEjectaReach = 2.5;
constexpr double kMaxBowlCells = 0.4;
static_assert(kEjectaReach * kMaxBowlCells <= kMaxReachCells,
              "crater ejecta may leave the probe stencil");
static_assert(kStencilHalf * kProbeStepCells >= kMaxReachCells + kCellMarginCells,
              "probe coverage cannot contain every reachable feature cell");

// Mean crater count per cell = type base x body age x archetype factor x
// this scale, capped at kMaxPerCell. Airless worlds keep their record;
// thick atmospheres erase it (EarthLike hosts none at all and pays no
// query cost — the acceptance criterion for bounded entities).
constexpr double kMeanScale = 1.6;

double type_base(PlanetType type) {
  switch (type) {
    case PlanetType::Barren: return 1.0;
    case PlanetType::Ice: return 0.55;
    case PlanetType::Desert: return 0.35;
    case PlanetType::EarthLike:
    default: return 0.0;
  }
}

double archetype_factor(Archetype archetype) {
  switch (archetype) {
    case Archetype::Cratered: return 3.2;
    case Archetype::RegolithPlains: return 1.2;
    case Archetype::Highlands: return 1.8;
    case Archetype::Dunes: return 0.25;        // dunes bury craters
    case Archetype::Mesas: return 0.55;
    case Archetype::Canyonlands: return 0.55;
    case Archetype::GlacialShield: return 0.4; // ice flow relaxes them
    case Archetype::CrevasseField: return 0.7;
    case Archetype::RidgeField: return 0.8;
    default: return 0.0;
  }
}

}  // namespace

FeatureField::FeatureField(const core::Key& body_key, const PlanetParams& planet)
    : features_key_(core::derive_named(body_key, name::FeaturesV1)),
      provinces_(body_key, planet),
      type_(planet.type),
      cells_per_face_(planet.cells_per_face * kCellsPerProvinceCell),
      radius_m_(planet.radius_m) {
  const auto draw = core::draw_point(features_key_, channel::Params, 0, 0, 0);
  age_ = Real(0.35) + Real(0.65) * u01(draw[0]);
  enabled_ = type_base(type_) > 0.0 && cells_per_face_ > 0;
}

CellId FeatureField::cell_of(const Dir3& unit_dir) const {
  const FaceUV face_uv = dir_to_face_uv(unit_dir);
  const auto n = static_cast<double>(cells_per_face_);
  const double fu = (face_uv.u.to_double() + 1.0) * 0.5 * n;
  const double fv = (face_uv.v.to_double() + 1.0) * 0.5 * n;
  auto clamp_cell = [&](double value) {
    if (value < 0.0) return std::uint32_t{0};
    if (value >= n) return cells_per_face_ - 1;
    return static_cast<std::uint32_t>(value);
  };
  return CellId{face_uv.face, clamp_cell(fu), clamp_cell(fv)};
}

FeatureField::CellCraters FeatureField::cell_craters(const CellId& cell) const {
  CellCraters out;
  const core::Key cell_key = core::derive_child(features_key_, kind::Feature, cell.face,
                                                cell.ci, cell.cj);
  // Density: the hosting province's archetype decides how cratered the
  // neighbourhood is (Cratered finally means craters, Flats means few).
  const Real n(static_cast<double>(cells_per_face_));
  const Real cu = (Real(static_cast<double>(cell.ci)) + Real(0.5)) / n * Real(2.0) - Real(1.0);
  const Real cv = (Real(static_cast<double>(cell.cj)) + Real(0.5)) / n * Real(2.0) - Real(1.0);
  const Dir3 center_dir = face_uv_to_dir(FaceUV{cell.face, cu, cv});
  const Archetype archetype =
      provinces_.cell_params(provinces_.cell_of(center_dir)).archetype;

  const Real expected = Real(type_base(type_) * archetype_factor(archetype) * kMeanScale) *
                        age_;
  const auto count_draw = core::draw_point(cell_key, channel::Params, 0, 0, 0);
  const double whole = expected.to_double();
  int count = static_cast<int>(whole);
  if (u01(count_draw[0]) < expected - Real(static_cast<double>(count))) {
    ++count;
  }
  if (count > kMaxPerCell) {
    count = kMaxPerCell;
  }

  const Real bowl_max = Real(kMaxBowlCells) / n;
  const Real min_by_metres = Real(90.0) / radius_m_;
  const Real bowl_min = det::max(min_by_metres, bowl_max / Real(45.0));
  const Real ratio = bowl_min / bowl_max;
  for (int i = 0; i < count; ++i) {
    const auto draw_a = core::draw_point(cell_key, channel::Params, i, 1, 0);
    const auto draw_b = core::draw_point(cell_key, channel::Params, i, 2, 0);
    Crater& crater = out.craters[out.count++];
    // Origin jittered inside the cell — the stencil invariant relies on
    // the origin staying within its own cell.
    const Real ju = (u01(draw_a[0]) - Real(0.5)) * Real(0.96);
    const Real jv = (u01(draw_a[1]) - Real(0.5)) * Real(0.96);
    const Real u = (Real(static_cast<double>(cell.ci)) + Real(0.5) + ju) / n * Real(2.0) -
                   Real(1.0);
    const Real v = (Real(static_cast<double>(cell.cj)) + Real(0.5) + jv) / n * Real(2.0) -
                   Real(1.0);
    crater.center = face_uv_to_dir(FaceUV{cell.face, u, v});
    // Power-law sizes, N(>D) ~ D^-2: invert P(R > r) between the bounds —
    // many small craters, few huge ones.
    const Real su = u01(draw_a[2]);
    crater.bowl_chord =
        bowl_min / det::sqrt(Real(1.0) - su * (Real(1.0) - ratio * ratio));
    const Real bowl_m = crater.bowl_chord * radius_m_;
    // Depth/diameter ~1:5 for simple craters, flattening for large ones
    // (central-peak/complex regime stand-in); freshness degrades depth.
    const Real freshness = Real(0.55) + Real(0.45) * u01(draw_b[0]);
    crater.depth_m = Real(0.4) * bowl_m / (Real(1.0) + bowl_m / Real(4000.0)) * freshness;
    crater.rim_frac = Real(0.12) + Real(0.10) * u01(draw_b[1]);
  }
  return out;
}

Real FeatureField::height_offset_m(const Dir3& unit_dir, Cache* cache) const {
  if (!enabled_) {
    return Real(0.0);
  }
  const auto n = static_cast<double>(cells_per_face_);
  const Real step(kProbeStepCells * 2.0 / n);

  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(unit_dir, &t1, &t2);

  // Candidate cells from the probe stencil, deduplicated into a fixed
  // array (this runs per elevation query — no allocation). Probes stay
  // unnormalized: dir_to_face_uv is scale-invariant (pure axis ratios),
  // so cell_of gives the same answer without the sqrt. Probes outside the
  // covered disc (rounded index per axis + half-diagonal slack) are
  // skipped — the square's corners exceed the coverage requirement.
  constexpr int kMaxCandidates = 64;
  constexpr int kDiscSq = 26;
  CellId candidates[kMaxCandidates];
  int candidate_count = 0;
  for (int di = -kStencilHalf; di <= kStencilHalf; ++di) {
    for (int dj = -kStencilHalf; dj <= kStencilHalf; ++dj) {
      if (di * di + dj * dj > kDiscSq) {
        continue;
      }
      const Real offset_u = step * Real(static_cast<double>(di));
      const Real offset_v = step * Real(static_cast<double>(dj));
      const Dir3 probe = Dir3{unit_dir.x + t1.x * offset_u + t2.x * offset_v,
                              unit_dir.y + t1.y * offset_u + t2.y * offset_v,
                              unit_dir.z + t1.z * offset_u + t2.z * offset_v};
      const CellId cell = cell_of(probe);
      bool seen = false;
      for (int c = 0; c < candidate_count; ++c) {
        if (candidates[c] == cell) {
          seen = true;
          break;
        }
      }
      if (!seen && candidate_count < kMaxCandidates) {
        candidates[candidate_count++] = cell;
      }
    }
  }

  Real offset(0.0);
  CellCraters scratch;
  for (int c = 0; c < candidate_count; ++c) {
    const CellCraters* craters_ptr;
    if (cache != nullptr) {
      const CellId& cell = candidates[c];
      const std::uint64_t packed = (static_cast<std::uint64_t>(cell.face) << 40U) |
                                   (static_cast<std::uint64_t>(cell.ci) << 20U) | cell.cj;
      const auto it = cache->find(packed);
      if (it != cache->end()) {
        craters_ptr = &it->second;
      } else {
        craters_ptr = &cache->emplace(packed, cell_craters(cell)).first->second;
      }
    } else {
      scratch = cell_craters(candidates[c]);
      craters_ptr = &scratch;
    }
    const CellCraters& craters = *craters_ptr;
    for (int i = 0; i < craters.count; ++i) {
      const Crater& crater = craters.craters[i];
      const Real reach = crater.bowl_chord * Real(kEjectaReach);
      const Real dist_sq = chord_sq(unit_dir, crater.center);
      if (dist_sq >= reach * reach) {
        continue;
      }
      const Real t = det::sqrt(dist_sq) / crater.bowl_chord;
      // Profile: parabolic bowl to 0.8, rim rising to its crest at 1.0,
      // ejecta blanket decaying to zero at 2.5 — C0 at every joint.
      if (t < Real(0.8)) {
        offset = offset + crater.depth_m * (t * t / Real(0.64) - Real(1.0));
      } else if (t < Real(1.0)) {
        const Real s = (t - Real(0.8)) * Real(5.0);
        const Real smooth = s * s * (Real(3.0) - (s + s));
        offset = offset + crater.depth_m * crater.rim_frac * smooth;
      } else {
        const Real g = (Real(kEjectaReach) - t) / Real(kEjectaReach - 1.0);
        offset = offset + crater.depth_m * crater.rim_frac * g * g * g;
      }
    }
  }
  return offset;
}

}  // namespace inf::gen
