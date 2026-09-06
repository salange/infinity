#include "city/site_build.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "city/architecture.hpp"
#include "city/flora.hpp"
#include "city/materials.hpp"
#include "city/props.hpp"
#include "city/rng.hpp"
#include "city/streets.hpp"
#include "city/towers.hpp"
#include "core/det/trig.hpp"
#include "core/key.hpp"
#include "gen/names.hpp"

namespace inf::city {

namespace {

// The lot lattice of grid-like families is rotated by the site axis
// (sites.cpp lots_in_block): block indices live in lattice space, lot
// footprints and the focus in site-local space.
struct Lattice {
  bool rotated{false};
  double ca{1.0};
  double sa{0.0};
  void to_lattice(double wx, double wy, double* lx, double* ly) const {
    *lx = rotated ? wx * ca + wy * sa : wx;
    *ly = rotated ? -wx * sa + wy * ca : wy;
  }
};

Lattice lattice_of(const gen::Site& site) {
  Lattice l;
  l.rotated = site.family == gen::LayoutFamily::Grid || site.family == gen::LayoutFamily::Linear ||
              site.family == gen::LayoutFamily::Terraced || site.family == gen::LayoutFamily::Lattice;
  if (l.rotated) {
    det::Real sn(0.0), cs(0.0);
    det::fast_sin_cos(det::Real(site.axis_rad), &sn, &cs);
    l.ca = cs.to_double();
    l.sa = sn.to_double();
  }
  return l;
}

// Site-local (east, north) -> scene (x, z).
Vec2 to_scene(double x_east, double y_north) {
  return Vec2{static_cast<float>(x_east), static_cast<float>(-y_north)};
}

constexpr float kCurb = 0.15f;

// How a settlement grows (T0022 A.2): one row per tier, the demo's six
// size classes mapped onto the design's tiers (Outpost 0, Hamlet and
// Village 1, Town 2, City 3, Metropolis 4, Capital 5; the ecumenopolis
// keeps the capital's row until it has its own renderer). The radius and
// the lots come from sites/v1; this table decides what the city layer
// puts on them: the capitol stage, the towers (density on the demo's
// ~130 m blocks, scaled to the site's block pitch; an absolute core
// radius with 0.35 of the density out to 1.6 x), the plaza odds, the
// fill probability (an unbuilt lot stays a lawn — the voids that make
// small settlements read as sparse), the prop budgets, the per-city
// tower geometry budgets (full detail only for the core towers) and the
// share of the radius beyond which standard buildings drop their trims.
// Probabilities interpolate toward the next tier's row by the site's
// progress, so growth is continuous inside a tier; the kit (stage,
// heroes, groups, tower density) changes at the tier thresholds.
struct GrowthRules {
  int stage;
  int max_floors;
  float tower_density;   // per ~130 m block, inside the core
  double core_m;         // full density inside, 0.35 of it out to 1.6 x
  bool heroes;
  bool groups;
  float plaza_p;
  float fill_p;
  int trees;
  int lamps;
  int overpasses;
  int hi_budget;         // towers built at full detail
  int mid_budget;        // towers built down to the near-context level
  float cheap_beyond;    // t (of the radius) beyond which standards lose trims and balconies
};
GrowthRules growth_row(int tier) {
  switch (static_cast<gen::SettlementTier>(std::clamp(tier, 1, 8))) {
    case gen::SettlementTier::Outpost: return {0, 0, 0.0f, 0.0, false, false, 0.0f, 0.0f, 4, 2, 0, 0, 0, 1.0f};
    case gen::SettlementTier::Hamlet: return {1, 0, 0.0f, 0.0, false, false, 0.30f, 0.45f, 30, 30, 0, 0, 0, 1.0f};
    case gen::SettlementTier::Village: return {1, 0, 0.0f, 0.0, false, false, 0.30f, 0.55f, 40, 60, 1, 0, 0, 1.0f};
    case gen::SettlementTier::Town: return {2, 12, 0.10f, 250.0, false, false, 0.20f, 0.65f, 90, 90, 2, 10, 24, 1.0f};
    case gen::SettlementTier::City: return {3, 24, 0.45f, 350.0, false, false, 0.14f, 0.82f, 140, 120, 4, 10, 24, 1.0f};
    case gen::SettlementTier::Metropolis: return {4, 40, 0.70f, 450.0, true, true, 0.12f, 0.92f, 200, 160, 6, 10, 24, 0.6f};
    default: return {5, 56, 0.90f, 450.0, true, true, 0.10f, 1.00f, 200, 160, 8, 14, 40, 0.4f};
  }
}
GrowthRules growth_rules(const gen::Site& site) {
  GrowthRules r = growth_row(site.tier);
  if (site.tier < static_cast<int>(gen::SettlementTier::Capital)) {
    const GrowthRules next = growth_row(site.tier + 1);
    const float p = clampf(site.progress, 0.0f, 1.0f);
    r.fill_p += (next.fill_p - r.fill_p) * p;
    r.plaza_p += (next.plaza_p - r.plaza_p) * p;
  }
  return r;
}

// The tower probability of a block at `dist` from the centre: the row's
// density is for the demo's ~130 m blocks; a site's blocks are B m.
float tower_probability(const GrowthRules& r, double block_m, double dist) {
  const float scale = static_cast<float>((block_m / 130.0) * (block_m / 130.0));
  const float p = std::min(0.95f, r.tower_density * scale);
  if (r.core_m <= 0.0) return 0.0f;
  if (dist < r.core_m) return p;
  if (dist < 1.6 * r.core_m) return p * 0.35f;
  return 0.0f;
}

// Distance from a point to a segment.
double segment_distance(double px, double py, double ax, double ay, double bx, double by) {
  const double vx = bx - ax;
  const double vy = by - ay;
  const double len2 = vx * vx + vy * vy;
  double t = len2 > 0.0 ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0;
  t = std::max(0.0, std::min(1.0, t));
  const double dx = ax + vx * t - px;
  const double dy = ay + vy * t - py;
  return std::sqrt(dx * dx + dy * dy);
}

// Whether an arterial corridor crosses a block (lattice rect centre, half
// diagonal).
bool arterial_crosses(const gen::Site& site, double cx, double cy, double half_diag) {
  for (const gen::Arterial& a : site.arterials) {
    for (std::size_t s = 0; s + 3 < a.xy.size(); s += 2) {
      if (segment_distance(cx, cy, a.xy[s], a.xy[s + 1], a.xy[s + 2], a.xy[s + 3]) < 0.5 * a.width_m + half_diag) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

double civic_centre_radius_m(const gen::Site& site) {
  // Every settlement has its founding site at the centre: the settler
  // couple's plate (stage 0), the hamlet's deck and ring (stage 1), the
  // union square from the town up; a capital's is the largest.
  const int stage = growth_row(site.tier).stage;
  if (stage <= 0) return 24.0;
  if (stage == 1) return 32.0;
  const double r = std::clamp(0.12 * site.radius_m, 55.0, 110.0);
  return site.capital ? 110.0 : r;
}

int capitol_stage(const gen::Site& site) { return growth_row(site.tier).stage; }

void build_site_scene(const gen::SiteField& sites, const gen::Site& site, const gen::TerrainField& field,
                      const SiteBuildParams& params, Scene* scene, SiteBuildStats* stats) {
  Scene& sc = *scene;
  const Architecture& site_arch = architecture_for(site.style);
  if (sc.materials.empty()) sc.materials = site_arch.materials();
  SiteBuildStats st;
  const GrowthRules rules = growth_rules(site);
  const core::Key buildings_key = core::derive_named(site.key, gen::name::BuildingsV1);
  // Ground under a lot: the civil-modified elevation relative to the
  // datum (the plateau target inside 0.72 R, the modified field beyond).
  const auto ground_z = [&](double x, double y) {
    if (x * x + y * y < 0.72 * 0.72 * site.radius_m * site.radius_m) {
      return sites.plateau_m(site, x, y) - site.datum_m;
    }
    return field.elevation_m(site.frame.to_dir(x, y)).to_double() - site.datum_m;
  };
  const Lattice lattice = lattice_of(site);
  double focus_lx = 0.0;
  double focus_ly = 0.0;
  lattice.to_lattice(params.focus_x, params.focus_y, &focus_lx, &focus_ly);
  const double civic_r = civic_centre_radius_m(site);
  // An outpost is the founding scene alone (stage 0): no blocks, no
  // streets, no lots — one couple's house and their landing pod.
  const int reach = rules.stage > 0 ? static_cast<int>(std::ceil(site.radius_m / site.block_m)) + 1 : -1;
  const float inv_radius = site.radius_m > 0.0 ? static_cast<float>(1.0 / site.radius_m) : 1.0f;
  std::vector<gen::Lot> lots;
  LotInput in;
  // The street kit (WP4): per block an asphalt square with the sidewalk
  // plate and curb on it, lane paint and crosswalks on the near levels,
  // lamps at the corners, a plaza in the courtyard of the bigger blocks;
  // arterials with medians; overpasses between plazas.
  int forced_family = 0;
  int tree_budget = rules.trees;
  int lamp_budget = params.lamp_budget > 0 ? std::min(params.lamp_budget * 3, rules.lamps) : rules.lamps;
  int hi_budget = rules.hi_budget;
  int mid_budget = rules.mid_budget;
  const bool streets = site.street_m > 0.0 && site.family != gen::LayoutFamily::Hive &&
                       site.family != gen::LayoutFamily::Crystal && site.family != gen::LayoutFamily::Domed;
  std::vector<Vec2> plaza_centres;
  const double B = site.block_m;
  const double hs = 0.5 * site.street_m;
  const auto lattice_to_scene = [&](double lx, double ly) {
    double wx = lx;
    double wy = ly;
    if (lattice.rotated) {
      wx = lx * lattice.ca - ly * lattice.sa;
      wy = lx * lattice.sa + ly * lattice.ca;
    }
    return to_scene(wx, wy);
  };
  const auto lattice_rect = [&](double x0, double y0, double x1, double y1) {
    std::vector<Vec2> poly{lattice_to_scene(x0, y0), lattice_to_scene(x1, y0), lattice_to_scene(x1, y1), lattice_to_scene(x0, y1)};
    if (plan_area(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
    return poly;
  };
  for (int by = -reach; by <= reach; ++by) {
    for (int bx = -reach; bx <= reach; ++bx) {
      if (params.focus_radius_m > 0.0) {
        const double lcx = (bx + 0.5) * site.block_m - focus_lx;
        const double lcy = (by + 0.5) * site.block_m - focus_ly;
        if (lcx * lcx + lcy * lcy > params.focus_radius_m * params.focus_radius_m) continue;
      }
      lots.clear();
      sites.lots_in_block(site, bx, by, &lots);
      if (lots.empty()) continue;
      const std::uint32_t block_first = static_cast<std::uint32_t>(sc.opaque.indices.size());
      const std::size_t draws_before = sc.draws.size();
      // The block's level by distance from the focus, like its lots.
      int block_detail = params.detail;
      double bwx = (bx + 0.5) * B;
      double bwy = (by + 0.5) * B;
      if (lattice.rotated) {
        const double lx = bwx;
        const double ly = bwy;
        bwx = lx * lattice.ca - ly * lattice.sa;
        bwy = lx * lattice.sa + ly * lattice.ca;
      }
      if (params.focus_radius_m > 0.0) {
        const double fd = std::sqrt((bwx - params.focus_x) * (bwx - params.focus_x) + (bwy - params.focus_y) * (bwy - params.focus_y));
        if (fd > params.context_range_m) block_detail -= 2;
        else if (fd > params.full_range_m) block_detail -= 1;
        block_detail = std::max(0, block_detail);
      }
      if (streets) {
        const double x0 = bx * B;
        const double y0 = by * B;
        const float g = static_cast<float>(ground_z(bwx, bwy));
        const bool civic_block = civic_r > 0.0 && bwx * bwx + bwy * bwy < (civic_r + 0.7 * B) * (civic_r + 0.7 * B);
        Rng gr(core::derive_child(buildings_key, gen::kind::Lot,
                                  0x100000000LL + (static_cast<std::int64_t>(bx) + 4096) * 8192 + (static_cast<std::int64_t>(by) + 4096)));
        // Asphalt under the whole block including its half streets; the
        // sidewalk plate with a curb on top unless an arterial runs
        // through (then the block stands on the road).
        {
          Emit a(&sc.opaque, M_ASPHALT);
          a.polygon(lattice_rect(x0 - hs, y0 - hs, x0 + B + hs, y0 + B + hs), g + 0.03f, true);
        }
        const bool on_arterial = arterial_crosses(site, bwx, bwy, 0.5 * B * 1.4142);
        if (!on_arterial && !civic_block) {
          const std::vector<Vec2> plate = lattice_rect(x0 + hs, y0 + hs, x0 + B - hs, y0 + B - hs);
          Emit s(&sc.opaque, M_SIDEWALK);
          s.polygon(plate, g + kCurb, true);
          Emit c(&sc.opaque, M_CURB);
          c.wall(plate, g, g + kCurb, true, true);
        }
        if (block_detail >= 1) {
          // Lane paint on the streets east and north of the block; the
          // neighbour paints the other two.
          const float road_y = g + 0.05f;
          road_paint(sc, lattice_to_scene(x0 + B, y0 - hs), lattice_to_scene(x0 + B, y0 + B + hs), static_cast<float>(site.street_m), false, road_y);
          road_paint(sc, lattice_to_scene(x0 - hs, y0 + B), lattice_to_scene(x0 + B + hs, y0 + B), static_cast<float>(site.street_m), false, road_y);
          if (block_detail >= 2 && !on_arterial) {
            const Vec2 ex = normalize(lattice_to_scene(x0 + B, y0) - lattice_to_scene(x0, y0));
            const Vec2 ny = normalize(lattice_to_scene(x0, y0 + B) - lattice_to_scene(x0, y0));
            crosswalk(sc, lattice_to_scene(x0 + B, y0 + B - hs - 2.0), ex, static_cast<float>(site.street_m), road_y + 0.005f);
            crosswalk(sc, lattice_to_scene(x0 + B - hs - 2.0, y0 + B), ny, static_cast<float>(site.street_m), road_y + 0.005f);
          }
          if (lamp_budget > 0 && !on_arterial) {
            const Vec2 corner = lattice_to_scene(x0 + B - hs - 1.2, y0 + B - hs - 1.2);
            const Vec2 out = normalize(lattice_to_scene(x0 + B, y0 + B) - corner);
            gen_lamp(sc, P3(corner, g + kCurb), std::atan2(out.y, out.x));
            --lamp_budget;
          }
        }
        // Courtyard plaza: the blocks whose lots line the edges leave the
        // interior free (sites/v1 keeps it as a courtyard).
        const double bdist = std::sqrt(bwx * bwx + bwy * bwy);
        const int block_ring = std::clamp(gen::Site::ring_of(bdist), 1, 7);
        const double lot_base = site.lot_m * (0.6 + 0.1 * block_ring);
        const double pitch = lot_base * 1.35;
        const int per_edge = std::max(1, static_cast<int>(B / pitch));
        const double margin = hs + 0.5 * pitch;
        const double inset = margin + 0.63 * lot_base;
        if (per_edge >= 3 && B - 2.0 * inset >= 16.0 && !on_arterial && !civic_block && gr.chance(rules.plaza_p)) {
          const std::vector<Vec2> court = lattice_rect(x0 + inset, y0 + inset, x0 + B - inset, y0 + B - inset);
          const float t = clampf(static_cast<float>(bdist) * inv_radius, 0.0f, 1.0f);
          PlazaKind kind = PlazaKind::Garden;
          const float roll = gr.next();
          if (t < 0.25f) kind = roll < 0.35f ? PlazaKind::Formal : (roll < 0.6f ? PlazaKind::Monument : PlazaKind::Fountain);
          else if (t > 0.5f && t < 0.9f && roll < 0.12f) kind = PlazaKind::Landing;
          else kind = roll < 0.55f ? PlazaKind::Garden : (roll < 0.8f ? PlazaKind::Terraced : PlazaKind::Fountain);
          build_plaza(sc, kind, court, g + kCurb, gr.child(5), block_detail, &tree_budget);
          plaza_centres.push_back(plan_centroid(court));
        }
      }
      Vec3 lo{1e30f, 1e30f, 1e30f};
      Vec3 hi{-1e30f, -1e30f, -1e30f};
      {
        const std::vector<Vec2> square = lattice_rect(bx * B - hs, by * B - hs, bx * B + B + hs, by * B + B + hs);
        const float g = static_cast<float>(ground_z(bwx, bwy));
        for (const Vec2& p : square) {
          lo = vmin(lo, Vec3{p.x, g - 1.0f, p.y});
          hi = vmax(hi, Vec3{p.x, g + 8.0f, p.y});
        }
      }
      // A tower block: inside the core, one tower takes the whole block
      // (its lots stand aside — the skyline is the city layer's).
      if (streets && rules.tower_density > 0.0f) {
        const double x0 = bx * B;
        const double y0 = by * B;
        const double bdist = std::sqrt(bwx * bwx + bwy * bwy);
        const float t = clampf(static_cast<float>(bdist / (1.6 * rules.core_m)), 0.0f, 1.0f);
        const float tower_p = tower_probability(rules, B, bdist);
        const bool on_arterial = arterial_crosses(site, bwx, bwy, 0.5 * B * 1.4142);
        const bool civic_block = civic_r > 0.0 && bwx * bwx + bwy * bwy < (civic_r + 0.7 * B) * (civic_r + 0.7 * B);
        Rng tr(core::derive_child(buildings_key, gen::kind::Lot,
                                  0x400000000LL + (static_cast<std::int64_t>(bx) + 4096) * 8192 + (static_cast<std::int64_t>(by) + 4096)));
        if (!on_arterial && !civic_block && B - 2.0 * hs >= 38.0 && tr.chance(tower_p)) {
          TowerBlockInput in_t;
          in_t.block.footprint = lattice_rect(x0 + hs, y0 + hs, x0 + B - hs, y0 + B - hs);
          in_t.block.centre = plan_centroid(in_t.block.footprint);
          const Vec2 e = lattice_to_scene(x0 + B, y0) - lattice_to_scene(x0, y0);
          in_t.block.rotation = std::atan2(e.y, e.x);
          in_t.block.ground_y = static_cast<float>(ground_z(bwx, bwy)) + kCurb;
          in_t.block.t = t;
          in_t.block.style = site.style;
          in_t.block.usage = gen::LotUsage::Civic;
          in_t.max_floors = std::max(10, static_cast<int>(static_cast<float>(rules.max_floors) * (1.0f - 0.6f * t)));
          in_t.heroes = rules.heroes;
          in_t.groups = rules.groups;
          if (rules.heroes && site.tier >= static_cast<int>(gen::SettlementTier::Metropolis) && forced_family < 6 && t < 0.5f) {
            in_t.forced_family = forced_family++;
          }
          // Geometry budget (T0022 A.3): full detail exists only for the
          // core towers and at most hi_budget of them, the near-context
          // level out to the tower zone for mid_budget more, the rest is
          // generated from the far-context level up. Levels a tower lacks
          // are the next coarser one.
          in_t.finest_level = (t < 0.28f && hi_budget > 0) ? 0 : ((t < 0.7f && mid_budget > 0) ? 1 : 2);
          if (in_t.finest_level == 0) --hi_budget;
          if (in_t.finest_level == 1) --mid_budget;
          const std::uint32_t tower_first = static_cast<std::uint32_t>(sc.opaque.indices.size());
          site_arch.build_tower_block(sc, in_t, tr.child(1), block_detail);
          const std::uint32_t tris = (static_cast<std::uint32_t>(sc.opaque.indices.size()) - tower_first) / 3;
          if (tris > 0) {
            ++st.towers;
            st.max_tower_triangles = std::max(st.max_tower_triangles, tris);
            hi = vmax(hi, Vec3{hi.x, in_t.block.ground_y + static_cast<float>(in_t.max_floors) * 4.5f + 20.0f, hi.z});
            lots.clear();  // the block is the tower's
          }
        }
      }
      for (const gen::Lot& lot : lots) {
        double cx = 0.0;
        double cy = 0.0;
        for (int k = 0; k < lot.vertex_count; ++k) {
          cx += lot.footprint[k][0];
          cy += lot.footprint[k][1];
        }
        cx /= lot.vertex_count;
        cy /= lot.vertex_count;
        if (civic_r > 0.0 && cx * cx + cy * cy < civic_r * civic_r) continue;  // the civic centre's ground
        in.id = lot.id;
        in.footprint.clear();
        for (int k = 0; k < lot.vertex_count; ++k) {
          in.footprint.push_back(to_scene(lot.footprint[k][0], lot.footprint[k][1]));
        }
        if (plan_area(in.footprint) < 0.0f) std::reverse(in.footprint.begin(), in.footprint.end());
        in.centre = to_scene(cx, cy);
        const Vec2 e = in.footprint[1] - in.footprint[0];
        in.rotation = std::atan2(e.y, e.x);
        const double gz = ground_z(cx, cy);
        in.ground_y = static_cast<float>(gz);
        in.height_budget = lot.height_budget_m;
        in.t = clampf(static_cast<float>(std::sqrt(cx * cx + cy * cy)) * inv_radius, 0.0f, 1.0f);
        in.tier = lot.tier;
        in.usage = lot.usage;
        in.style = lot.style;
        const Architecture& arch = architecture_for(lot.style);
        int detail = params.detail;
        if (params.focus_radius_m > 0.0) {
          const double fdx = cx - params.focus_x;
          const double fdy = cy - params.focus_y;
          const double fd = std::sqrt(fdx * fdx + fdy * fdy);
          if (fd > params.context_range_m) detail -= 2;
          else if (fd > params.full_range_m) detail -= 1;
          detail = std::max(0, detail);
        }
        const core::Key lot_key = core::derive_child(buildings_key, gen::kind::Lot, static_cast<std::int64_t>(lot.id));
        // An unbuilt lot (T0022 A.3): a lawn with a tree now and then — the
        // voids that make a hamlet read as sparse. The roll is keyed per
        // lot and compared against a probability that only rises with the
        // tier and the progress, so a lot that was built stays built.
        if (rules.fill_p < 1.0f && lot.usage != gen::LotUsage::Pad && lot.usage != gen::LotUsage::Monument) {
          Rng fr = Rng(lot_key).child(0xF1);
          if (fr.next() >= rules.fill_p) {
            Emit lawn(&sc.opaque, M_GRASS);
            lawn.polygon(plan_offset(in.footprint, -1.0f), in.ground_y + 0.02f, true);
            if (fr.chance(0.4f) && tree_budget > 0) {
              gen_tree(sc, fr.child(77), P3(in.centre, in.ground_y), fr.range(6.0f, 9.0f));
              --tree_budget;
            }
            ++st.lawns;
            for (const Vec2& p : in.footprint) {
              lo = vmin(lo, Vec3{p.x, in.ground_y, p.y});
              hi = vmax(hi, Vec3{p.x, in.ground_y + 10.0f, p.y});
            }
            continue;
          }
        }
        // The outer districts of the big classes get the cheapest standards.
        if (in.t > rules.cheap_beyond && lot.height_budget_m < 34.0f) detail = 0;
        const std::uint32_t lot_first = static_cast<std::uint32_t>(sc.opaque.indices.size());
        const std::size_t lot_first_vertex = sc.opaque.vertices.size();
        const std::size_t foliage_first = sc.foliage.vertices.size();
        const std::size_t foliage_first_index = sc.foliage.indices.size();
        const std::size_t lights_first = sc.lights.size();
        const LotBuildResult r = arch.build_lot(sc, in, Rng(lot_key), detail);
        if (!r.built) continue;
        // A lot whose geometry is not finite is dropped whole: one NaN
        // vertex is a screen-filling triangle on the GPU.
        bool finite = true;
        for (std::size_t v = lot_first_vertex; v < sc.opaque.vertices.size() && finite; ++v) {
          const Vertex& vx = sc.opaque.vertices[v];
          finite = std::isfinite(vx.position.x + vx.position.y + vx.position.z + vx.normal.x + vx.normal.y +
                                 vx.normal.z + vx.tangent.x + vx.tangent.y + vx.tangent.z + vx.uv.x + vx.uv.y);
        }
        if (!finite) {
          sc.opaque.vertices.resize(lot_first_vertex);
          sc.opaque.indices.resize(lot_first);
          sc.foliage.vertices.resize(foliage_first);
          sc.foliage.indices.resize(foliage_first_index);
          sc.lights.resize(lights_first);
          if (st.dropped == 0) {
            std::fprintf(stderr, "city: site %u lot %u (%s, %d vertices, budget %.1f m) built non-finite geometry; dropped\n",
                         site.province, lot.id, gen::to_string(lot.usage), lot.vertex_count, lot.height_budget_m);
          }
          ++st.dropped;
          continue;
        }
        const std::uint32_t tris = (static_cast<std::uint32_t>(sc.opaque.indices.size()) - lot_first) / 3;
        ++st.lots;
        if (!r.tower) {
          // One culling range per building (T0022 B.1): tight enough for
          // occlusion culling to bite; the block stays the coarse range.
          Vec2 flo, fhi;
          plan_bounds(in.footprint, &flo, &fhi);
          const float h = lot.height_budget_m + 3.0f;
          const Vec2 fc = (flo + fhi) * 0.5f;
          const float rad = std::sqrt(0.25f * ((fhi.x - flo.x) * (fhi.x - flo.x) + (fhi.y - flo.y) * (fhi.y - flo.y)) + 0.25f * h * h) + 3.0f;
          sc.register_fine(lot_first, static_cast<std::uint32_t>(sc.opaque.indices.size()), Vec3{fc.x, in.ground_y + h * 0.5f, fc.y}, rad);
        }
        if (r.tower) {
          ++st.towers;
          st.max_tower_triangles = std::max(st.max_tower_triangles, tris);
        } else if (lot.usage == gen::LotUsage::Residential || lot.usage == gen::LotUsage::Civic ||
                   lot.usage == gen::LotUsage::Industrial) {
          ++st.standards;
          st.max_standard_triangles = std::max(st.max_standard_triangles, tris);
        }
        for (const Vec2& p : in.footprint) {
          lo = vmin(lo, Vec3{p.x, in.ground_y, p.y});
          hi = vmax(hi, Vec3{p.x, in.ground_y + lot.height_budget_m, p.y});
        }
      }
      const std::uint32_t block_end = static_cast<std::uint32_t>(sc.opaque.indices.size());
      if (block_end > block_first) {
        // One range per block over everything the lots did not register
        // themselves (tower level groups), so nothing is drawn twice.
        const Vec3 centre = (lo + hi) * 0.5f;
        const float radius = length(hi - lo) * 0.5f + 1.0f;
        std::uint32_t cursor = block_first;
        for (std::size_t d = draws_before; d < sc.draws.size(); ++d) {
          const DrawRange& r = sc.draws[d];
          if (r.first > cursor) sc.register_range(cursor, r.first, centre, radius, -1, 0, 1e30f, true);
          cursor = std::max(cursor, r.first + r.count);
        }
        if (cursor < block_end) sc.register_range(cursor, block_end, centre, radius, -1, 0, 1e30f, true);
      }
    }
  }
  // Arterials: asphalt, lane paint, medians with hedges and trees.
  {
    Rng ar(core::derive_child(buildings_key, gen::kind::Lot, 0x200000000LL));
    int seg_index = 0;
    for (const gen::Arterial& a : site.arterials) {
      for (std::size_t s = 0; s + 3 < a.xy.size(); s += 2, ++seg_index) {
        double ax = a.xy[s];
        double ay = a.xy[s + 1];
        double bxp = a.xy[s + 2];
        double byp = a.xy[s + 3];
        if (params.focus_radius_m > 0.0) {
          // Clip the segment to the focus disc (plus a block of margin).
          const double rr = params.focus_radius_m + 0.7 * B;
          const double dx = bxp - ax;
          const double dy = byp - ay;
          const double fx0 = ax - params.focus_x;
          const double fy0 = ay - params.focus_y;
          const double qa = dx * dx + dy * dy;
          const double qb = 2.0 * (fx0 * dx + fy0 * dy);
          const double qc = fx0 * fx0 + fy0 * fy0 - rr * rr;
          const double disc = qb * qb - 4.0 * qa * qc;
          if (qa <= 0.0 || disc < 0.0) continue;
          const double sq = std::sqrt(disc);
          const double t0 = std::max(0.0, (-qb - sq) / (2.0 * qa));
          const double t1 = std::min(1.0, (-qb + sq) / (2.0 * qa));
          if (t1 <= t0) continue;
          bxp = ax + dx * t1;
          byp = ay + dy * t1;
          ax += dx * t0;
          ay += dy * t0;
        }
        const double mx = 0.5 * (ax + bxp);
        const double my = 0.5 * (ay + byp);
        if (mx * mx + my * my > site.radius_m * site.radius_m * 1.1) continue;
        const float g = static_cast<float>(ground_z(mx, my));
        const Vec2 pa = to_scene(ax, ay);
        const Vec2 pb = to_scene(bxp, byp);
        road_surface(sc, {pa, pb}, a.width_m, g + 0.04f);
        if (params.detail >= 1) road_paint(sc, pa, pb, a.width_m, true, g + 0.06f);
        if (a.width_m >= 14.0f && params.detail >= 1) {
          // Medians in pieces with gaps for crossings.
          const Vec2 d = normalize(pb - pa);
          const float len = length(pb - pa);
          for (float t0 = 10.0f; t0 + 20.0f < len; t0 += 70.0f) {
            const float t1 = std::min(len - 10.0f, t0 + 56.0f);
            build_median(sc, pa + d * t0, pa + d * t1, 3.2f, kCurb, g + 0.04f);
            if (tree_budget > 0 && ar.chance(0.5f)) {
              gen_tree(sc, ar.child(static_cast<std::uint32_t>(seg_index * 64 + static_cast<int>(t0))), P3(pa + d * (0.5f * (t0 + t1)), g + 0.04f + kCurb), ar.range(6.0f, 8.0f));
              --tree_budget;
            }
          }
        }
      }
    }
    // Overpasses between plazas 60-220 m apart.
    int overpasses = 0;
    Rng orr(core::derive_child(buildings_key, gen::kind::Lot, 0x300000000LL));
    for (std::size_t i = 0; i < plaza_centres.size() && overpasses < rules.overpasses; ++i) {
      int best = -1;
      float best_d = 1e9f;
      for (std::size_t j = 0; j < plaza_centres.size(); ++j) {
        if (j == i) continue;
        const float d = length(plaza_centres[j] - plaza_centres[i]);
        if (d > 60.0f && d < 220.0f && d < best_d) {
          best_d = d;
          best = static_cast<int>(j);
        }
      }
      if (best < 0) continue;
      const Vec2 pa = plaza_centres[i];
      const Vec2 pb = plaza_centres[static_cast<std::size_t>(best)];
      const Vec2 d = normalize(pb - pa);
      const Vec2 n{d.y, -d.x};
      const float sway = orr.range(8.0f, 16.0f) * (orr.chance(0.5f) ? 1.0f : -1.0f);
      const std::vector<Vec2> ctrl = {pa + d * 6.0f, pa + d * (best_d * 0.33f) + n * sway, pa + d * (best_d * 0.66f) - n * sway, pb - d * 6.0f};
      const float g = static_cast<float>(ground_z(0.5 * (pa.x + pb.x), -0.5 * (pa.y + pb.y)));
      build_overpass(sc, ctrl, g + kCurb + 6.5f, g + kCurb, orr, 1);
      ++overpasses;
      plaza_centres[static_cast<std::size_t>(best)] = Vec2{1e6f, 1e6f};  // each plaza at most one
    }
    st.plazas = static_cast<std::uint32_t>(plaza_centres.size());
    st.overpasses = static_cast<std::uint32_t>(overpasses);
  }
  // The founding site and the civic centre (T0022 A.1): stage 0 is the
  // settler couple's glass house and the wreck of their landing pod on a
  // small plate; stage 1 adds the deck and a colonnade ring and keeps the
  // pod as a memorial; from stage 2 the union square holds the civic hall
  // facing the pod, and from stage 3 the unification ring stands where
  // the pod lay (the ring replaces the wreck, in its memory). A higher
  // stage only adds to a lower one.
  if (civic_r > 0.0 && (params.focus_radius_m <= 0.0 ||
                        params.focus_x * params.focus_x + params.focus_y * params.focus_y <
                            (params.focus_radius_m + civic_r) * (params.focus_radius_m + civic_r))) {
    const std::uint32_t first = static_cast<std::uint32_t>(sc.opaque.indices.size());
    Rng r(core::derive_child(buildings_key, gen::kind::Lot, 0x0C171C00LL));
    const float y = static_cast<float>(ground_z(0.0, 0.0));
    const float rot = -static_cast<float>(site.axis_rad);  // gen angles turn the other way in the scene frame
    const int stage = rules.stage;
    const Vec2 axis{std::sin(rot), std::cos(rot)};  // the ring lies "south" of the government in the site's frame
    const Vec2 side{axis.y, -axis.x};
    const float cr = static_cast<float>(civic_r);
    if (stage == 0) {
      // One settler couple: the house on a small plate, the pod where the
      // ring will one day stand, a path between them, a tree, a lamp.
      const std::vector<Vec2> plate = plan_transform(plan_rounded_rect(28.0f, 20.0f, 6.0f, 6), Vec2{0, 0}, rot);
      Emit s(&sc.opaque, M_SIDEWALK);
      s.polygon(plate, y + kCurb, true);
      Emit c(&sc.opaque, M_CURB);
      c.wall(plate, y, y + kCurb, true, true);
      const Vec2 house = side * -10.0f - axis * 6.0f;
      const Vec2 pod = side * 12.0f + axis * 6.0f;
      site_arch.build_key(sc, KeyRole::Government, house, rot, 0.0f, y + kCurb, r.child(1), params.detail, stage);
      build_pod_wreck(sc, pod, y + kCurb, r, params.detail, false);
      Emit path(&sc.opaque, M_PLAZA);
      path.polygon(ribbon(spline({side * -6.0f - axis * 2.0f, side * 2.0f + axis * 1.0f, side * 8.0f + axis * 4.0f}, 6), 1.2f), y + kCurb + 0.01f, true);
      gen_tree(sc, r.child(2), P3(side * -22.0f + axis * 10.0f, y + kCurb), 7.0f);
      gen_lamp(sc, P3(axis * -14.0f, y + kCurb), rot + kPi * 0.5f);
      st.key_buildings += 2;
    } else {
      const float half = static_cast<float>(std::min(42.0, civic_r * 0.38));
      const std::vector<Vec2> plaza = plan_transform(plan_rect(cr * 0.92f, cr * 0.92f), Vec2{0, 0}, rot);
      Emit floor(&sc.opaque, stage >= 2 ? M_MARBLE_WHITE : M_CONCRETE_WHITE);
      floor.polygon(plaza, y + 0.02f, true);
      const Vec2 gov = stage >= 2 ? Vec2{0, 0} - axis * (half * 0.55f) : Vec2{0, 0} - axis * 8.0f;
      const Vec2 ring_at = stage >= 2 ? axis * (half * 1.7f) : axis * 14.0f;
      site_arch.build_key(sc, KeyRole::Government, gov, rot, half, y, r.child(1), params.detail, stage);
      if (stage >= 3) {
        site_arch.build_key(sc, KeyRole::UnificationRing, ring_at, rot + kPi * 0.5f, std::min(14.0f, half * 0.35f), y, r.child(2), params.detail);
      } else {
        build_pod_wreck(sc, ring_at, y, r, params.detail, true);  // the founders' pod, kept where the ring will stand
      }
      build_hedge_ring(sc, plaza, 3.0f, 0.9f, 0.9f, y, 16.0f, r);
      if (stage >= 2) {
        // Union square: fountains either side of the axis between the
        // two, round basins in the corners, benches at the fountains.
        const Vec2 mid = axis * (half * 0.55f);
        for (const float sgn : {-1.0f, 1.0f}) {
          const Vec2 fc = mid + side * (sgn * cr * 0.5f);
          build_fountain(sc, fc, std::min(12.0f, cr * 0.11f), y, r, params.detail);
          for (int k = 0; k < 6; ++k) {
            const float a = static_cast<float>(k) / 6.0f * 2.0f * kPi + 0.4f;
            gen_bench(sc, P3(fc + Vec2{std::cos(a), std::sin(a)} * std::min(18.0f, cr * 0.17f), y), a + kPi * 0.5f);
          }
        }
        for (const float sx : {-1.0f, 1.0f}) {
          for (const float sz : {-1.0f, 1.0f}) {
            const Vec2 bc = axis * (sz * cr * 0.7f) + side * (sx * cr * 0.7f);
            build_basin(sc, bc, std::min(9.0f, cr * 0.09f), std::min(9.0f, cr * 0.09f), true, y, 0.4f);
            gen_planter(sc, r.child(static_cast<std::uint32_t>(60 + static_cast<int>(sx + 1.0f) * 2 + static_cast<int>(sz + 1.0f))),
                        bc + side * (sx * cr * 0.12f), 4.0f, 1.5f, y);
          }
        }
      }
      if (params.detail >= 1) {
        // The square's trees are its own (not the site's budget): a ring
        // inside the hedge and an inner ring around the fountains.
        const int n_trees = cr > 80.0f ? 24 : (cr > 40.0f ? 16 : 8);
        for (int k = 0; k < n_trees; ++k) {
          const float a = static_cast<float>(k) / static_cast<float>(n_trees) * 2.0f * kPi + 0.2f;
          const Vec2 p{std::cos(a) * cr * 0.84f, std::sin(a) * cr * 0.84f};
          gen_tree(sc, r.child(static_cast<std::uint32_t>(40 + k)), P3(p, y + 0.02f), r.range(7.0f, 11.0f));
        }
        if (cr > 80.0f) {
          for (int k = 0; k < 12; ++k) {
            const float a = static_cast<float>(k) / 12.0f * 2.0f * kPi;
            const Vec2 p{std::cos(a) * cr * 0.55f, std::sin(a) * cr * 0.55f};
            if (std::fabs(dot(normalize(p), axis)) > 0.8f) continue;  // keep the axis between the two open
            gen_tree(sc, r.child(static_cast<std::uint32_t>(80 + k)), P3(p, y + 0.02f), r.range(6.0f, 9.0f));
          }
        }
      }
      const int lamps = std::min(params.lamp_budget, stage >= 2 ? 12 : 4);
      for (int k = 0; k < lamps; ++k) {
        const float a = static_cast<float>(k) / static_cast<float>(lamps) * 2.0f * kPi;
        const Vec2 p{std::cos(a) * cr * 0.62f, std::sin(a) * cr * 0.62f};
        gen_lamp(sc, P3(p, y), a + kPi);
      }
      st.key_buildings += 2;
    }
    const std::uint32_t end = static_cast<std::uint32_t>(sc.opaque.indices.size());
    sc.register_range(first, end, Vec3{0.0f, y + 20.0f, 0.0f}, static_cast<float>(civic_r) * 1.5f + 40.0f);
  }
  sc.finalize_draws();
  st.triangles = static_cast<std::uint32_t>((sc.opaque.indices.size() + sc.foliage.indices.size()) / 3);
  sc.city_radius = static_cast<float>(site.radius_m);
  sc.stats_towers = static_cast<int>(st.towers);
  sc.stats_standards = static_cast<int>(st.standards);
  if (stats != nullptr) *stats = st;
}

}  // namespace inf::city
