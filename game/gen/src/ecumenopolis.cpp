#include "gen/ecumenopolis.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "gen/material.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using civ::u01;
using det::Real;

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
double smoothstep(double t) {
  t = clamp01(t);
  return t * t * (3.0 - 2.0 * t);
}
double hash01(std::uint64_t h) { return static_cast<double>(h >> 11U) * 0x1.0p-53; }

// The cheap per-block hash on the sanctioned mixer path (derived key ^
// mix(coords)); used for the sub-draws of a block after its one Philox
// draw seeded the block.
std::uint64_t block_hash(std::uint64_t seed, std::int64_t a, std::int64_t b, std::uint64_t salt) {
  const std::uint64_t packed = (static_cast<std::uint64_t>(a) * 0x9E3779B97F4A7C15ULL) ^
                               (static_cast<std::uint64_t>(b) * 0xC2B2AE3D27D4EB4FULL) ^ salt;
  return det::mix64(seed ^ det::mix64(packed));
}

// Fractional face-cell coordinates of a direction at n cells per face
// edge: (face, fu, fv) with fu, fv in [0, n).
void face_cell_coords(const Dir3& dir, std::uint32_t n, std::uint8_t* face, double* fu, double* fv) {
  const FaceUV f = dir_to_face_uv(dir);
  *face = f.face;
  const double scale = 0.5 * static_cast<double>(n);
  *fu = (f.u.to_double() + 1.0) * scale;
  *fv = (f.v.to_double() + 1.0) * scale;
  const double hi = static_cast<double>(n) - 1e-9;
  if (*fu < 0.0) *fu = 0.0;
  if (*fv < 0.0) *fv = 0.0;
  if (*fu > hi) *fu = hi;
  if (*fv > hi) *fv = hi;
}

Dir3 cell_point(std::uint8_t face, std::uint32_t n, double fu, double fv) {
  const double inv = 2.0 / static_cast<double>(n);
  return face_uv_to_dir(FaceUV{face, Real(fu * inv - 1.0), Real(fv * inv - 1.0)});
}

}  // namespace

EcumenopolisField::EcumenopolisField(const core::Key& body_entity_key, const TerrainField& field,
                                     const SettlementPlan& plan, const RaceParams& race,
                                     const std::vector<FactionParams>& factions,
                                     const CivState& state)
    : key_(core::derive_named(body_entity_key, name::EcumenopolisV1)),
      field_(field),
      state_(state),
      n_(plan.cells_per_face),
      radius_m_(field.planet().radius_m.to_double()),
      sea_m_(field.planet().sea_level_m.to_double()),
      has_sea_(field.planet().land_fraction.to_double() < 0.999),
      capital_(plan.capital) {
  (void)factions;
  // --- the block level: face cells of ~120 m ---------------------------------
  const double face_arc_m = 1.5707963267948966 * radius_m_;
  block_level_ = 8;
  while (block_level_ < 20 && face_arc_m / static_cast<double>(1U << block_level_) > kBlockTargetM * 1.4142) {
    ++block_level_;
  }
  blocks_per_face_ = 1U << block_level_;
  block_m_ = face_arc_m / static_cast<double>(blocks_per_face_);

  // --- style base (design 15), one for the whole city -----------------------
  {
    StyleVector& s = style_;
    s.race_type = race.type;
    s.race_variant = race.variant;
    for (int i = 0; i < 2; ++i) for (int c = 0; c < 3; ++c) s.palette[i][c] = race.palette[i][c];
    s.material_family = race.material_family;
    s.faction_type = state.faction_type;
    s.tech_tier = race.tech_tier;
    s.tier = SettlementTier::Ecumenopolis;
    s.level = 7;
    s.ruined = state.ruined;
    s.domed = false;
    s.regularity = 0.85f;
    s.wear = state.ruined ? 1.0f : 0.15f;
    s.ornament = 0.5f;
    s.light_density = state.ruined ? 0.0f : 0.9f;
    s.construction = 1.0f;
  }

  // --- plates: per province, max of nine base-terrain probes + 40 m ---------
  plate_.assign(plan.provinces.size(), 0.0f);
  plate_min_ = 1e300;
  plate_max_ = -1e300;
  const ProvinceField& provinces = field.provinces();
  for (const ProvinceSite& province : plan.provinces) {
    const Dir3 centre = provinces.representative(province.cell);
    std::uint8_t face = 0;
    double fu = 0.0;
    double fv = 0.0;
    face_cell_coords(centre, n_, &face, &fu, &fv);
    double top = -1e300;
    bool any_land = false;
    for (int k = 0; k < 9; ++k) {
      const double du = (k % 3 - 1) * 0.33;
      const double dv = (k / 3 - 1) * 0.33;
      const Dir3 probe = k == 4 ? centre : cell_point(face, n_, fu + du, fv + dv);
      const double h = field.base_elevation_m(probe).to_double();
      top = std::max(top, h);
      any_land = any_land || !has_sea_ || h > sea_m_;
    }
    double datum = top + kPlateAboveTerrainM;
    if (has_sea_) {
      if (province.ocean || !any_land) datum = sea_m_ + kPlateAboveSeaM;
      datum = std::max(datum, sea_m_ + kPlateAboveSeaM);
    }
    plate_[province.index] = static_cast<float>(datum);
    plate_min_ = std::min(plate_min_, datum);
    plate_max_ = std::max(plate_max_, datum);
  }

  // --- the district field: three octaves on the unit sphere ----------------
  const double wavelengths_m[3] = {300000.0, 60000.0, 12000.0};
  for (int o = 0; o < 3; ++o) {
    octave_keys_[o] = core::derive_child(key_, kind::MacroCell, o);
    // At least four lattice cells around the planet for the largest
    // wavelength on small bodies.
    const double wavelength = std::min(wavelengths_m[o], 1.5707963267948966 * radius_m_);
    octave_scale_[o] = radius_m_ / wavelength;
  }
}

double EcumenopolisField::province_datum(const Dir3& probe) const {
  const CellId cell = field_.provinces().cell_of(probe);
  const std::size_t index = (static_cast<std::size_t>(cell.face) * n_ + cell.ci) * n_ + cell.cj;
  return index < plate_.size() ? static_cast<double>(plate_[index]) : 0.0;
}

double EcumenopolisField::plate_m(const Dir3& unit_dir) const {
  std::uint8_t face = 0;
  double fu = 0.0;
  double fv = 0.0;
  face_cell_coords(unit_dir, n_, &face, &fu, &fv);
  const double ci = std::floor(fu);
  const double cj = std::floor(fv);
  const double fx = fu - ci - 0.5;  // [-0.5, 0.5)
  const double fy = fv - cj - 0.5;
  // Ramp weights toward the x and y neighbours across the nearer border:
  // 0 inside the plateau, 0.5 exactly on the border (symmetric, so the
  // neighbour's own blend meets it continuously).
  const double sx = smoothstep((std::fabs(fx) - (0.5 - kRampBand)) / (2.0 * kRampBand));
  const double sy = smoothstep((std::fabs(fy) - (0.5 - kRampBand)) / (2.0 * kRampBand));
  const double d00 = province_datum(unit_dir);
  if (sx <= 0.0 && sy <= 0.0) {
    return d00;
  }
  const double dxs = fx >= 0.0 ? 1.0 : -1.0;
  const double dys = fy >= 0.0 ? 1.0 : -1.0;
  // Neighbour plates through probe directions (face_uv_to_dir accepts
  // coordinates past the face edge, cell_of resolves the true cell).
  const double d10 = sx > 0.0 ? province_datum(cell_point(face, n_, ci + 0.5 + dxs, cj + 0.5)) : d00;
  const double d01 = sy > 0.0 ? province_datum(cell_point(face, n_, ci + 0.5, cj + 0.5 + dys)) : d00;
  const double d11 = (sx > 0.0 && sy > 0.0)
                         ? province_datum(cell_point(face, n_, ci + 0.5 + dxs, cj + 0.5 + dys))
                         : (sx > 0.0 ? d10 : d01);
  return (1.0 - sx) * (1.0 - sy) * d00 + sx * (1.0 - sy) * d10 + (1.0 - sx) * sy * d01 + sx * sy * d11;
}

double EcumenopolisField::hole_factor(const Dir3& unit_dir) const {
  if (!state_.ruined) {
    return 1.0;
  }
  // Collapsed arterial cells (8 x 8 blocks): keyed per cell; the plate
  // falls to the terrain inside 60 % of the cell with a rim over 30 %.
  const std::uint32_t n = blocks_per_face_ >> kArterialShift;
  std::uint8_t face = 0;
  double fu = 0.0;
  double fv = 0.0;
  face_cell_coords(unit_dir, n, &face, &fu, &fv);
  const double ai = std::floor(fu);
  const double aj = std::floor(fv);
  const auto d = core::draw_point(key_, channel::Layout, static_cast<std::int64_t>(ai),
                                  static_cast<std::int64_t>(aj), static_cast<std::int64_t>(face) + 8);
  if (u01(d[0]) >= kHoleFraction) {
    return 1.0;
  }
  const double m = std::max(std::fabs(fu - ai - 0.5), std::fabs(fv - aj - 0.5));
  return smoothstep((m - 0.3) / 0.15);
}

det::Real EcumenopolisField::modify(const Dir3& unit_dir, det::Real base_m, const BaseEval& base_at) const {
  (void)base_at;
  const double base = base_m.to_double();
  double plate = plate_m(unit_dir);
  if (state_.ruined) {
    const double w = hole_factor(unit_dir);
    if (w < 1.0) {
      plate = base + (plate - base) * w;
    }
  }
  // Preserved peaks: terrain above the plate pierces the city.
  return Real(plate > base ? plate : base);
}

HeightModifier::Urban EcumenopolisField::urban(const Dir3& unit_dir, det::Real surface_m) const {
  Urban out;
  out.family = style_.material_family;
  const double plate = plate_m(unit_dir);
  if (surface_m.to_double() > plate + 0.5) {
    return out;  // a preserved peak (terrain above the plate): bare, dark
  }
  const double hole = hole_factor(unit_dir);
  out.weight = (state_.ruined ? 0.55 : 1.0) * hole;
  if (state_.ruined) {
    out.night_light = 0.02 * hole;
    return out;
  }
  // The light lattice: brightest along arterials and mega-avenues,
  // district intensity elsewhere.
  std::uint8_t face = 0;
  double fu = 0.0;
  double fv = 0.0;
  face_cell_coords(unit_dir, blocks_per_face_ >> kArterialShift, &face, &fu, &fv);
  const double ax = std::fabs(fu - std::floor(fu) - 0.5);
  const double ay = std::fabs(fv - std::floor(fv) - 0.5);
  const double arterial = 1.0 - smoothstep((0.5 - std::max(ax, ay)) / 0.12);
  const double mx = std::fabs(fu / 8.0 - std::floor(fu / 8.0) - 0.5);
  const double my = std::fabs(fv / 8.0 - std::floor(fv / 8.0) - 0.5);
  const double avenue = 1.0 - smoothstep((0.5 - std::max(mx, my)) / 0.04);
  const District d = district(unit_dir);
  out.night_light = clamp01(d.light * (0.35 + 0.65 * std::max(arterial, avenue)));
  return out;
}

double EcumenopolisField::noise(int octave, double x, double y, double z, int word) const {
  const double fx = std::floor(x);
  const double fy = std::floor(y);
  const double fz = std::floor(z);
  const double tx = smoothstep(x - fx);
  const double ty = smoothstep(y - fy);
  const double tz = smoothstep(z - fz);
  double corner[8];
  for (int c = 0; c < 8; ++c) {
    const auto d = core::draw_point(octave_keys_[octave], channel::Layout,
                                    static_cast<std::int64_t>(fx) + (c & 1),
                                    static_cast<std::int64_t>(fy) + ((c >> 1) & 1),
                                    static_cast<std::int64_t>(fz) + ((c >> 2) & 1));
    corner[c] = u01(d[static_cast<std::size_t>(word)]);
  }
  const double x0 = corner[0] + (corner[1] - corner[0]) * tx;
  const double x1 = corner[2] + (corner[3] - corner[2]) * tx;
  const double x2 = corner[4] + (corner[5] - corner[4]) * tx;
  const double x3 = corner[6] + (corner[7] - corner[6]) * tx;
  const double y0 = x0 + (x1 - x0) * ty;
  const double y1 = x2 + (x3 - x2) * ty;
  return y0 + (y1 - y0) * tz;
}

EcumenopolisField::District EcumenopolisField::district(const Dir3& unit_dir) const {
  const double px = unit_dir.x.to_double();
  const double py = unit_dir.y.to_double();
  const double pz = unit_dir.z.to_double();
  double type_n = 0.0;
  double height_n = 0.0;
  const double weights[3] = {0.5, 0.3, 0.2};
  double fine = 0.0;
  for (int o = 0; o < 3; ++o) {
    const double s = octave_scale_[o];
    const double t = noise(o, px * s + 17.0, py * s + 29.0, pz * s + 41.0, 0);
    const double h = noise(o, px * s + 53.0, py * s + 67.0, pz * s + 79.0, 1);
    type_n += weights[o] * t;
    height_n += weights[o] * h;
    if (o == 2) fine = t;
  }
  District d;
  if (fine > 0.86) {
    d.type = DistrictType::Park;
  } else if (type_n > 0.64) {
    d.type = DistrictType::Civic;
  } else if (type_n < 0.36) {
    d.type = DistrictType::Industrial;
  } else {
    d.type = DistrictType::Residential;
  }
  // 200-1500 m, skewed toward the low end; civic districts reach higher.
  const double hn = clamp01((height_n - 0.2) / 0.6);
  d.height_budget_m = 200.0 + 1300.0 * hn * hn;
  switch (d.type) {
    case DistrictType::Civic: d.height_budget_m *= 1.15; d.density = 0.75; d.light = 1.0; break;
    case DistrictType::Residential: d.height_budget_m *= 0.75; d.density = 0.9; d.light = 0.7; break;
    case DistrictType::Industrial: d.height_budget_m *= 0.4; d.density = 0.95; d.light = 0.45; break;
    case DistrictType::Park: d.height_budget_m = 60.0; d.density = 0.5; d.light = 0.25; break;
  }
  if (d.height_budget_m > 1500.0) d.height_budget_m = 1500.0;
  return d;
}

Dir3 EcumenopolisField::arterial_cell_centre(const BlockId& block) const {
  const std::uint32_t n = blocks_per_face_ >> kArterialShift;
  return cell_point(block.face, n, (block.bi >> kArterialShift) + 0.5, (block.bj >> kArterialShift) + 0.5);
}

EcumenopolisField::BlockId EcumenopolisField::block_of(const Dir3& unit_dir) const {
  std::uint8_t face = 0;
  double fu = 0.0;
  double fv = 0.0;
  face_cell_coords(unit_dir, blocks_per_face_, &face, &fu, &fv);
  return BlockId{face, static_cast<std::uint32_t>(fu), static_cast<std::uint32_t>(fv)};
}

Dir3 EcumenopolisField::block_centre(const BlockId& block) const {
  return cell_point(block.face, blocks_per_face_, block.bi + 0.5, block.bj + 0.5);
}

Dir3 EcumenopolisField::block_corner(const BlockId& block, int k) const {
  const double du = (k == 1 || k == 2) ? 1.0 : 0.0;
  const double dv = (k == 2 || k == 3) ? 1.0 : 0.0;
  return cell_point(block.face, blocks_per_face_, block.bi + du, block.bj + dv);
}

core::Key EcumenopolisField::block_key(const BlockId& block) const {
  return core::derive_child(key_, kind::Site, block.face, block.bi, block.bj);
}

core::Key EcumenopolisField::tower_key(const BlockId& block, int tower) const {
  return core::derive_child(core::derive_named(block_key(block), name::BuildingsV1), kind::Lot, tower);
}

EcumenopolisField::CellInfo EcumenopolisField::cell_info(const BlockId& block) const {
  // The arterial cell (8 x 8 blocks) decides what every block in it
  // shares: the preserved-peak test, the collapse (ruins) and the
  // district — one terrain read and one district sample per cell, so a
  // tile costs what its cells cost, not what its blocks would.
  CellInfo info;
  const Dir3 cell_centre = arterial_cell_centre(block);
  if (field_.base_elevation_m(cell_centre).to_double() > plate_m(cell_centre) + 0.5) {
    return info;
  }
  if (state_.ruined && hole_factor(cell_centre) < 0.5) {
    return info;
  }
  info.built = true;
  info.district = district(cell_centre);
  return info;
}

int EcumenopolisField::towers_in_block(const BlockId& block, const SiteFrame& frame,
                                       std::vector<Lot>* out, const CellInfo* info) const {
  CellInfo local;
  if (info == nullptr) {
    local = cell_info(block);
    info = &local;
  }
  if (!info->built) {
    return 0;
  }
  const Dir3 centre = block_centre(block);
  const double plate = plate_m(centre);
  // The block's footprint in the frame: corners, then the half vectors
  // along u and v (the block is a slightly skewed square; the towers use
  // its own axes).
  double cx[4];
  double cy[4];
  for (int k = 0; k < 4; ++k) {
    frame.to_local(block_corner(block, k), &cx[k], &cy[k]);
  }
  const double ox = 0.25 * (cx[0] + cx[1] + cx[2] + cx[3]);
  const double oy = 0.25 * (cy[0] + cy[1] + cy[2] + cy[3]);
  const double ax = 0.25 * (cx[1] + cx[2] - cx[0] - cx[3]);  // half vector along +u
  const double ay = 0.25 * (cy[1] + cy[2] - cy[0] - cy[3]);
  const double bx = 0.25 * (cx[2] + cx[3] - cx[0] - cx[1]);  // half vector along +v
  const double by = 0.25 * (cy[2] + cy[3] - cy[0] - cy[1]);
  const double half_u = std::sqrt(ax * ax + ay * ay);
  const double half_v = std::sqrt(bx * bx + by * by);
  if (half_u < 20.0 || half_v < 20.0) {
    return 0;
  }
  // Street insets per border: block street, arterial, mega-avenue.
  const auto inset = [&](std::uint32_t index, bool high_side) {
    const std::uint32_t border = high_side ? index + 1 : index;
    if ((border & ((1U << kAvenueShift) - 1)) == 0) return kAvenueHalfM;
    if ((border & ((1U << kArterialShift) - 1)) == 0) return kArterialHalfM;
    return kStreetHalfM;
  };
  const double u_lo = -1.0 + inset(block.bi, false) / half_u;
  const double u_hi = 1.0 - inset(block.bi, true) / half_u;
  const double v_lo = -1.0 + inset(block.bj, false) / half_v;
  const double v_hi = 1.0 - inset(block.bj, true) / half_v;
  if (u_hi - u_lo < 0.3 || v_hi - v_lo < 0.3) {
    return 0;
  }
  // The block's draws: one Philox draw seeds the cheap sub-hashes.
  const auto seed = core::draw_point(block_key(block), channel::Layout, 0, 0, 0);
  const std::uint64_t s = seed[0] ^ (seed[1] << 1U);
  const District& d = info->district;
  const double landmark = hash01(block_hash(s, 0, 0, 0x11)) < 0.04 ? 1.5 : 1.0;
  int count = 1;
  double slices_u[4][2] = {};
  double slices_v[4][2] = {};
  double heights[4] = {};
  LotUsage usage = LotUsage::Residential;
  switch (d.type) {
    case DistrictType::Civic: {
      count = 1;
      const double f = 0.5 + 0.2 * hash01(block_hash(s, 0, 0, 0x12));
      const double mu = 0.5 * (u_lo + u_hi);
      const double mv = 0.5 * (v_lo + v_hi);
      slices_u[0][0] = mu - 0.5 * f * (u_hi - u_lo); slices_u[0][1] = mu + 0.5 * f * (u_hi - u_lo);
      slices_v[0][0] = mv - 0.5 * f * (v_hi - v_lo); slices_v[0][1] = mv + 0.5 * f * (v_hi - v_lo);
      heights[0] = d.height_budget_m * (0.7 + 0.3 * hash01(block_hash(s, 0, 0, 0x13))) * landmark;
      usage = LotUsage::Civic;
      break;
    }
    case DistrictType::Residential: {
      count = 2 + static_cast<int>(hash01(block_hash(s, 0, 0, 0x14)) * 2.0);  // 2-3 slabs along u
      const double gap = 8.0 / half_u;
      const double span = (u_hi - u_lo - gap * (count - 1)) / count;
      for (int i = 0; i < count; ++i) {
        slices_u[i][0] = u_lo + i * (span + gap);
        slices_u[i][1] = slices_u[i][0] + span;
        slices_v[i][0] = v_lo;
        slices_v[i][1] = v_hi;
        heights[i] = d.height_budget_m * (0.45 + 0.55 * hash01(block_hash(s, i, 1, 0x15))) * (i == 0 ? landmark : 1.0);
      }
      usage = LotUsage::Residential;
      break;
    }
    case DistrictType::Industrial: {
      count = 1;
      slices_u[0][0] = u_lo; slices_u[0][1] = u_hi;
      slices_v[0][0] = v_lo; slices_v[0][1] = v_hi;
      heights[0] = d.height_budget_m * (0.6 + 0.4 * hash01(block_hash(s, 0, 0, 0x16)));
      usage = LotUsage::Industrial;
      break;
    }
    case DistrictType::Park: {
      count = 1;
      const double mu = 0.5 * (u_lo + u_hi);
      const double mv = 0.5 * (v_lo + v_hi);
      slices_u[0][0] = mu - 0.35 * (u_hi - u_lo); slices_u[0][1] = mu + 0.35 * (u_hi - u_lo);
      slices_v[0][0] = mv - 0.35 * (v_hi - v_lo); slices_v[0][1] = mv + 0.35 * (v_hi - v_lo);
      heights[0] = d.height_budget_m * (0.6 + 0.4 * hash01(block_hash(s, 0, 0, 0x17)));
      usage = LotUsage::Agricultural;
      break;
    }
  }
  // Density: some blocks stay podium-only (a low slab).
  const bool low = hash01(block_hash(s, 0, 0, 0x18)) > d.density;
  for (int i = 0; i < count; ++i) {
    Lot lot;
    lot.id = static_cast<std::uint32_t>(i);
    lot.order = static_cast<std::uint32_t>(i);
    lot.tier = static_cast<std::uint8_t>(SettlementTier::Ecumenopolis);
    lot.vertex_count = 4;
    const double uu[4] = {slices_u[i][0], slices_u[i][1], slices_u[i][1], slices_u[i][0]};
    const double vv[4] = {slices_v[i][0], slices_v[i][0], slices_v[i][1], slices_v[i][1]};
    for (int k = 0; k < 4; ++k) {
      lot.footprint[k][0] = static_cast<float>(ox + ax * uu[k] + bx * vv[k]);
      lot.footprint[k][1] = static_cast<float>(oy + ay * uu[k] + by * vv[k]);
    }
    lot.datum_m = static_cast<float>(plate);
    lot.height_budget_m = static_cast<float>(low ? std::min(heights[i], 40.0) : heights[i]);
    lot.usage = usage;
    lot.style = style_;
    out->push_back(lot);
  }
  return count;
}

EcumenopolisField::TileId EcumenopolisField::tile_of(const Dir3& unit_dir, int shift) const {
  std::uint8_t face = 0;
  double fu = 0.0;
  double fv = 0.0;
  face_cell_coords(unit_dir, blocks_per_face_ >> shift, &face, &fu, &fv);
  return TileId{shift, face, static_cast<std::uint32_t>(fu), static_cast<std::uint32_t>(fv)};
}

Dir3 EcumenopolisField::tile_centre(const TileId& tile) const {
  return cell_point(tile.face, blocks_per_face_ >> tile.shift, tile.ti + 0.5, tile.tj + 0.5);
}

SiteFrame EcumenopolisField::tile_frame(const TileId& tile) const {
  SiteFrame frame;
  frame.up = tile_centre(tile);
  tangent_basis(frame.up, &frame.east, &frame.north);
  frame.radius_m = radius_m_;
  return frame;
}

// --- tile meshes -----------------------------------------------------------------

namespace {

struct TileWriter {
  std::vector<float>* out;
  void vertex(double x, double y, double z, double nx, double ny, double nz, int slot) {
    out->push_back(static_cast<float>(x));
    out->push_back(static_cast<float>(y));
    out->push_back(static_cast<float>(z));
    out->push_back(static_cast<float>(nx));
    out->push_back(static_cast<float>(ny));
    out->push_back(static_cast<float>(nz));
    for (int k = 0; k < 4; ++k) out->push_back(k == slot ? 1.0f : 0.0f);
  }
};

}  // namespace

EcumenopolisMesh build_ecumenopolis_tile(const EcumenopolisField& field,
                                         const EcumenopolisField::TileId& tile, int detail,
                                         BuildingMethod method) {
  EcumenopolisMesh out;
  building_palette(field.style(), out.mesh.palette);
  const SiteFrame frame = field.tile_frame(tile);
  const double R = frame.radius_m;
  const double datum = field.plate_m(frame.up);
  const double origin_r = R + datum;
  out.mesh.origin[0] = frame.up.x.to_double() * origin_r;
  out.mesh.origin[1] = frame.up.y.to_double() * origin_r;
  out.mesh.origin[2] = frame.up.z.to_double() * origin_r;
  const double ex[3] = {frame.east.x.to_double(), frame.east.y.to_double(), frame.east.z.to_double()};
  const double ny[3] = {frame.north.x.to_double(), frame.north.y.to_double(), frame.north.z.to_double()};
  const double uz[3] = {frame.up.x.to_double(), frame.up.y.to_double(), frame.up.z.to_double()};
  TileWriter w{&out.mesh.vertices};
  const auto place = [&](double x, double y, double z, double* px, double* py, double* pz) {
    const Dir3 d = frame.to_dir(x, y);
    const double r = R + datum + z;
    *px = d.x.to_double() * r - out.mesh.origin[0];
    *py = d.y.to_double() * r - out.mesh.origin[1];
    *pz = d.z.to_double() * r - out.mesh.origin[2];
  };
  const auto emit = [&](const BuildingMesh& building) {
    const std::size_t count = building.vertices.size() / 7;
    for (std::size_t v = 0; v < count; ++v) {
      const float* in = building.vertices.data() + v * 7;
      double px, py, pz;
      place(in[0], in[1], in[2], &px, &py, &pz);
      const double nx = ex[0] * in[3] + ny[0] * in[4] + uz[0] * in[5];
      const double nyy = ex[1] * in[3] + ny[1] * in[4] + uz[1] * in[5];
      const double nz = ex[2] * in[3] + ny[2] * in[4] + uz[2] * in[5];
      w.vertex(px, py, pz, nx, nyy, nz, static_cast<int>(in[6]));
    }
    out.triangle_count += building.triangle_count;
  };
  const std::uint32_t span = 1U << tile.shift;
  const std::uint32_t b0 = tile.ti * span;
  const std::uint32_t b1 = tile.tj * span;
  std::vector<Lot> lots;
  if (detail <= 2) {
    BuildingParams bp;
    bp.method = detail == 0 ? method : (detail == 1 ? BuildingMethod::Grammar : BuildingMethod::Mass);
    const std::uint32_t per_cell = std::min<std::uint32_t>(span, 1U << EcumenopolisField::kArterialShift);
    for (std::uint32_t cj = 0; cj < span; cj += per_cell) {
      for (std::uint32_t ci = 0; ci < span; ci += per_cell) {
        const EcumenopolisField::CellInfo info = field.cell_info(EcumenopolisField::BlockId{tile.face, b0 + ci, b1 + cj});
        if (!info.built) continue;
        for (std::uint32_t j = 0; j < per_cell; ++j) {
          for (std::uint32_t i = 0; i < per_cell; ++i) {
            const EcumenopolisField::BlockId block{tile.face, b0 + ci + i, b1 + cj + j};
            lots.clear();
            if (field.towers_in_block(block, frame, &lots, &info) == 0) continue;
            ++out.block_count;
            for (const Lot& lot : lots) {
              bp.ground_z = static_cast<double>(lot.datum_m) - datum;
              emit(build_building(lot, field.tower_key(block, static_cast<int>(lot.id)), bp));
              ++out.tower_count;
            }
          }
        }
      }
    }
    return out;
  }
  // The far view: per arterial cell one slab at a third of the district
  // budget plus two keyed blocks' tallest towers as boxes (the towers
  // exist identically in the near view; the slab stands for the rest).
  const std::uint32_t cells = span >> EcumenopolisField::kArterialShift;
  const std::uint32_t per_cell = 1U << EcumenopolisField::kArterialShift;
  BuildingParams mass;
  mass.method = BuildingMethod::Mass;
  for (std::uint32_t cj = 0; cj < cells; ++cj) {
    for (std::uint32_t ci = 0; ci < cells; ++ci) {
      const EcumenopolisField::BlockId first{tile.face, b0 + ci * per_cell, b1 + cj * per_cell};
      const EcumenopolisField::BlockId last{tile.face, first.bi + per_cell - 1, first.bj + per_cell - 1};
      // The cell's decisions, shared by all its blocks.
      const EcumenopolisField::CellInfo info = field.cell_info(first);
      if (!info.built) continue;
      lots.clear();
      if (field.towers_in_block(first, frame, &lots, &info) == 0) continue;
      const Lot probe = lots[0];
      // Slab over the cell's block span, inset by the arterial.
      double cx[4];
      double cy[4];
      frame.to_local(field.block_corner(first, 0), &cx[0], &cy[0]);
      frame.to_local(field.block_corner(EcumenopolisField::BlockId{tile.face, last.bi, first.bj}, 1), &cx[1], &cy[1]);
      frame.to_local(field.block_corner(last, 2), &cx[2], &cy[2]);
      frame.to_local(field.block_corner(EcumenopolisField::BlockId{tile.face, first.bi, last.bj}, 3), &cx[3], &cy[3]);
      const double ox = 0.25 * (cx[0] + cx[1] + cx[2] + cx[3]);
      const double oy = 0.25 * (cy[0] + cy[1] + cy[2] + cy[3]);
      const double shrink = 1.0 - EcumenopolisField::kArterialHalfM / (0.5 * field.tile_m(EcumenopolisField::kArterialShift));
      const EcumenopolisField::District& d = info.district;
      Lot slab;
      slab.vertex_count = 4;
      for (int k = 0; k < 4; ++k) {
        slab.footprint[k][0] = static_cast<float>(ox + (cx[k] - ox) * shrink);
        slab.footprint[k][1] = static_cast<float>(oy + (cy[k] - oy) * shrink);
      }
      slab.datum_m = probe.datum_m;
      slab.height_budget_m = static_cast<float>(std::max(20.0, 0.3 * d.height_budget_m));
      slab.usage = LotUsage::Industrial;
      slab.style = field.style();
      const core::Key cell_key = core::derive_child(field.block_key(first), kind::Lot, 0x5148);
      mass.ground_z = static_cast<double>(slab.datum_m) - datum;
      emit(build_building(slab, cell_key, mass));
      out.block_count += per_cell * per_cell;
      // Two keyed blocks of the cell, their tallest tower each.
      const auto pickd = core::draw_point(cell_key, channel::Layout, 0, 0, 1);
      for (int k = 0; k < 2; ++k) {
        const std::uint32_t i = static_cast<std::uint32_t>(civ::u01(pickd[static_cast<std::size_t>(k) * 2]) * per_cell) % per_cell;
        const std::uint32_t j = static_cast<std::uint32_t>(civ::u01(pickd[static_cast<std::size_t>(k) * 2 + 1]) * per_cell) % per_cell;
        const EcumenopolisField::BlockId block{tile.face, first.bi + i, first.bj + j};
        lots.clear();
        if (field.towers_in_block(block, frame, &lots, &info) == 0) continue;
        const Lot* tallest = &lots[0];
        for (const Lot& lot : lots) {
          if (lot.height_budget_m > tallest->height_budget_m) tallest = &lot;
        }
        mass.ground_z = static_cast<double>(tallest->datum_m) - datum;
        emit(build_building(*tallest, field.tower_key(block, static_cast<int>(tallest->id)), mass));
        ++out.tower_count;
      }
    }
  }
  return out;
}

}  // namespace inf::gen
