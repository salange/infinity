#include "gen/sites.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "core/det/trig.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using civ::u01;
using det::Real;

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// Per-lot cheap hash on the sanctioned mixer path: derived key ^ mix(coords).
std::uint64_t lot_hash(std::uint64_t lattice, std::int64_t bx, std::int64_t by, std::int64_t i,
                       std::uint64_t salt) {
  const std::uint64_t packed = (static_cast<std::uint64_t>(bx) * 0x9E3779B97F4A7C15ULL) ^
                               (static_cast<std::uint64_t>(by) * 0xC2B2AE3D27D4EB4FULL) ^
                               (static_cast<std::uint64_t>(i) * 0x165667B19E3779F9ULL) ^ salt;
  return det::mix64(lattice ^ det::mix64(packed));
}
double hash01(std::uint64_t h) { return static_cast<double>(h >> 11U) * 0x1.0p-53; }

// Deterministic trig on the det path (no libm in gen; ci grep gate).
inline double cos_d(double a) {
  det::Real s(0.0), c(0.0);
  det::fast_sin_cos(det::Real(a), &s, &c);
  return c.to_double();
}
inline double sin_d(double a) {
  det::Real s(0.0), c(0.0);
  det::fast_sin_cos(det::Real(a), &s, &c);
  return s.to_double();
}
// atan2 by a rational approximation (max error ~1e-4 rad): cosmetic
// orientations only, and only +-*/ so every platform agrees.
inline double atan2_d(double y, double x) {
  const double ax = x < 0.0 ? -x : x;
  const double ay = y < 0.0 ? -y : y;
  const double mx = ax > ay ? ax : ay;
  if (mx <= 0.0) return 0.0;
  const double mn = ax > ay ? ay : ax;
  const double a = mn / mx;
  const double s = a * a;
  double r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
  if (ay > ax) r = 1.57079637 - r;
  if (x < 0.0) r = 3.14159274 - r;
  return y < 0.0 ? -r : r;
}

}  // namespace

void SiteFrame::to_local(const Dir3& dir, double* x, double* y) const {
  *x = dot(dir, east).to_double() * radius_m;
  *y = dot(dir, north).to_double() * radius_m;
}

Dir3 SiteFrame::to_dir(double x, double y) const {
  const double ex = x / radius_m;
  const double ny = y / radius_m;
  const double uz = std::sqrt(std::max(0.0, 1.0 - ex * ex - ny * ny));
  return normalize(Dir3{up.x * Real(uz) + east.x * Real(ex) + north.x * Real(ny),
                        up.y * Real(uz) + east.y * Real(ex) + north.y * Real(ny),
                        up.z * Real(uz) + east.z * Real(ex) + north.z * Real(ny)});
}

double ring_radius_m(int tier) {
  return tier_radius_m(static_cast<SettlementTier>(std::clamp(tier, 0, 7)));
}

int Site::ring_of(double dist_m) {
  for (int t = 1; t <= 7; ++t) {
    if (dist_m <= ring_radius_m(t)) {
      return t;
    }
  }
  return 8;
}

SiteField::SiteField(const core::Key& body_entity_key, const TerrainField& field,
                     const SettlementPlan& plan, const RaceParams& race,
                     const std::vector<FactionParams>& factions, const CivState& state)
    : sites_key_(core::derive_named(body_entity_key, name::SitesV1)),
      field_(field),
      plan_(plan),
      n_(plan.cells_per_face),
      radius_m_(field.planet().radius_m.to_double()) {
  site_index_.assign(plan.provinces.size(), -1);
  for (const ProvinceSite& province : plan.provinces) {
    if (!province.settled || province.tier == SettlementTier::None ||
        province.tier == SettlementTier::Ecumenopolis) {
      continue;
    }
    Site site = build_site(province, plan, race, factions, state);
    if (!site.valid) {
      continue;
    }
    site_index_[province.index] = static_cast<std::int32_t>(sites_.size());
    sites_.push_back(std::move(site));
  }
}

const Site* SiteField::site_of(const CellId& cell) const {
  const std::size_t index =
      (static_cast<std::size_t>(cell.face) * n_ + cell.ci) * n_ + cell.cj;
  if (index >= site_index_.size() || site_index_[index] < 0) {
    return nullptr;
  }
  return &sites_[static_cast<std::size_t>(site_index_[index])];
}

Site SiteField::build_site(const ProvinceSite& province, const SettlementPlan& plan,
                           const RaceParams& race, const std::vector<FactionParams>& factions,
                           const CivState& state) const {
  Site site;
  site.province = province.index;
  site.cell = province.cell;
  site.key = core::derive_child(sites_key_, kind::Site, province.cell.face, province.cell.ci,
                                province.cell.cj);
  const PlanetParams& planet = field_.planet();
  const bool has_sea = planet.land_fraction.to_double() < 0.999;
  site.sea_m = planet.sea_level_m.to_double();
  site.tier = static_cast<int>(province.tier);
  site.max_tier = static_cast<int>(province.max_tier);
  site.radius_m = ring_radius_m(site.tier);
  site.progress = province.site_progress;
  site.ruined = plan.ruined;
  site.domed = plan.domed;
  site.capital = province.capital;
  site.coastal = province.coastal;
  site.river = province.river;

  // --- centre: 16 keyed candidates inside the province (within 0.6 of
  // the cell radius so the site never crosses the border), scored by
  // flatness x above-sea x coast/river bonus. The best wins.
  const double cell_chord = 2.0 / static_cast<double>(n_) * 0.9;  // ~cell edge on the unit sphere
  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(province.centre, &t1, &t2);
  Dir3 best_dir = province.centre;
  double best_score = -1.0;
  double best_elevation = 0.0;
  for (int k = 0; k < 16; ++k) {
    const auto d = core::draw_point(site.key, channel::Layout, 0, k, 0);
    const double r = std::sqrt(u01(d[0])) * 0.6 * 0.5 * cell_chord;
    const double az = u01(d[1]) * 6.283185307179586;
    Real sine(0.0);
    Real cosine(0.0);
    det::fast_sin_cos(Real(az), &sine, &cosine);
    const double ox = cosine.to_double() * r;
    const double oy = sine.to_double() * r;
    const Dir3 candidate = normalize(Dir3{province.centre.x + t1.x * Real(ox) + t2.x * Real(oy),
                                          province.centre.y + t1.y * Real(ox) + t2.y * Real(oy),
                                          province.centre.z + t1.z * Real(ox) + t2.z * Real(oy)});
    // The centre stays inside its own province cell (the civil lookups
    // and the diff addressing key sites by province).
    if (!(field_.provinces().cell_of(candidate) == province.cell)) {
      continue;
    }
    const TerrainField::ElevationD e = field_.elevation_and_gradient(candidate);
    const double elevation = e.elevation_m.to_double();
    const double grade = std::sqrt(dot(e.slope, e.slope).to_double());
    double score = (1.0 - clamp01(grade / 0.35));
    if (has_sea) {
      const double above = elevation - site.sea_m;
      if (above < 5.0) {
        score *= 0.0;
      } else {
        // Coasts and low ground are prized by organics.
        score *= 1.0 + 0.4 / (1.0 + above / 300.0);
      }
    }
    if (score > best_score) {
      best_score = score;
      best_dir = candidate;
      best_elevation = elevation;
    }
  }
  if (best_score < 0.0) {
    // No candidate inside the cell: the representative itself.
    best_dir = province.centre;
    best_elevation = field_.elevation_m(best_dir).to_double();
    best_score = has_sea && best_elevation < site.sea_m + 5.0 ? 0.0 : 0.1;
  }
  site.valid = best_score > 0.0;
  if (!site.valid) {
    return site;
  }
  // --- frame ------------------------------------------------------------------
  site.frame.up = best_dir;
  site.frame.radius_m = radius_m_;
  {
    const Dir3 north_axis{Real(0.0), Real(0.0), Real(1.0)};
    Dir3 east{north_axis.y * best_dir.z - north_axis.z * best_dir.y,
              north_axis.z * best_dir.x - north_axis.x * best_dir.z,
              north_axis.x * best_dir.y - north_axis.y * best_dir.x};
    if (dot(east, east).to_double() < 1e-6) {
      tangent_basis(best_dir, &east, &t2);
    }
    east = normalize(east);
    site.frame.east = east;
    site.frame.north = normalize(Dir3{best_dir.y * east.z - best_dir.z * east.y,
                                      best_dir.z * east.x - best_dir.x * east.z,
                                      best_dir.x * east.y - best_dir.y * east.x});
  }
  // --- datum: median of 9 samples within half the radius -----------------
  {
    double samples[9];
    samples[0] = best_elevation;
    for (int k = 1; k < 9; ++k) {
      const double az = (k - 1) * 0.7853981633974483;
      Real sine(0.0);
      Real cosine(0.0);
      det::fast_sin_cos(Real(az), &sine, &cosine);
      const double r = 0.5 * site.radius_m;
      samples[k] = field_.elevation_m(site.frame.to_dir(cosine.to_double() * r, sine.to_double() * r))
                       .to_double();
    }
    std::sort(samples, samples + 9);
    site.datum_m = samples[4];
    if (has_sea && site.datum_m < site.sea_m + 3.0) {
      site.datum_m = site.sea_m + 3.0;  // never below sea
    }
  }
  // --- family (design 14.2) ------------------------------------------------
  {
    const auto d = core::draw_point(site.key, channel::Layout, 1, 0, 0);
    const RaceTypeInfo& info = race_type_info(race.type);
    LayoutFamily family = u01(d[0]) < 0.7 ? info.layout_primary : info.layout_secondary;
    const FactionParams* faction =
        state.faction_index >= 0 && state.faction_index < static_cast<int>(factions.size())
            ? &factions[static_cast<std::size_t>(state.faction_index)]
            : nullptr;
    if (race.type == RaceType::Humanoid && faction != nullptr) {
      switch (faction->type) {
        case FactionType::Government: family = site.capital || u01(d[1]) < 0.35 ? LayoutFamily::Radial : LayoutFamily::Grid; break;
        case FactionType::Independent:
        case FactionType::Outlaw: family = LayoutFamily::Organic; break;
        case FactionType::AlignedMachine:
        case FactionType::RenegadeMachine: family = LayoutFamily::Lattice; break;
        case FactionType::Count: break;
      }
    }
    if (site.coastal && u01(d[2]) < 0.5 && race.type != RaceType::Machine &&
        race.type != RaceType::Insectoid && race.type != RaceType::Crystalline) {
      family = LayoutFamily::Linear;
    }
    if (best_score < 0.5 && race.type != RaceType::Insectoid && race.type != RaceType::Crystalline) {
      family = LayoutFamily::Terraced;  // steep ground
    }
    if (site.domed) {
      family = race.type == RaceType::Machine && u01(d[3]) < 0.5 ? LayoutFamily::Lattice
                                                                  : LayoutFamily::Domed;
    }
    site.family = family;
    site.axis_rad = u01(d[3]) * 3.14159265358979323846;
    // Linear sites run along the shore: the local downhill direction is
    // roughly toward the water, so the axis is perpendicular to it.
    const TerrainField::ElevationD e = field_.elevation_and_gradient(best_dir);
    double sx = 0.0;
    double sy = 0.0;
    site.frame.to_local(Dir3{best_dir.x + e.slope.x, best_dir.y + e.slope.y, best_dir.z + e.slope.z}, &sx, &sy);
    const double downhill = det::atan2(Real(-sy), Real(-sx)).to_double();
    if (family == LayoutFamily::Linear || family == LayoutFamily::Terraced) {
      site.axis_rad = downhill + 1.5707963267948966;
    }
    // --- lattice parameters by family. Fixed per site (never by tier):
    // the block lattice must not move when the site grows a ring. Lot
    // size scales with the RING a block lies in (lots_in_block), so the
    // old town keeps its small lots and the outer rings build bigger.
    switch (family) {
      case LayoutFamily::Grid: site.block_m = 100.0; site.street_m = 14.0; site.lot_m = 22.0; site.density = 0.85; break;
      case LayoutFamily::Radial: site.block_m = 90.0; site.street_m = 12.0; site.lot_m = 20.0; site.density = 0.85; break;
      case LayoutFamily::Organic: site.block_m = 66.0; site.street_m = 8.0; site.lot_m = 16.0; site.density = 0.6; break;
      case LayoutFamily::Linear: site.block_m = 76.0; site.street_m = 10.0; site.lot_m = 18.0; site.density = 0.7; break;
      case LayoutFamily::Hive: site.block_m = 56.0; site.street_m = 0.0; site.lot_m = 26.0; site.density = 0.75; break;
      case LayoutFamily::Crystal: site.block_m = 66.0; site.street_m = 0.0; site.lot_m = 18.0; site.density = 0.55; break;
      case LayoutFamily::Lattice: site.block_m = 80.0; site.street_m = 10.0; site.lot_m = 24.0; site.density = 0.9; break;
      case LayoutFamily::Terraced: site.block_m = 66.0; site.street_m = 8.0; site.lot_m = 14.0; site.density = 0.65; break;
      case LayoutFamily::Domed: site.block_m = 120.0; site.street_m = 6.0; site.lot_m = 40.0; site.density = 0.45; break;
      case LayoutFamily::Count: break;
    }
  }
  // --- style base (design 15) ----------------------------------------------
  {
    StyleVector& s = site.style;
    s.race_type = race.type;
    s.race_variant = race.variant;
    for (int i = 0; i < 2; ++i) for (int c = 0; c < 3; ++c) s.palette[i][c] = race.palette[i][c];
    s.material_family = race.material_family;
    s.faction_type = state.faction_type;
    s.tech_tier = race.tech_tier;
    s.tier = static_cast<SettlementTier>(site.tier);
    s.level = state.level;
    s.ruined = site.ruined;
    s.domed = site.domed;
    double regularity = 0.5;
    double wear = 0.2;
    double ornament = 0.3;
    double lights = 0.3 + 0.1 * site.tier;
    switch (state.faction_type) {
      case FactionType::Government: regularity += 0.35; ornament += 0.3; wear -= 0.1; break;
      case FactionType::Independent: regularity -= 0.1; ornament += 0.05; break;
      case FactionType::Outlaw: regularity -= 0.3; wear += 0.45; ornament -= 0.2; lights *= 0.35; break;
      case FactionType::AlignedMachine: regularity += 0.4; wear -= 0.15; lights += 0.2; break;
      case FactionType::RenegadeMachine: regularity += 0.1; wear += 0.25; lights += 0.1; break;
      case FactionType::Count: break;
    }
    if (site.ruined) { wear = 1.0; lights = 0.0; }
    s.regularity = static_cast<float>(clamp01(regularity));
    s.wear = static_cast<float>(clamp01(wear));
    s.ornament = static_cast<float>(clamp01(ornament));
    s.light_density = static_cast<float>(clamp01(lights));
    site.light_density = s.light_density;
  }
  build_arterials(&site);
  return site;
}

void SiteField::build_arterials(Site* site) const {
  const auto add = [&](std::vector<std::pair<double, double>> pts, double width) {
    Arterial a;
    a.width_m = static_cast<float>(width);
    for (const auto& p : pts) {
      a.xy.push_back(static_cast<float>(p.first));
      a.xy.push_back(static_cast<float>(p.second));
    }
    site->arterials.push_back(std::move(a));
  };
  const double R = site->radius_m;
  const double ca = cos_d(site->axis_rad);
  const double sa = sin_d(site->axis_rad);
  const auto rot = [&](double x, double y) {
    return std::pair<double, double>{x * ca - y * sa, x * sa + y * ca};
  };
  const double w = site->street_m * 1.6 + 6.0;
  switch (site->family) {
    case LayoutFamily::Grid:
    case LayoutFamily::Lattice:
      add({rot(-R, 0.0), rot(R, 0.0)}, w);
      add({rot(0.0, -R), rot(0.0, R)}, w);
      break;
    case LayoutFamily::Radial: {
      const int spokes = site->tier >= 5 ? 6 : 4;
      for (int i = 0; i < spokes; ++i) {
        const double az = site->axis_rad + i * 6.283185307179586 / spokes;
        add({{0.0, 0.0}, {cos_d(az) * R, sin_d(az) * R}}, w);
      }
      break;
    }
    case LayoutFamily::Linear:
      add({rot(-R, 0.0), rot(R, 0.0)}, w);
      break;
    case LayoutFamily::Organic:
    case LayoutFamily::Terraced: {
      // Three wandering paths from the centre, keyed.
      for (int i = 0; i < 3; ++i) {
        const auto d = core::draw_point(site->key, channel::Layout, 2, i, 0);
        double az = site->axis_rad + i * 2.0943951023931953 + (u01(d[0]) - 0.5) * 0.8;
        std::vector<std::pair<double, double>> pts{{0.0, 0.0}};
        double x = 0.0;
        double y = 0.0;
        for (int s = 1; s <= 4; ++s) {
          az += (u01(d[s % 4]) - 0.5) * 0.9;
          x += cos_d(az) * R / 4.0;
          y += sin_d(az) * R / 4.0;
          pts.emplace_back(x, y);
        }
        add(pts, w * 0.8);
      }
      break;
    }
    case LayoutFamily::Crystal: {
      for (int i = 0; i < 5; ++i) {
        const double az = site->axis_rad + i * 1.2566370614359172;
        add({{0.0, 0.0}, {cos_d(az) * R, sin_d(az) * R}}, 4.0);
      }
      break;
    }
    case LayoutFamily::Hive:
    case LayoutFamily::Domed:
    case LayoutFamily::Count: break;
  }
}

bool SiteField::on_street(const Site& site, double x, double y) const {
  // Streets of the square lattice: every block edge; families without
  // streets never mask.
  if (site.street_m <= 0.0) {
    return false;
  }
  const double B = site.block_m;
  const double fx = x - std::floor(x / B) * B;
  const double fy = y - std::floor(y / B) * B;
  const double half = 0.5 * site.street_m;
  return fx < half || fx > B - half || fy < half || fy > B - half;
}

double SiteField::plateau_m(const Site& site, double x, double y) const {
  if (site.family != LayoutFamily::Terraced) {
    return site.datum_m;
  }
  // Terraces: steps along the downhill axis (perpendicular to the site
  // axis), each kTerraceStepM high, following the mean slope through
  // the centre. The datum sits at the centre; steps rise uphill.
  const double ca = cos_d(site.axis_rad);
  const double sa = sin_d(site.axis_rad);
  const double across = -x * sa + y * ca;  // signed distance along the downhill axis (positive = uphill)
  const double run = 60.0;                 // metres per terrace
  const double step = std::floor(across / run + 0.5);
  return site.datum_m + step * kTerraceStepM;
}

void SiteField::lots_in_block(const Site& site, int bx, int by, std::vector<Lot>* out) const {
  const double B = site.block_m;
  const double x0 = bx * B;
  const double y0 = by * B;
  // Block outside the current radius: nothing (cheap reject by the
  // block's nearest corner).
  const double cx = std::clamp(0.0, x0, x0 + B);
  const double cy = std::clamp(0.0, y0, y0 + B);
  if (cx * cx + cy * cy > site.radius_m * site.radius_m) {
    return;
  }
  const std::uint64_t lattice = core::lattice_key(site.key, channel::Layout);
  // The ring this BLOCK lies in (by its centre) fixes its lot scale
  // forever — hamlet lots stay small inside the city that grows around
  // them.
  const double bcx = x0 + 0.5 * B;
  const double bcy = y0 + 0.5 * B;
  const int block_ring = std::clamp(Site::ring_of(std::sqrt(bcx * bcx + bcy * bcy)), 1, 7);
  const double lot_size_base = site.lot_m * (0.6 + 0.1 * block_ring);
  // Lot lattice inside the block: pitch = lot + a gap; rows along both
  // block edges face the streets, the block interior holds a courtyard
  // for the street families.
  const double pitch = lot_size_base + 0.35 * lot_size_base;
  const int per_edge = std::max(1, static_cast<int>(B / pitch));
  const double margin = 0.5 * site.street_m + 0.5 * pitch;
  const bool streets = site.street_m > 0.0;
  int index = 0;
  for (int j = 0; j < per_edge; ++j) {
    for (int i = 0; i < per_edge; ++i) {
      ++index;
      const bool edge_cell = !streets || i == 0 || j == 0 || i == per_edge - 1 || j == per_edge - 1;
      if (!edge_cell && per_edge > 2 && site.family != LayoutFamily::Hive &&
          site.family != LayoutFamily::Crystal && site.family != LayoutFamily::Domed) {
        continue;  // courtyard
      }
      const std::uint64_t h = lot_hash(lattice, bx, by, index, 0x51);
      if (hash01(h) > site.density) {
        continue;
      }
      double lx = x0 + margin + i * (B - 2.0 * margin) / std::max(1, per_edge - 1);
      double ly = y0 + margin + j * (B - 2.0 * margin) / std::max(1, per_edge - 1);
      if (per_edge == 1) {
        lx = x0 + 0.5 * B;
        ly = y0 + 0.5 * B;
      }
      // Jitter: none for grids, up to 40 % of a pitch for organic families.
      const double jitter = site.family == LayoutFamily::Grid || site.family == LayoutFamily::Lattice ||
                                    site.family == LayoutFamily::Radial
                                ? 0.05
                                : (site.family == LayoutFamily::Organic ? 0.4 : 0.2);
      lx += (hash01(lot_hash(lattice, bx, by, index, 0x52)) - 0.5) * jitter * pitch;
      ly += (hash01(lot_hash(lattice, bx, by, index, 0x53)) - 0.5) * jitter * pitch;
      // Rotate the whole lattice by the site axis for grid/linear/terraced.
      double wx = lx;
      double wy = ly;
      if (site.family == LayoutFamily::Grid || site.family == LayoutFamily::Linear ||
          site.family == LayoutFamily::Terraced || site.family == LayoutFamily::Lattice) {
        const double ca = cos_d(site.axis_rad);
        const double sa = sin_d(site.axis_rad);
        wx = lx * ca - ly * sa;
        wy = lx * sa + ly * ca;
      }
      const double dist = std::sqrt(wx * wx + wy * wy);
      if (dist > site.radius_m) {
        continue;
      }
      if (site.family == LayoutFamily::Linear && std::fabs(-wx * sin_d(site.axis_rad) + wy * cos_d(site.axis_rad)) > 0.35 * site.radius_m) {
        continue;  // a ribbon along the axis
      }
      // Arterial clearance.
      bool blocked = false;
      for (const Arterial& a : site.arterials) {
        const double half = 0.5 * a.width_m + 0.5 * site.lot_m;
        for (std::size_t s = 0; s + 3 < a.xy.size(); s += 2) {
          const double ax = a.xy[s];
          const double ay = a.xy[s + 1];
          const double bxp = a.xy[s + 2];
          const double byp = a.xy[s + 3];
          const double vx = bxp - ax;
          const double vy = byp - ay;
          const double len2 = vx * vx + vy * vy;
          double t = len2 > 0.0 ? ((wx - ax) * vx + (wy - ay) * vy) / len2 : 0.0;
          t = clamp01(t);
          const double px = ax + vx * t - wx;
          const double py = ay + vy * t - wy;
          if (px * px + py * py < half * half) {
            blocked = true;
            break;
          }
        }
        if (blocked) break;
      }
      if (blocked) {
        continue;
      }
      // The ring that created this lot, and its reveal threshold inside
      // that ring: 60 % radial position (inner lots first), 40 % keyed.
      const int ring = Site::ring_of(dist);
      if (ring > site.tier) {
        continue;
      }
      const double r_in = ring_radius_m(ring - 1);
      const double r_out = ring_radius_m(ring);
      const double radial = r_out > r_in ? clamp01((dist - r_in) / (r_out - r_in)) : 0.0;
      const double reveal = 0.6 * radial + 0.4 * hash01(lot_hash(lattice, bx, by, index, 0x54));
      float construction = 1.0f;
      if (ring == site.tier) {
        if (reveal >= site.progress) {
          continue;  // not yet built
        }
        const double behind = site.progress - reveal;
        if (behind < 0.03 && !site.ruined) {
          construction = static_cast<float>(behind / 0.03);  // ruins: nothing is being built
        }
      }
      Lot lot;
      lot.id = static_cast<std::uint32_t>((static_cast<std::uint32_t>(bx & 0xFFF) << 20) |
                                          (static_cast<std::uint32_t>(by & 0xFFF) << 8) |
                                          static_cast<std::uint32_t>(index & 0xFF));
      lot.order = static_cast<std::uint32_t>(reveal * 1.0e6);
      lot.tier = static_cast<std::uint8_t>(ring);
      // Footprint: a rectangle (street families), an octagon (hives,
      // domes), a rotated square (crystal), a hexagon (lattice).
      const double size = lot_size_base * (0.75 + 0.5 * hash01(lot_hash(lattice, bx, by, index, 0x55)));
      const double aspect = 0.7 + 0.6 * hash01(lot_hash(lattice, bx, by, index, 0x56));
      const double rotation = site.family == LayoutFamily::Organic
                                  ? (hash01(lot_hash(lattice, bx, by, index, 0x57)) - 0.5) * 0.6 + site.axis_rad
                                  : site.axis_rad;
      const double cr = cos_d(rotation);
      const double sr = sin_d(rotation);
      const auto put = [&](int k, double px, double py) {
        lot.footprint[k][0] = static_cast<float>(wx + px * cr - py * sr);
        lot.footprint[k][1] = static_cast<float>(wy + px * sr + py * cr);
      };
      if (site.family == LayoutFamily::Hive || site.family == LayoutFamily::Domed) {
        lot.vertex_count = 8;
        for (int k = 0; k < 8; ++k) {
          const double az = k * 0.7853981633974483;
          put(k, cos_d(az) * 0.5 * size, sin_d(az) * 0.5 * size);
        }
      } else if (site.family == LayoutFamily::Lattice) {
        lot.vertex_count = 6;
        for (int k = 0; k < 6; ++k) {
          const double az = k * 1.0471975511965976;
          put(k, cos_d(az) * 0.5 * size, sin_d(az) * 0.5 * size);
        }
      } else {
        lot.vertex_count = 4;
        const double hx = 0.5 * size * aspect;
        const double hy = 0.5 * size / aspect;
        put(0, -hx, -hy);
        put(1, hx, -hy);
        put(2, hx, hy);
        put(3, -hx, hy);
      }
      lot.datum_m = static_cast<float>(plateau_m(site, wx, wy) - site.datum_m);
      // Usage by radius inside the RING and by the ring; heights by usage
      // and ring — every quantity reads the ring that created the lot,
      // never the site's current tier, so growth is additive.
      const double rel = r_out > 0.0 ? dist / r_out : 0.0;
      const double sector = atan2_d(wy, wx) - site.axis_rad;
      const double use_roll = hash01(lot_hash(lattice, bx, by, index, 0x58));
      if (rel < 0.12 && ring >= 3 && use_roll < 0.6) {
        lot.usage = LotUsage::Civic;
      } else if (rel > 0.55 && rel < 0.85 && cos_d(sector - 2.3) > 0.5 && use_roll < 0.7) {
        lot.usage = LotUsage::Industrial;
      } else if (rel > 0.8 && ring <= 4 && use_roll < 0.5 &&
                 (site.family == LayoutFamily::Organic || site.family == LayoutFamily::Grid ||
                  site.family == LayoutFamily::Linear)) {
        lot.usage = LotUsage::Agricultural;
      } else if (use_roll > 0.985) {
        lot.usage = LotUsage::Pad;
      } else if (rel < 0.05 && site.family == LayoutFamily::Radial && use_roll < 0.9) {
        lot.usage = LotUsage::Monument;
      } else {
        lot.usage = LotUsage::Residential;
      }
      double height = 4.0 + 2.5 * ring;
      const double centre_boost = ring >= 5 ? (1.0 - rel) * (1.0 - rel) * 60.0 * (ring - 4) : 0.0;
      switch (lot.usage) {
        case LotUsage::Civic: height = 10.0 + 6.0 * ring + centre_boost; break;
        case LotUsage::Industrial: height = 8.0 + 2.0 * ring; break;
        case LotUsage::Agricultural: height = 3.5; break;
        case LotUsage::Pad: height = 1.0; break;
        case LotUsage::Monument: height = 20.0 + 10.0 * ring; break;
        case LotUsage::Residential: height += centre_boost * 0.6; break;
        case LotUsage::Count: break;
      }
      height *= 0.7 + 0.6 * hash01(lot_hash(lattice, bx, by, index, 0x59));
      if (site.domed) height = std::min(height, 0.5 * size);  // domes are hemispheres
      lot.height_budget_m = static_cast<float>(height);
      lot.style = site.style;
      lot.style.construction = construction;
      out->push_back(lot);
    }
  }
}

void SiteField::all_lots(const Site& site, std::vector<Lot>* out) const {
  const int reach = static_cast<int>(std::ceil(site.radius_m / site.block_m)) + 1;
  for (int by = -reach; by <= reach; ++by) {
    for (int bx = -reach; bx <= reach; ++bx) {
      lots_in_block(site, bx, by, out);
    }
  }
}

std::uint32_t SiteField::visible_count(const Site& site, float progress) const {
  Site probe = site;
  probe.progress = progress;
  std::vector<Lot> lots;
  all_lots(probe, &lots);
  return static_cast<std::uint32_t>(lots.size());
}

}  // namespace inf::gen
