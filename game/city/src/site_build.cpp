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
