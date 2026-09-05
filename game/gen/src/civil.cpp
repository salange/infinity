#include "gen/civil.hpp"

#include <algorithm>
#include <cmath>

namespace inf::gen {

using det::Real;

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
double smoothstep(double t) {
  t = clamp01(t);
  return t * t * (3.0 - 2.0 * t);
}

}  // namespace

CivilField::CivilField(const SiteField& sites, const TerrainField& field)
    : sites_(sites),
      field_(field),
      n_(sites.cells_per_face()),
      radius_m_(field.planet().radius_m.to_double()),
      sea_m_(field.planet().sea_level_m.to_double()),
      has_sea_(field.planet().land_fraction.to_double() < 0.999) {
  if (!sites.sites().empty()) {
    material_family_ = sites.sites()[0].style.material_family;
  }
  // Road segments from the plan, with base elevations at their vertices
  // (9 per road), and a per-province index of the segments that can
  // reach it (both endpoint provinces plus any province a vertex lies
  // in).
  const SettlementPlan& plan = sites.plan();
  province_segments_.assign(plan.provinces.size(), {});
  const ProvinceField& provinces = field.provinces();
  for (const Road& road : plan.roads) {
    double elev[9];
    for (int i = 0; i < 9; ++i) {
      elev[i] = field.elevation_m(road.points[i]).to_double();
      if (has_sea_ && elev[i] < sea_m_ + 2.0) elev[i] = sea_m_ + 2.0;  // roads never dip into the sea
    }
    for (int i = 0; i < 8; ++i) {
      RoadSegment seg;
      seg.a = road.points[i];
      seg.b = road.points[i + 1];
      seg.width_m = road.width_m;
      seg.elevation_a_m = static_cast<float>(elev[i]);
      seg.elevation_b_m = static_cast<float>(elev[i + 1]);
      const auto index = static_cast<std::uint32_t>(segments_.size());
      segments_.push_back(seg);
      const CellId ca = provinces.cell_of(seg.a);
      const CellId cb = provinces.cell_of(seg.b);
      const std::uint32_t ia = (static_cast<std::uint32_t>(ca.face) * n_ + ca.ci) * n_ + ca.cj;
      const std::uint32_t ib = (static_cast<std::uint32_t>(cb.face) * n_ + cb.ci) * n_ + cb.cj;
      if (ia < province_segments_.size()) province_segments_[ia].push_back(index);
      if (ib != ia && ib < province_segments_.size()) province_segments_[ib].push_back(index);
      // Endpoint provinces of the whole road too (a segment may pass a
      // province without a vertex inside it near the corners).
      if (road.a < province_segments_.size() && road.a != ia && road.a != ib) province_segments_[road.a].push_back(index);
      if (road.b < province_segments_.size() && road.b != ia && road.b != ib) province_segments_[road.b].push_back(index);
    }
  }
}

int CivilField::candidates(const Dir3& unit_dir, std::uint32_t out[kMaxCandidates]) const {
  // The province under the direction plus probes at +-0.55 cell around
  // it (the features/v1 stencil idea): every province whose site or road
  // could reach here is among them.
  const ProvinceField& provinces = field_.provinces();
  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(unit_dir, &t1, &t2);
  const double step = 0.55 * 2.0 / static_cast<double>(n_);
  int count = 0;
  const auto add = [&](const Dir3& probe) {
    const CellId cell = provinces.cell_of(probe);
    const std::uint32_t index = (static_cast<std::uint32_t>(cell.face) * n_ + cell.ci) * n_ + cell.cj;
    for (int i = 0; i < count; ++i) {
      if (out[i] == index) return;
    }
    if (count < kMaxCandidates) out[count++] = index;
  };
  add(unit_dir);
  for (int di = -1; di <= 1; ++di) {
    for (int dj = -1; dj <= 1; ++dj) {
      if (di == 0 && dj == 0) continue;
      add(Dir3{unit_dir.x + t1.x * Real(step * di) + t2.x * Real(step * dj),
               unit_dir.y + t1.y * Real(step * di) + t2.y * Real(step * dj),
               unit_dir.z + t1.z * Real(step * di) + t2.z * Real(step * dj)});
    }
  }
  return count;
}

bool CivilField::find_site(const Dir3& unit_dir, SiteHit* hit) const {
  std::uint32_t cands[kMaxCandidates];
  const int count = candidates(unit_dir, cands);
  const std::vector<Site>& all = sites_.sites();
  const SettlementPlan& plan = sites_.plan();
  bool found = false;
  for (int c = 0; c < count; ++c) {
    const std::uint32_t province = cands[c];
    if (province >= plan.provinces.size()) continue;
    const Site* site = sites_.site_of(plan.provinces[province].cell);
    if (site == nullptr) continue;
    // Bound: the site radius plus the blend margin, as a chord.
    const double bound = site->radius_m * 1.05 / radius_m_;
    if (chord_sq(unit_dir, site->frame.up).to_double() > bound * bound) continue;
    double x = 0.0;
    double y = 0.0;
    site->frame.to_local(unit_dir, &x, &y);
    const double dist = std::sqrt(x * x + y * y);
    if (dist > site->radius_m * 1.05) continue;
    if (!found || dist < hit->dist_m) {
      hit->site = site;
      hit->dist_m = dist;
      hit->x = x;
      hit->y = y;
      found = true;
    }
  }
  (void)all;
  return found;
}

bool CivilField::find_road(const Dir3& unit_dir, double* lateral_m, double* width_m,
                           double* centre_elevation_m) const {
  if (segments_.empty()) {
    return false;
  }
  std::uint32_t cands[kMaxCandidates];
  const int count = candidates(unit_dir, cands);
  bool found = false;
  double best = 1.0e300;
  for (int c = 0; c < count; ++c) {
    if (cands[c] >= province_segments_.size()) continue;
    for (const std::uint32_t si : province_segments_[cands[c]]) {
      const RoadSegment& seg = segments_[si];
      // Point-to-segment in the tangent plane (chord space scaled by R).
      const Dir3 ab{seg.b.x - seg.a.x, seg.b.y - seg.a.y, seg.b.z - seg.a.z};
      const Dir3 ap{unit_dir.x - seg.a.x, unit_dir.y - seg.a.y, unit_dir.z - seg.a.z};
      const double len2 = dot(ab, ab).to_double();
      double t = len2 > 0.0 ? dot(ap, ab).to_double() / len2 : 0.0;
      t = clamp01(t);
      const Dir3 q{seg.a.x + ab.x * Real(t), seg.a.y + ab.y * Real(t), seg.a.z + ab.z * Real(t)};
      const double d = std::sqrt(chord_sq(unit_dir, q).to_double()) * radius_m_;
      if (d < best) {
        best = d;
        *lateral_m = d;
        *width_m = seg.width_m;
        *centre_elevation_m = seg.elevation_a_m + (seg.elevation_b_m - seg.elevation_a_m) * t;
        found = true;
      }
    }
  }
  return found && best < 2.0 * (*width_m);
}

bool CivilField::near(const Dir3& unit_dir) const {
  SiteHit hit;
  if (find_site(unit_dir, &hit)) return true;
  double lateral = 0.0;
  double width = 0.0;
  double centre = 0.0;
  return find_road(unit_dir, &lateral, &width, &centre);
}

det::Real CivilField::modify(const Dir3& unit_dir, det::Real base_m, const BaseEval& base_at) const {
  (void)base_at;
  double h = base_m.to_double();
  // Roads first (they lie under the plateau blend inside sites).
  double lateral = 0.0;
  double width = 0.0;
  double centre = 0.0;
  if (find_road(unit_dir, &lateral, &width, &centre)) {
    // Full grade within half a width, shoulders over one width.
    const double w = 1.0 - smoothstep((lateral - 0.5 * width) / width);
    h = h + (centre - h) * w;
  }
  SiteHit hit;
  if (find_site(unit_dir, &hit)) {
    const Site& site = *hit.site;
    double target = sites_.plateau_m(site, hit.x, hit.y);
    if (site.domed) {
      // Domed colonies: individual pads — the lattice cell's centre
      // decides, the plateau only inside the lot pitch.
      target = site.datum_m;
    }
    const double R = site.radius_m;
    const double w = 1.0 - smoothstep((hit.dist_m - 0.75 * R) / (0.25 * R));
    h = h + (target - h) * w;
  }
  if (has_sea_ && h < base_m.to_double() && h < sea_m_ + 1.0) {
    h = base_m.to_double();  // never lower ground into the sea
  }
  return Real(h);
}

HeightModifier::Urban CivilField::urban(const Dir3& unit_dir) const {
  Urban out;
  out.family = material_family_;
  double lateral = 0.0;
  double width = 0.0;
  double centre = 0.0;
  if (find_road(unit_dir, &lateral, &width, &centre)) {
    out.weight = 1.0 - smoothstep((lateral - 0.5 * width) / (0.5 * width));
  }
  SiteHit hit;
  if (find_site(unit_dir, &hit)) {
    const Site& site = *hit.site;
    const double R = site.radius_m;
    const double core = 1.0 - smoothstep((hit.dist_m - 0.7 * R) / (0.3 * R));
    out.weight = std::max(out.weight, core * (site.ruined ? 0.5 : 0.9));
    out.family = site.style.material_family;
    out.night_light = site.light_density * core;
  }
  return out;
}

}  // namespace inf::gen
