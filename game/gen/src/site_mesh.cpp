#include "gen/site_mesh.hpp"

#include <algorithm>
#include <cmath>

#include "core/det/mix.hpp"
#include "gen/material.hpp"
#include "gen/names.hpp"

namespace inf::gen {

namespace {

// Nearest-LOD terminal ranges from the focus (site-local metres).
constexpr double kPartsRangeM = 350.0;
constexpr double kGrammarRangeM = 900.0;

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
  // Inside the plateau (0.75 R) the civil modifier returns the plateau
  // target exactly, so no terrain read is needed there; the rim blend
  // beyond it reads the modified field (a few hundred lots).
  const auto ground_z = [&](double x, double y) {
    if (x * x + y * y < 0.72 * 0.72 * site.radius_m * site.radius_m) {
      return sites.plateau_m(site, x, y) - site.datum_m;
    }
    return field.elevation_m(site.frame.to_dir(x, y)).to_double() - site.datum_m;
  };
  const int reach = static_cast<int>(std::ceil(site.radius_m / site.block_m)) + 1;
  // Superblock masses (the far LOD, and the base of the mid LOD on big
  // sites): k x k blocks merged into one box whose height is the ring's
  // typical lot height — no lot generation at all, so a 12 km capital
  // costs a few thousand boxes instead of a hundred thousand blocks.
  const double blocks_in_site = 3.14159265 * (site.radius_m / site.block_m) * (site.radius_m / site.block_m);
  int merge = 1;
  while (merge < 8 && blocks_in_site / static_cast<double>(merge * merge) > 6000.0) ++merge;
  const int merge_mid = std::max(1, merge / 2);
  const auto emit_building = [&](const BuildingMesh& building) {
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
  };
  const auto superblocks = [&](int k, bool landmarks_only) {
    const int sreach = (reach + k - 1) / k + 1;
    for (int sy = -sreach; sy <= sreach; ++sy) {
      for (int sx = -sreach; sx <= sreach; ++sx) {
        const double x0 = sx * k * site.block_m;
        const double y0 = sy * k * site.block_m;
        const double cx = x0 + 0.5 * k * site.block_m;
        const double cy = y0 + 0.5 * k * site.block_m;
        const double dist = std::sqrt(cx * cx + cy * cy);
        if (dist > site.radius_m * 0.97) continue;
        if (params.focus_radius_m > 0.0) {
          const double fx = cx - params.focus_x;
          const double fy = cy - params.focus_y;
          if (fx * fx + fy * fy <= params.focus_radius_m * params.focus_radius_m) continue;  // the focus builds its own
        }
        // Typical lot height of the ring that created this ground.
        const int ring = std::min(7, Site::ring_of(dist));
        const double r_out = ring_radius_m(ring);
        const double rel = r_out > 0.0 ? dist / r_out : 0.0;
        double height = 4.0 + 2.5 * ring;
        if (ring >= 5) height += (1.0 - rel) * (1.0 - rel) * 60.0 * (ring - 4) * 0.6;
        if (site.domed) height = std::min(height, 0.5 * site.lot_m);
        // Keyed variation so the far view is a skyline, not a waffle: most
        // superblocks scatter around the ring height, one in twelve is a
        // landmark standing for the tall lots the near LOD will show.
        const std::uint64_t h = det::mix64((static_cast<std::uint64_t>(static_cast<std::uint32_t>(sx)) << 32U) ^
                                           static_cast<std::uint32_t>(sy) ^ site.key.k0);
        const double u = static_cast<double>(h >> 11U) * 0x1.0p-53;
        height *= 0.55 + 0.9 * u;
        const bool landmark = (h & 15U) == 0U;
        if (landmark) height *= 2.2;
        if (landmarks_only && !landmark) continue;  // the far view: the ground is the bake, the skyline the landmarks
        height *= site.ruined ? 0.4 : 0.8;
        Lot block;
        block.vertex_count = 4;
        // Merged superblocks stand for whole neighbourhoods: no street
        // gap between them (the gaps read as a waffle from 20 km).
        const double inset = k > 1 ? 0.0 : 0.5 * site.street_m;
        block.footprint[0][0] = static_cast<float>(x0 + inset); block.footprint[0][1] = static_cast<float>(y0 + inset);
        block.footprint[1][0] = static_cast<float>(x0 + k * site.block_m - inset); block.footprint[1][1] = static_cast<float>(y0 + inset);
        block.footprint[2][0] = static_cast<float>(x0 + k * site.block_m - inset); block.footprint[2][1] = static_cast<float>(y0 + k * site.block_m - inset);
        block.footprint[3][0] = static_cast<float>(x0 + inset); block.footprint[3][1] = static_cast<float>(y0 + k * site.block_m - inset);
        block.height_budget_m = static_cast<float>(height);
        block.style = site.style;
        block.style.construction = 1.0f;
        BuildingParams bp;
        bp.method = BuildingMethod::Mass;
        bp.ground_z = sites.plateau_m(site, cx, cy) - site.datum_m;
        const core::Key key = core::derive_child(core::derive_named(site.key, name::BuildingsV1), kind::Lot,
                                                 0x53000000LL + sx * 4096LL + sy);
        emit_building(build_building(block, key, bp));
      }
    }
  };
  if (params.detail >= 2) {
    superblocks(std::max(1, merge), true);
    return out;
  }
  const bool big = site.tier >= 6;  // Capital/Metropolis: block masses outside the focus; a City gets every lot as a mass
  if (params.detail == 1 && big) {
    superblocks(merge_mid, false);  // outside the focus (or everywhere without one)
  } else if (params.detail == 0 && params.focus_radius_m > 0.0) {
    superblocks(merge, false);  // the far ground beyond the focus
  }
  std::vector<Lot> lots;
  for (int by = -reach; by <= reach; ++by) {
    for (int bx = -reach; bx <= reach; ++bx) {
      if (params.focus_radius_m > 0.0) {
        const double cx = (bx + 0.5) * site.block_m - params.focus_x;
        const double cy = (by + 0.5) * site.block_m - params.focus_y;
        if (cx * cx + cy * cy > params.focus_radius_m * params.focus_radius_m) {
          continue;
        }
      } else if (params.detail == 1 && big) {
        continue;  // no focus: the superblocks carry the whole site
      }
      lots.clear();
      sites.lots_in_block(site, bx, by, &lots);
      if (lots.empty()) {
        continue;
      }
      double mean_h = 0.0;
      for (const Lot& lot : lots) mean_h += lot.height_budget_m;
      mean_h /= static_cast<double>(lots.size());
      // Mid LOD inside the focus of a big site: the block mass plus the
      // lots that rise above it.
      if (params.detail == 1 && big) {
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
        block.height_budget_m = static_cast<float>(std::min(top, mean_h * 1.2));
        block.style = lots[0].style;
        block.style.construction = 1.0f;
        block.datum_m = lots[0].datum_m;
        block.id = 0xB10C0000u;
        lots.push_back(block);
      }
      for (const Lot& lot : lots) {
        const double h_full = static_cast<double>(lot.height_budget_m);
        const bool is_block = lot.id == 0xB10C0000u;
        if (params.detail == 1 && big && !is_block && h_full < 1.5 * mean_h) {
          continue;  // the block mass carries the rest
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
        // civil-modified surface under the lot centre). The terminal set
        // is chosen per lot by distance from the focus: parts nearest,
        // grammar in the middle distance, masses beyond — a 3 km city
        // stays under a million triangles at the nearest LOD.
        BuildingParams bp;
        if (is_block) {
          bp.method = BuildingMethod::Mass;
        } else if (params.detail == 1) {
          bp.method = (big || h_full >= 1.5 * mean_h) ? BuildingMethod::Grammar : BuildingMethod::Mass;
        } else {
          const double fdx = cx - params.focus_x;
          const double fdy = cy - params.focus_y;
          const double fd = std::sqrt(fdx * fdx + fdy * fdy);
          bp.method = fd < kPartsRangeM ? params.method
                      : (fd < kGrammarRangeM ? BuildingMethod::Grammar : BuildingMethod::Mass);
          if (bp.method == BuildingMethod::GrammarParts && params.focus_radius_m <= 0.0 && site.radius_m > kGrammarRangeM) {
            bp.method = BuildingMethod::Grammar;  // no focus given on a big site: never all parts
          }
        }
        bp.ground_z = ground_z(cx, cy);
        const core::Key lot_key = core::derive_child(
            core::derive_named(site.key, name::BuildingsV1), kind::Lot, static_cast<std::int64_t>(lot.id));
        emit_building(build_building(lot, lot_key, bp));
      }
    }
  }
  // Winding: the terrain pipeline culls nothing (double-sided lighting
  // by normal), so orientation only matters for the normal we wrote.
  return out;
}

}  // namespace inf::gen
