#include "gen/site_mesh.hpp"

#include <algorithm>
#include <cmath>

#include "gen/material.hpp"
#include "gen/names.hpp"

namespace inf::gen {

namespace {

// Vertex writer: position (site-local metres, planet-local later via the
// origin), normal, palette weights (one-hot over the site palette).
struct Writer {
  std::vector<float>* out;
  void vertex(double x, double y, double z, double nx, double ny, double nz, int material_slot) {
    out->push_back(static_cast<float>(x));
    out->push_back(static_cast<float>(y));
    out->push_back(static_cast<float>(z));
    out->push_back(static_cast<float>(nx));
    out->push_back(static_cast<float>(ny));
    out->push_back(static_cast<float>(nz));
    for (int k = 0; k < 4; ++k) {
      out->push_back(k == material_slot ? 1.0f : 0.0f);
    }
  }
};

}  // namespace

void site_palette(const Site& site, int detail, std::uint8_t out[4]) {
  building_palette(site.style, out);
  if (detail == 1 && !site.ruined && site.style.material_family <= 1 &&
      site.style.faction_type != FactionType::Outlaw) {
    // Mid LOD: the triplanar tiling paints a window grid on every wall
    // in world metres — windows for free where geometric bays are too
    // expensive.
    out[0] = static_cast<std::uint8_t>(Material::FacadeWindows);
  }
}

SiteMesh build_site_mesh(const SiteField& sites, const Site& site, const TerrainField& field,
                         const SiteMeshParams& params) {
  SiteMesh out;
  site_palette(site, params.detail, out.mesh.palette);
  const double R = site.frame.radius_m;
  // Origin: the centre on the plateau.
  const Dir3 up = site.frame.up;
  const double origin_r = R + site.datum_m;
  out.mesh.origin[0] = up.x.to_double() * origin_r;
  out.mesh.origin[1] = up.y.to_double() * origin_r;
  out.mesh.origin[2] = up.z.to_double() * origin_r;
  const double ex[3] = {site.frame.east.x.to_double(), site.frame.east.y.to_double(), site.frame.east.z.to_double()};
  const double ny[3] = {site.frame.north.x.to_double(), site.frame.north.y.to_double(), site.frame.north.z.to_double()};
  const double uz[3] = {up.x.to_double(), up.y.to_double(), up.z.to_double()};
  Writer w{&out.mesh.vertices};
  // A site-local point (x east, y north, z up from the plateau datum)
  // to planet-local metres relative to the origin. The sphere's
  // curvature is honoured through the frame's to_dir (exact), so a 12 km
  // capital does not float at its rim.
  const auto place = [&](double x, double y, double z, double* px, double* py, double* pz) {
    const Dir3 d = site.frame.to_dir(x, y);
    const double r = R + site.datum_m + z;
    *px = d.x.to_double() * r - out.mesh.origin[0];
    *py = d.y.to_double() * r - out.mesh.origin[1];
    *pz = d.z.to_double() * r - out.mesh.origin[2];
  };
  const auto normal = [&](double lx, double ly, double lz, double* nx, double* nyy, double* nz) {
    *nx = ex[0] * lx + ny[0] * ly + uz[0] * lz;
    *nyy = ex[1] * lx + ny[1] * ly + uz[1] * lz;
    *nz = ex[2] * lx + ny[2] * ly + uz[2] * lz;
  };
  // Ground under a lot: the civil-modified elevation at its centre,
  // relative to the datum (the plateau makes this ~0 inside 0.75 R).
  const auto ground_z = [&](double x, double y) {
    return field.elevation_m(site.frame.to_dir(x, y)).to_double() - site.datum_m;
  };
  const int reach = static_cast<int>(std::ceil(site.radius_m / site.block_m)) + 1;
  std::vector<Lot> lots;
  for (int by = -reach; by <= reach; ++by) {
    for (int bx = -reach; bx <= reach; ++bx) {
      if (params.focus_radius_m > 0.0) {
        const double cx = (bx + 0.5) * site.block_m - params.focus_x;
        const double cy = (by + 0.5) * site.block_m - params.focus_y;
        if (cx * cx + cy * cy > params.focus_radius_m * params.focus_radius_m) {
          continue;
        }
      }
      lots.clear();
      sites.lots_in_block(site, bx, by, &lots);
      if (lots.empty()) {
        continue;
      }
      if (params.detail >= 2) {
        // One mass per block: the block's bounding box at its tallest lot.
        double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300, top = 0.0;
        for (const Lot& lot : lots) {
          for (int k = 0; k < lot.vertex_count; ++k) {
            minx = std::min(minx, static_cast<double>(lot.footprint[k][0]));
            maxx = std::max(maxx, static_cast<double>(lot.footprint[k][0]));
            miny = std::min(miny, static_cast<double>(lot.footprint[k][1]));
            maxy = std::max(maxy, static_cast<double>(lot.footprint[k][1]));
          }
          top = std::max(top, static_cast<double>(lot.height_budget_m) * lot.style.construction);
        }
        Lot block;
        block.vertex_count = 4;
        block.footprint[0][0] = static_cast<float>(minx); block.footprint[0][1] = static_cast<float>(miny);
        block.footprint[1][0] = static_cast<float>(maxx); block.footprint[1][1] = static_cast<float>(miny);
        block.footprint[2][0] = static_cast<float>(maxx); block.footprint[2][1] = static_cast<float>(maxy);
        block.footprint[3][0] = static_cast<float>(minx); block.footprint[3][1] = static_cast<float>(maxy);
        block.height_budget_m = static_cast<float>(top * 0.8);
        block.style = lots[0].style;
        block.style.construction = 1.0f;
        block.datum_m = lots[0].datum_m;
        lots.assign(1, block);
      }
      double mean_h = 0.0;
      for (const Lot& lot : lots) mean_h += lot.height_budget_m;
      mean_h /= static_cast<double>(lots.size());
      for (const Lot& lot : lots) {
        const double h_full = static_cast<double>(lot.height_budget_m);
        if (params.detail == 1 && h_full < 1.5 * mean_h && site.tier >= 5) {
          continue;  // big sites at mid range: the tall lots only
        }
        double cx = 0.0;
        double cy = 0.0;
        for (int k = 0; k < lot.vertex_count; ++k) {
          cx += lot.footprint[k][0];
          cy += lot.footprint[k][1];
        }
        cx /= lot.vertex_count;
        cy /= lot.vertex_count;
        // buildings/v1: the executor in the lot's frame (ground = the
        // civil-modified surface under the lot centre).
        BuildingParams bp;
        bp.method = params.detail >= 2 ? BuildingMethod::Mass
                    : (params.detail == 1 ? BuildingMethod::Grammar : params.method);
        bp.ground_z = ground_z(cx, cy);
        const core::Key lot_key = core::derive_child(
            core::derive_named(site.key, name::BuildingsV1), kind::Lot, static_cast<std::int64_t>(lot.id));
        const BuildingMesh building = build_building(lot, lot_key, bp);
        // Site-local -> planet-local relative to the origin.
        const std::size_t count = building.vertices.size() / 7;
        for (std::size_t v = 0; v < count; ++v) {
          const float* in = building.vertices.data() + v * 7;
          double px, py, pz;
          place(in[0], in[1], in[2], &px, &py, &pz);
          double nx, nyv, nz;
          normal(in[3], in[4], in[5], &nx, &nyv, &nz);
          w.vertex(px, py, pz, nx, nyv, nz, static_cast<int>(in[6]));
        }
        out.triangle_count += building.triangle_count;
        ++out.lot_count;
      }
    }
  }
  // Winding: the terrain pipeline culls nothing (double-sided lighting
  // by normal), so orientation only matters for the normal we wrote.
  return out;
}

}  // namespace inf::gen
