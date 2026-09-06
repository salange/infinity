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

// Prop budgets and plaza odds by the development level's size class
// (L1-2 small, L3 medium, L4-5 large, L6+ metropolis).
struct SizeClass {
  int trees;
  int lamps;
  int overpasses;
  float plaza_p;
};
SizeClass size_class(int level) {
  if (level <= 2) return {40, 60, 1, 0.45f};
  if (level == 3) return {90, 90, 2, 0.5f};
  if (level <= 5) return {140, 120, 4, 0.55f};
  return {200, 160, 6, 0.6f};
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
  if (!site.capital || site.style.level < 5) return 0.0;
  return std::min(110.0, 0.12 * site.radius_m);
}

void build_site_scene(const gen::SiteField& sites, const gen::Site& site, const gen::TerrainField& field,
                      const SiteBuildParams& params, Scene* scene, SiteBuildStats* stats) {
  Scene& sc = *scene;
  const Architecture& site_arch = architecture_for(site.style);
  if (sc.materials.empty()) sc.materials = site_arch.materials();
  SiteBuildStats st;
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
  const int reach = static_cast<int>(std::ceil(site.radius_m / site.block_m)) + 1;
  const float inv_radius = site.radius_m > 0.0 ? static_cast<float>(1.0 / site.radius_m) : 1.0f;
  std::vector<gen::Lot> lots;
  LotInput in;
  // The street kit (WP4): per block an asphalt square with the sidewalk
  // plate and curb on it, lane paint and crosswalks on the near levels,
  // lamps at the corners, a plaza in the courtyard of the bigger blocks;
  // arterials with medians; overpasses between plazas.
  const SizeClass sizes = size_class(site.style.level);
  int tree_budget = sizes.trees;
  int lamp_budget = params.lamp_budget > 0 ? std::min(params.lamp_budget * 3, sizes.lamps) : sizes.lamps;
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
        if (per_edge >= 3 && B - 2.0 * inset >= 16.0 && !on_arterial && !civic_block && gr.chance(sizes.plaza_p)) {
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
        const Vec3 centre = (lo + hi) * 0.5f;
        sc.register_range(block_first, block_end, centre, length(hi - lo) * 0.5f + 1.0f);
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
    for (std::size_t i = 0; i < plaza_centres.size() && overpasses < sizes.overpasses; ++i) {
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
  // The civic centre of a planetary capital: the government building
  // facing the unification ring across a marble plaza, lamps around.
  if (civic_r > 0.0 && (params.focus_radius_m <= 0.0 ||
                        params.focus_x * params.focus_x + params.focus_y * params.focus_y <
                            (params.focus_radius_m + civic_r) * (params.focus_radius_m + civic_r))) {
    const std::uint32_t first = static_cast<std::uint32_t>(sc.opaque.indices.size());
    Rng r(core::derive_child(buildings_key, gen::kind::Lot, 0x0C171C00LL));
    const float y = static_cast<float>(ground_z(0.0, 0.0));
    const float rot = -static_cast<float>(site.axis_rad);  // gen angles turn the other way in the scene frame
    const float half = static_cast<float>(std::min(42.0, civic_r * 0.38));
    const std::vector<Vec2> plaza = plan_transform(plan_rect(static_cast<float>(civic_r) * 0.92f, static_cast<float>(civic_r) * 0.92f), Vec2{0, 0}, rot);
    Emit floor(&sc.opaque, M_MARBLE_WHITE);
    floor.polygon(plaza, y + 0.02f, true);
    const Vec2 axis{std::sin(rot), std::cos(rot)};  // the ring lies "south" of the government in the site's frame
    site_arch.build_key(sc, KeyRole::Government, Vec2{0, 0} - axis * (half * 0.55f), rot, half, y, r.child(1), params.detail);
    site_arch.build_key(sc, KeyRole::UnificationRing, axis * (half * 1.7f), rot + kPi * 0.5f,
                        std::min(14.0f, half * 0.35f), y, r.child(2), params.detail);
    build_hedge_ring(sc, plaza, 3.0f, 0.9f, 0.9f, y, 16.0f, r);
    int lamps = std::min(params.lamp_budget, 12);
    for (int k = 0; k < lamps; ++k) {
      const float a = static_cast<float>(k) / static_cast<float>(lamps) * 2.0f * kPi;
      const Vec2 p{std::cos(a) * static_cast<float>(civic_r) * 0.8f, std::sin(a) * static_cast<float>(civic_r) * 0.8f};
      gen_lamp(sc, P3(p, y), a + kPi);
    }
    st.key_buildings += 2;
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
