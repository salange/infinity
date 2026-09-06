// Headless planetary-system inspection (T0012): params JSON + ephemeris
// table for eyeballing, plus the golden report.

#include "commands_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gen/climate.hpp"
#include "gen/life.hpp"
#include "gen/material.hpp"
#include "gen/system.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "world/chunk_grid.hpp"
#include "world/chunk_manager.hpp"

namespace inf::cli {

int cmd_dump_system(const core::Seed128& seed, std::int64_t start_ns, std::int64_t span_ns,
                    int steps) {
  const auto tree = gen::make_tree(seed);
  const auto node = tree->get(gen::default_system_address());
  const gen::StarSystemParams system = gen::generate_system(node->key());
  std::printf("{\n\"system\": %s,\n\"ephemeris\": %s}\n",
              gen::system_to_json(system).c_str(),
              gen::ephemeris_table_json(system, core::WorldTime::from_ns(start_ns),
                                        steps > 1 ? span_ns / (steps - 1) : span_ns, steps)
                  .c_str());
  return 0;
}

int cmd_hash_system() {
  std::fputs(gen::hash_system_report().c_str(), stdout);
  return 0;
}

// T0019: capture aid — the app's spawn planet (or --slot) with its life
// draw, and a few gentle land spots as planet-local `pos` script lines.
int cmd_find_land(const core::Seed128& seed, int slot_arg, int moon_arg) {
  const gen::StarSystemParams system = gen::generate_system(gen::default_system_key(seed));
  const gen::SystemCell cell{};
  const int slot = slot_arg >= 0 ? slot_arg : gen::default_landable_slot(system);
  for (int s = 0; s < gen::kMaxPlanetSlots; ++s) {
    const gen::SystemPlanet& planet = system.planets[static_cast<std::size_t>(s)];
    if (!planet.occupied) {
      continue;
    }
    const gen::BodyHandle body = gen::body_for_system_slot(seed, cell, s);
    const gen::PlanetParams params = gen::planet_params_for_slot(system, s, body);
    const gen::MacroField macro(body.entity);
    const gen::ClimateField climate(body.entity, params, macro);
    const gen::LifeParams life = gen::derive_life(body.entity, params, climate);
    std::printf("slot %d: %-9s r=%.0f flux=%.2f meanT=%.0fK life=%s/%s%s\n", s,
                gen::to_string(params.type), params.radius_m.to_double(),
                params.flux_rel.to_double(), climate.mean_temperature_k(),
                gen::to_string(life.chemistry), gen::to_string(life.stage),
                s == slot ? "  <- scanning" : "");
  }
  const gen::BodyHandle body = moon_arg >= 0 ? gen::body_for_system_moon(seed, cell, slot, moon_arg)
                                             : gen::body_for_system_slot(seed, cell, slot);
  const gen::PlanetParams params =
      moon_arg >= 0 ? gen::planet_params_for_moon(system, slot, moon_arg, body)
                    : gen::planet_params_for_slot(system, slot, body);
  const gen::TerrainField field(body.entity, params);
  const double radius = params.radius_m.to_double();
  const double sea = params.sea_level_m.to_double();
  {
    const gen::LifeParams& life = field.life();
    std::printf("body: %s r=%.0f flux=%.2f palette=%u life=%s/%s\n", gen::to_string(params.type),
                radius, params.flux_rel.to_double(), params.palette_id,
                gen::to_string(life.chemistry), gen::to_string(life.stage));
  }
  std::uint64_t histogram[gen::kMaterialCount] = {};
  if (const char* at = std::getenv("INF_AT"); at != nullptr) {
    // Diagnostic: the province/terrain facts under a planet-local point.
    double ax = 0.0;
    double ay = 0.0;
    double az = 0.0;
    if (std::sscanf(at, "%lf %lf %lf", &ax, &ay, &az) == 3) {
      const double len = std::sqrt(ax * ax + ay * ay + az * az);
      const gen::Dir3 dir{det::Real(ax / len), det::Real(ay / len), det::Real(az / len)};
      const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir));
      const gen::BlendedParams blended = field.provinces().sample(dir);
      const double elevation = field.elevation_m(dir).to_double();
      std::printf("at: elev=%.1f sea=%.1f archetype=%s relief_amp=%.0f base=%.0f rugged=%.2f\n",
                  elevation, sea, gen::to_string(canonical.dominant_archetype),
                  blended.relief_amplitude_m.to_double(), blended.base_elevation_m.to_double(),
                  blended.ruggedness.to_double());
      // Relief statistics in a 2 km neighbourhood: min/max elevation over a grid.
      double lo = 1e30;
      double hi = -1e30;
      for (int j = -10; j <= 10; ++j) {
        for (int i = -10; i <= 10; ++i) {
          const double ox = ax + 100.0 * i;
          const double oy = ay + 100.0 * j;
          const double oz = az;
          const double ol = std::sqrt(ox * ox + oy * oy + oz * oz);
          const gen::Dir3 od{det::Real(ox / ol), det::Real(oy / ol), det::Real(oz / ol)};
          const double e = field.elevation_m(od).to_double();
          lo = std::min(lo, e);
          hi = std::max(hi, e);
        }
      }
      std::printf("at: elevation range within ~1 km: %.1f .. %.1f m (span %.1f m)\n", lo, hi, hi - lo);
    }
  }
  gen::TerrainField::ParamCache cache;
  struct Spot {
    double score;
    double x, y, z;
    double elevation;
    gen::Material material;
  };
  std::vector<Spot> spots;
  constexpr int kN = 48;
  for (int i = 0; i < kN * kN * 2; ++i) {
    // Fibonacci sphere.
    const double t = (i + 0.5) / (kN * kN * 2.0);
    const double phi = std::acos(1.0 - 2.0 * t);
    const double theta = 3.883222077450933 * i;
    const double dx = std::sin(phi) * std::cos(theta);
    const double dy = std::sin(phi) * std::sin(theta);
    const double dz = std::cos(phi);
    const gen::Dir3 dir{det::Real(dx), det::Real(dy), det::Real(dz)};
    const gen::TerrainField::ElevationD e = field.elevation_and_gradient(dir);
    const double elevation = e.elevation_m.to_double();
    if (elevation <= sea + 15.0) {
      continue;
    }
    const double grade = std::sqrt(e.slope.x.to_double() * e.slope.x.to_double() +
                                   e.slope.y.to_double() * e.slope.y.to_double() +
                                   e.slope.z.to_double() * e.slope.z.to_double());
    const double r = radius + elevation;
    const gen::VertexMaterial vm = field.classify_vertex(dx * r, dy * r, dz * r, dx, dy, dz, &cache);
    ++histogram[static_cast<std::size_t>(vm.mat0)];
    const gen::MaterialInputs in = field.material_inputs(dx * r, dy * r, dz * r, dx, dy, dz, &cache);
    // Prefer gentle, temperate, low land with a material blend nearby.
    const double score = (1.0 - std::min(grade, 1.0)) * (0.3 + in.climate.humidity) *
                         (in.climate.biotemp_c > 3.0 ? 1.0 : 0.3) *
                         (1.0 / (1.0 + std::max(0.0, elevation - sea) / 600.0));
    spots.push_back({score, dx * r, dy * r, dz * r, elevation, vm.mat0});
  }
  std::printf("primary materials over the scan:");
  for (std::uint32_t m = 1; m < gen::kMaterialCount; ++m) {
    if (histogram[m] > 0) {
      std::printf(" %s=%.1f%%", gen::material_info(static_cast<gen::Material>(m)).name,
                  100.0 * static_cast<double>(histogram[m]) / (kN * kN * 2.0));
    }
  }
  std::printf("\n");
  std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.score > b.score; });
  std::printf("%zu land spots on slot %d (sea %.0f m):\n", spots.size(), slot, sea);
  for (std::size_t i = 0; i < spots.size() && i < 8; ++i) {
    const Spot& sp = spots[i];
    const double len = std::sqrt(sp.x * sp.x + sp.y * sp.y + sp.z * sp.z);
    const double up = (len + 40.0) / len;
    std::printf("pos %.1f %.1f %.1f   # elev %.0f m, %s, score %.2f\n", sp.x * up, sp.y * up, sp.z * up,
                sp.elevation, gen::material_info(sp.material).name, sp.score);
  }
  return 0;
}

// T0019 follow-up: ground probe. Compares the analytic ground under a
// planet-local point with the density field along the radial and with
// the meshes the streamer produces there — the three things a player
// stands on, sees, and falls through when they disagree.
int cmd_probe_column(const core::Seed128& seed, int slot_arg, int moon_arg, double ax, double ay,
                     double az) {
  const gen::StarSystemParams system = gen::generate_system(gen::default_system_key(seed));
  const gen::SystemCell cell{};
  const int slot = slot_arg >= 0 ? slot_arg : gen::default_landable_slot(system);
  const gen::BodyHandle body = moon_arg >= 0 ? gen::body_for_system_moon(seed, cell, slot, moon_arg)
                                             : gen::body_for_system_slot(seed, cell, slot);
  const gen::PlanetParams params =
      moon_arg >= 0 ? gen::planet_params_for_moon(system, slot, moon_arg, body)
                    : gen::planet_params_for_slot(system, slot, body);
  const gen::TerrainField field(body.entity, params);
  const double len = std::sqrt(ax * ax + ay * ay + az * az);
  const gen::Dir3 dir{det::Real(ax / len), det::Real(ay / len), det::Real(az / len)};
  const double radius = params.radius_m.to_double();
  const double elevation = field.elevation_m(dir).to_double();
  const double ground = field.ground_radius_m(dir).to_double();
  const double floor = field.ground_radius_below_m(dir, det::Real(len)).to_double();
  std::printf("point |p|=%.2f  nominal+elevation=%.2f  ground_radius=%.2f  floor_from_point=%.2f\n",
              len, radius + elevation, ground, floor);
  std::printf("caves enabled=%d  detail at ground=%.2f\n", field.caves().enabled() ? 1 : 0,
              field.detail_m(gen::Dir3{dir.x * det::Real(ground), dir.y * det::Real(ground),
                                       dir.z * det::Real(ground)})
                  .to_double());
  // Density sign changes along the radial, ground +-60 m.
  std::printf("density crossings (r - ground):");
  double prev = 0.0;
  bool have_prev = false;
  int crossings = 0;
  for (double d = 60.0; d >= -60.0; d -= 0.2) {
    const det::Real r(ground + d);
    const double dens = field.density(gen::Dir3{dir.x * r, dir.y * r, dir.z * r}).to_double();
    if (have_prev && ((prev > 0.0) != (dens > 0.0))) {
      std::printf(" %+.1f", d);
      ++crossings;
    }
    prev = dens;
    have_prev = true;
  }
  std::printf("  [%d]\n", crossings);
  // Streamed meshes: run the chunk manager headless with the camera at
  // the point, drain, and intersect the radial with every triangle
  // within reach.
  const gen::TerrainSampler sampler(field);
  world::ChunkManagerConfig config;
  config.worker_count = 4;
  config.uploads_per_update = 100000;  // deliver everything: no per-frame budget headless
  world::ChunkManager manager(sampler, config);
  for (int round = 0; round < 220; ++round) {
    (void)manager.update(ax, ay, az);
    manager.drain();
  }
  (void)manager.update(ax, ay, az);
  const auto chunks = manager.resident_chunks();
  std::printf("resident chunks: %zu\n", chunks.size());
  // Which chunks contain this column, at which lod/shell, and do they
  // hold the surface?
  const gen::FaceUV fuv = gen::dir_to_face_uv(dir);
  std::printf("column: face=%d u=%.6f v=%.6f\n", fuv.face, fuv.u.to_double(), fuv.v.to_double());
  {
    int per_lod[24] = {};
    int per_face[6] = {};
    for (const auto& chunk : chunks) {
      if (chunk->addr.lod < 24) ++per_lod[chunk->addr.lod];
      if (chunk->addr.face < 6) ++per_face[chunk->addr.face];
    }
    std::printf("resident by lod:");
    for (int l = 0; l < 24; ++l) {
      if (per_lod[l] > 0) std::printf(" lod%d=%d", l, per_lod[l]);
    }
    std::printf("\nresident by face:");
    for (int f = 0; f < 6; ++f) std::printf(" f%d=%d", f, per_face[f]);
    std::printf("\n");
    // Expected cell indices for the column at each lod, and whether any
    // shell of that cell is resident.
    for (int l = 0; l < 24; ++l) {
      const double cells = static_cast<double>(std::uint64_t{1} << l);
      const auto ci = static_cast<std::uint32_t>((fuv.u.to_double() + 1.0) * 0.5 * cells);
      const auto cj = static_cast<std::uint32_t>((fuv.v.to_double() + 1.0) * 0.5 * cells);
      int found = 0;
      for (const auto& chunk : chunks) {
        if (chunk->addr.face == fuv.face && chunk->addr.lod == l && chunk->addr.i == ci &&
            chunk->addr.j == cj) {
          ++found;
        }
      }
      if (found > 0) std::printf("  cell lod%d (%u,%u): %d shells resident\n", l, ci, cj, found);
    }
  }
  for (const auto& chunk : chunks) {
    const world::ChunkGrid grid = world::ChunkGrid::from_addr(chunk->addr, params.radius_m);
    if (chunk->addr.face != fuv.face) continue;
    if (fuv.u.to_double() < grid.u0.to_double() || fuv.u.to_double() > grid.u1.to_double() ||
        fuv.v.to_double() < grid.v0.to_double() || fuv.v.to_double() > grid.v1.to_double()) {
      continue;
    }
    std::printf("  column chunk lod=%d shell=%d r=[%+.1f, %+.1f] rel. ground, triangles=%zu\n",
                chunk->addr.lod, chunk->addr.shell, grid.r0.to_double() - ground,
                grid.r1.to_double() - ground, chunk->mesh.triangle_count());
  }
  struct Hit {
    double r;
    int lod;
    int shell;
  };
  std::vector<Hit> hits;
  const double ox = ax / len;
  const double oy = ay / len;
  const double oz = az / len;
  for (const auto& chunk : chunks) {
    const auto& v = chunk->mesh.vertices;
    constexpr std::size_t kStride = world::ChunkMesh::kVertexFloats;
    for (std::size_t t = 0; t + 3 * kStride <= v.size(); t += 3 * kStride) {
      double P[3][3];
      for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) {
          P[k][c] = chunk->mesh.origin[c] + static_cast<double>(v[t + k * kStride + c]);
        }
      }
      // Ray from the origin along (ox,oy,oz): Moller-Trumbore.
      const double e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]};
      const double e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};
      const double h[3] = {oy * e2[2] - oz * e2[1], oz * e2[0] - ox * e2[2], ox * e2[1] - oy * e2[0]};
      const double det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
      if (std::abs(det) < 1e-12) continue;
      const double inv = 1.0 / det;
      const double s0[3] = {-P[0][0], -P[0][1], -P[0][2]};
      const double u = inv * (s0[0] * h[0] + s0[1] * h[1] + s0[2] * h[2]);
      if (u < 0.0 || u > 1.0) continue;
      const double q[3] = {s0[1] * e1[2] - s0[2] * e1[1], s0[2] * e1[0] - s0[0] * e1[2], s0[0] * e1[1] - s0[1] * e1[0]};
      const double w = inv * (ox * q[0] + oy * q[1] + oz * q[2]);
      if (w < 0.0 || u + w > 1.0) continue;
      const double r = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
      if (r > 0.0 && std::abs(r - ground) < 200.0) {
        hits.push_back({r, chunk->addr.lod, chunk->addr.shell});
      }
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.r > b.r; });
  std::printf("mesh surfaces on the radial (r - ground, lod, shell):");
  for (const Hit& h : hits) {
    std::printf(" (%+.2f, %d, %d)", h.r - ground, h.lod, h.shell);
  }
  std::printf("\n");
  return 0;
}

}  // namespace inf::cli
