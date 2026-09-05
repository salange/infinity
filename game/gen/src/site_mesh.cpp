#include "gen/site_mesh.hpp"

#include <algorithm>
#include <cmath>

#include "gen/material.hpp"

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

void site_palette(const Site& site, std::uint8_t out[4]) {
  // Walls / roof / base / accent by material family.
  switch (site.style.material_family) {
    case 1:  // metal and glass
      out[0] = static_cast<std::uint8_t>(Material::Plating);
      out[1] = static_cast<std::uint8_t>(Material::RockShale);
      out[2] = static_cast<std::uint8_t>(Material::Paving);
      out[3] = static_cast<std::uint8_t>(Material::Plating);
      break;
    case 2:  // resin (hives)
      out[0] = static_cast<std::uint8_t>(Material::ResinFloor);
      out[1] = static_cast<std::uint8_t>(Material::SoilDry);
      out[2] = static_cast<std::uint8_t>(Material::ResinFloor);
      out[3] = static_cast<std::uint8_t>(Material::RockSandstone);
      break;
    case 3:  // crystal
      out[0] = static_cast<std::uint8_t>(Material::CrystalField);
      out[1] = static_cast<std::uint8_t>(Material::CrystalFloor);
      out[2] = static_cast<std::uint8_t>(Material::CrystalFloor);
      out[3] = static_cast<std::uint8_t>(Material::IceSheet);
      break;
    case 4:  // grown
      out[0] = static_cast<std::uint8_t>(Material::Moss);
      out[1] = static_cast<std::uint8_t>(Material::LichenCrust);
      out[2] = static_cast<std::uint8_t>(Material::SoilLoam);
      out[3] = static_cast<std::uint8_t>(Material::ForestFloor);
      break;
    default:  // stone
      out[0] = static_cast<std::uint8_t>(Material::RockSandstone);
      out[1] = static_cast<std::uint8_t>(Material::RockShale);
      out[2] = static_cast<std::uint8_t>(Material::Paving);
      out[3] = static_cast<std::uint8_t>(Material::RockGranite);
      break;
  }
  if (site.ruined) {
    out[0] = static_cast<std::uint8_t>(Material::RockShale);
    out[1] = static_cast<std::uint8_t>(Material::Scree);
  }
}

SiteMesh build_site_mesh(const SiteField& sites, const Site& site, const TerrainField& field,
                         const SiteMeshParams& params) {
  SiteMesh out;
  site_palette(site, out.mesh.palette);
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
        if (params.detail == 1 && h_full < 1.5 * mean_h) {
          continue;
        }
        // Construction: a foundation slab, then the frame rising.
        const double stage = lot.style.construction;
        double h = h_full * (stage < 0.3 ? 0.08 : (stage < 1.0 ? 0.08 + (stage - 0.3) / 0.7 * 0.92 : 1.0));
        if (site.ruined) {
          h = h_full * 0.35;  // collapsed to a stub
        }
        if (h < 0.5) h = 0.5;
        double cx = 0.0;
        double cy = 0.0;
        for (int k = 0; k < lot.vertex_count; ++k) {
          cx += lot.footprint[k][0];
          cy += lot.footprint[k][1];
        }
        cx /= lot.vertex_count;
        cy /= lot.vertex_count;
        const double base = ground_z(cx, cy) - 0.8;  // sunk into the ground
        const double top = base + 0.8 + h;
        const int wall_mat = 0;
        const int roof_mat = 1;
        const int accent_mat = lot.usage == LotUsage::Civic || lot.usage == LotUsage::Monument ? 3 : 0;
        const int n = lot.vertex_count;
        // Walls: outward normals from the footprint edges (footprints are
        // counter-clockwise in the local frame).
        for (int k = 0; k < n; ++k) {
          const double ax = lot.footprint[k][0];
          const double ay = lot.footprint[k][1];
          const double bx2 = lot.footprint[(k + 1) % n][0];
          const double by2 = lot.footprint[(k + 1) % n][1];
          double nx = by2 - ay;
          double nyv = -(bx2 - ax);
          const double len = std::sqrt(nx * nx + nyv * nyv);
          if (len < 1e-9) continue;
          nx /= len;
          nyv /= len;
          // Ensure outward: the normal must point away from the centre.
          if ((0.5 * (ax + bx2) - cx) * nx + (0.5 * (ay + by2) - cy) * nyv < 0.0) {
            nx = -nx;
            nyv = -nyv;
          }
          double wn[3];
          normal(nx, nyv, 0.0, &wn[0], &wn[1], &wn[2]);
          double p0[3], p1[3], p2[3], p3[3];
          place(ax, ay, base, &p0[0], &p0[1], &p0[2]);
          place(bx2, by2, base, &p1[0], &p1[1], &p1[2]);
          place(bx2, by2, top, &p2[0], &p2[1], &p2[2]);
          place(ax, ay, top, &p3[0], &p3[1], &p3[2]);
          const int m = accent_mat != 0 ? accent_mat : wall_mat;
          // Two triangles, winding so the face's outward normal matches.
          w.vertex(p0[0], p0[1], p0[2], wn[0], wn[1], wn[2], m);
          w.vertex(p1[0], p1[1], p1[2], wn[0], wn[1], wn[2], m);
          w.vertex(p2[0], p2[1], p2[2], wn[0], wn[1], wn[2], m);
          w.vertex(p0[0], p0[1], p0[2], wn[0], wn[1], wn[2], m);
          w.vertex(p2[0], p2[1], p2[2], wn[0], wn[1], wn[2], m);
          w.vertex(p3[0], p3[1], p3[2], wn[0], wn[1], wn[2], m);
          out.triangle_count += 2;
        }
        // Roof: a fan from the centroid.
        double rn[3];
        normal(0.0, 0.0, 1.0, &rn[0], &rn[1], &rn[2]);
        double c[3];
        place(cx, cy, top, &c[0], &c[1], &c[2]);
        for (int k = 0; k < n; ++k) {
          double p0[3], p1[3];
          place(lot.footprint[k][0], lot.footprint[k][1], top, &p0[0], &p0[1], &p0[2]);
          place(lot.footprint[(k + 1) % n][0], lot.footprint[(k + 1) % n][1], top, &p1[0], &p1[1], &p1[2]);
          w.vertex(c[0], c[1], c[2], rn[0], rn[1], rn[2], roof_mat);
          w.vertex(p0[0], p0[1], p0[2], rn[0], rn[1], rn[2], roof_mat);
          w.vertex(p1[0], p1[1], p1[2], rn[0], rn[1], rn[2], roof_mat);
          ++out.triangle_count;
        }
        ++out.lot_count;
      }
    }
  }
  // Winding: the terrain pipeline culls nothing (double-sided lighting
  // by normal), so orientation only matters for the normal we wrote.
  return out;
}

}  // namespace inf::gen
