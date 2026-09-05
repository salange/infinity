#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <set>

#include "gen/biome.hpp"
#include "gen/climate.hpp"
#include "gen/life.hpp"
#include "gen/material.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "tex/tiles.hpp"
#include "world/chunk_manager.hpp"

using namespace inf;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) { return gen::default_body(core::Seed128{0, lo}); }

gen::Dir3 dir(double x, double y, double z) {
  const double len = std::sqrt(x * x + y * y + z * z);
  return gen::Dir3{det::Real(x / len), det::Real(y / len), det::Real(z / len)};
}

}  // namespace

TEST_CASE("climate/v1: deterministic, physical, uses the planet's full range") {
  for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
    const auto type = static_cast<gen::PlanetType>(type_index);
    const gen::BodyHandle body = body_for(11);
    const gen::PlanetParams planet = gen::derive_planet_params(body, type);
    const gen::MacroField macro(body.entity);
    const gen::ClimateField a(body.entity, planet, macro);
    const gen::ClimateField b(body.entity, planet, macro);
    CAPTURE(type_index);
    const gen::Climate ca = a.sample(dir(0.3, 0.2, 0.9), 100.0, 100.0);
    const gen::Climate cb = b.sample(dir(0.3, 0.2, 0.9), 100.0, 100.0);
    CHECK(ca.temperature_k == cb.temperature_k);
    CHECK(ca.humidity == cb.humidity);
    CHECK(ca.t01 == cb.t01);
    // Poles colder than the equator on a low-obliquity, non-locked world.
    if (!planet.tidally_locked && planet.obliquity_rad.to_double() < 0.6) {
      const gen::Climate eq = a.sample(dir(1.0, 0.0, 0.0), 0.0, 0.0);
      const gen::Climate pole = a.sample(dir(0.0, 0.0, 1.0), 0.0, 0.0);
      CHECK(pole.temperature_k < eq.temperature_k);
    }
    // Altitude cools.
    const gen::Climate low = a.sample(dir(0.5, 0.5, 0.1), 0.0, 0.0);
    const gen::Climate high = a.sample(dir(0.5, 0.5, 0.1), 2000.0, 2000.0);
    CHECK(high.temperature_k < low.temperature_k);
    // Ranges.
    CHECK(ca.humidity >= 0.0);
    CHECK(ca.humidity <= 1.0);
    CHECK(ca.temperature_k > 3.0);
    CHECK(ca.temperature_k < 700.0);
    CHECK(ca.t01 >= 0.0);
    CHECK(ca.t01 <= 1.0);
    // Dry worlds stay dry; EarthLike worlds get real humidity somewhere.
    if (type == gen::PlanetType::Barren || type == gen::PlanetType::Desert) {
      CHECK(a.mean_humidity() < 0.2);
    }
    if (type == gen::PlanetType::EarthLike) {
      CHECK(a.mean_humidity() > 0.15);
    }
  }
}

TEST_CASE("life/v1: habitable-but-sterile worlds exist; life needs habitability") {
  int habitable = 0;
  int sterile_habitable = 0;
  int occupied = 0;
  std::set<int> chemistries;
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    for (std::uint32_t type_index = 0; type_index < 4; ++type_index) {
      const auto type = static_cast<gen::PlanetType>(type_index);
      const gen::BodyHandle body = body_for(seed);
      const gen::PlanetParams planet = gen::derive_planet_params(body, type);
      const gen::MacroField macro(body.entity);
      const gen::ClimateField climate(body.entity, planet, macro);
      const gen::LifeParams life = gen::derive_life(body.entity, planet, climate);
      const gen::LifeParams again = gen::derive_life(body.entity, planet, climate);
      CHECK(life.stage == again.stage);
      CHECK(life.chemistry == again.chemistry);
      if (life.occupied) {
        CHECK(life.habitable);
        CHECK(life.chemistry != gen::LifeChemistry::None);
        CHECK(life.stage != gen::LifeStage::Sterile);
        chemistries.insert(static_cast<int>(life.chemistry));
        ++occupied;
      } else {
        CHECK(life.chemistry == gen::LifeChemistry::None);
        CHECK(static_cast<int>(life.stage) <= 1);
      }
      habitable += life.habitable ? 1 : 0;
      sterile_habitable += (life.habitable && !life.occupied) ? 1 : 0;
      // Airless Barren worlds only host cryogenic crystalline life.
      if (type == gen::PlanetType::Barren && life.occupied) {
        CHECK(life.chemistry == gen::LifeChemistry::Crystalline);
      }
    }
  }
  CHECK(habitable > 400);           // plenty of candidates
  CHECK(sterile_habitable > 100);   // the "could, but doesn't" worlds
  CHECK(occupied > 100);
  CHECK(chemistries.size() >= 3);   // carbon plus exotic chemistries appear
}

TEST_CASE("material/v2: pure, deterministic, two valid ids, physically sane") {
  const gen::BodyHandle body = body_for(83);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);
  const double radius = planet.radius_m.to_double();
  gen::TerrainField::ParamCache cache;
  int distinct = 0;
  std::set<int> ids;
  for (int i = 0; i < 200; ++i) {
    const double a = 0.1 + i * 0.037;
    const gen::Dir3 d = dir(std::cos(a), std::sin(a) * 0.7, std::sin(a * 0.31));
    const double elevation = field.elevation_m(d).to_double();
    const double r = radius + elevation;
    const double px = d.x.to_double() * r;
    const double py = d.y.to_double() * r;
    const double pz = d.z.to_double() * r;
    const gen::VertexMaterial vm =
        field.classify_vertex(px, py, pz, d.x.to_double(), d.y.to_double(), d.z.to_double(), &cache);
    const gen::VertexMaterial again =
        field.classify_vertex(px, py, pz, d.x.to_double(), d.y.to_double(), d.z.to_double());
    CHECK(vm.mat0 == again.mat0);
    CHECK(vm.mat1 == again.mat1);
    CHECK(vm.blend == again.blend);
    CHECK(vm.mat0 != gen::Material::None);
    CHECK(static_cast<std::uint32_t>(vm.mat0) < gen::kMaterialCount);
    CHECK(static_cast<std::uint32_t>(vm.mat1) < gen::kMaterialCount);
    CHECK(vm.blend >= 0.0f);
    CHECK(vm.blend <= 1.0f);
    ids.insert(static_cast<int>(vm.mat0));
    // A 60-degree cliff face reads as bedrock (or its living cover),
    // never as sand: tilt the normal along a tangent of the sphere.
    const double dx = d.x.to_double();
    const double dy = d.y.to_double();
    const double dz = d.z.to_double();
    double tx = -dy;
    double ty = dx;
    double tz = 0.0;
    const double tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
    tx /= tlen;
    ty /= tlen;
    const double nx = 0.5 * dx + 0.866 * tx;
    const double ny = 0.5 * dy + 0.866 * ty;
    const double nz = 0.5 * dz + 0.866 * tz;
    const gen::VertexMaterial cliff = field.classify_vertex(px, py, pz, nx, ny, nz, &cache);
    CHECK(cliff.mat0 != gen::Material::SandBeach);
    CHECK(cliff.mat0 != gen::Material::SandDune);
    // Deep below the surface it is bedrock, whatever the surface wears.
    const double deep_r = r - 6.0;
    const gen::VertexMaterial buried = field.classify_vertex(
        d.x.to_double() * deep_r, d.y.to_double() * deep_r, d.z.to_double() * deep_r,
        d.x.to_double(), d.y.to_double(), d.z.to_double(), &cache);
    const bool rock = buried.mat0 == gen::Material::RockGranite ||
                      buried.mat0 == gen::Material::RockShale ||
                      buried.mat0 == gen::Material::RockBasalt ||
                      buried.mat0 == gen::Material::RockSandstone;
    CHECK(rock);
    distinct = static_cast<int>(ids.size());
  }
  CHECK(distinct >= 3);
  // Registry integrity.
  for (std::uint32_t id = 0; id < gen::kMaterialCount; ++id) {
    const gen::MaterialInfo& info = gen::material_info(static_cast<gen::Material>(id));
    CHECK(info.name != nullptr);
    CHECK(info.tile_m > 0.0f);
  }
}

TEST_CASE("biome/v1: grid covers the axes; alpine belt with altitude") {
  std::set<int> seen;
  for (int t = 0; t <= 10; ++t) {
    for (int h = 0; h <= 10; ++h) {
      const gen::BiomeSample b = gen::classify_biome(t / 10.0, h / 10.0, 2.0 + 26.0 * t / 10.0, 0.0);
      seen.insert(static_cast<int>(b.primary));
      CHECK(b.blend >= 0.0);
      CHECK(b.blend <= 0.5001);
    }
  }
  CHECK(seen.size() >= 8);
  const gen::BiomeSample high = gen::classify_biome(0.8, 0.6, 24.0, 5000.0);
  CHECK(high.primary == gen::Biome::Alpine);
  const gen::BiomeSample frozen = gen::classify_biome(0.9, 0.9, 0.0, 0.0);
  CHECK((frozen.primary == gen::Biome::PolarDesert || frozen.primary == gen::Biome::Tundra));
}

TEST_CASE("tex: procedural tiles are seamless and deterministic") {
  std::size_t count = 0;
  const char* const* names = tex::known_tile_names(&count);
  REQUIRE(count > 20);
  for (std::size_t i = 0; i < count; i += 5) {
    const tex::Tile a = tex::generate_tile(names[i], 64, 7);
    const tex::Tile b = tex::generate_tile(names[i], 64, 7);
    REQUIRE(a.albedo.size() == 64 * 64 * 4);
    CHECK(std::memcmp(a.albedo.data(), b.albedo.data(), a.albedo.size()) == 0);
    CHECK(std::memcmp(a.normal.data(), b.normal.data(), a.normal.size()) == 0);
    // Seamless: the wrap-around neighbours differ no more than adjacent
    // interior texels do (on average), i.e. no edge discontinuity.
    double edge = 0.0;
    double interior = 0.0;
    for (int y = 0; y < 64; ++y) {
      for (int c = 0; c < 3; ++c) {
        edge += std::abs(static_cast<int>(a.albedo[(y * 64 + 63) * 4 + c]) -
                         static_cast<int>(a.albedo[(y * 64 + 0) * 4 + c]));
        interior += std::abs(static_cast<int>(a.albedo[(y * 64 + 31) * 4 + c]) -
                             static_cast<int>(a.albedo[(y * 64 + 32) * 4 + c]));
      }
    }
    CAPTURE(names[i]);
    CHECK(edge < interior * 2.5 + 64.0 * 3.0 * 2.0);
  }
}

TEST_CASE("terrain: walking floor from inside solid climbs back to the surface") {
  // A player whose feet ended up below the surface (overshoot, edit) must
  // stand on the surface above, not sink.
  const gen::BodyHandle body = body_for(83);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);
  int checked = 0;
  for (int i = 0; i < 120; ++i) {
    const double a = 0.3 + i * 0.151;
    const gen::Dir3 d = dir(std::cos(a), std::sin(a) * 0.8, std::sin(a * 0.37) * 0.6);
    const double top = field.ground_radius_m(d).to_double();
    if (top < planet.radius_m.to_double() + planet.sea_level_m.to_double()) {
      continue;  // under water: the sea deck handles it
    }
    const double floor = field.ground_radius_below_m(d, det::Real(top - 4.0)).to_double();
    CAPTURE(i);
    CHECK(floor > top - 4.5);
    CHECK(floor < top + 0.5);
    ++checked;
  }
  CHECK(checked >= 8);
}

TEST_CASE("chunk streamer: the column under a grounded camera refines to max lod, mesh on ground") {
  // The two fall-through mechanisms: refinement stalling on high terrain
  // (distance measured against the nominal sphere) and stale coarse
  // parents drawn over their children.
  const gen::BodyHandle body = body_for(83);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);
  const gen::TerrainSampler sampler(field);
  gen::Dir3 best = dir(1.0, 0.2, 0.1);
  double best_elev = -1e9;
  for (int i = 0; i < 24; ++i) {
    const double a = 0.2 + i * 0.26;
    const gen::Dir3 d = dir(std::cos(a), std::sin(a), std::sin(a * 0.7) * 0.5);
    const double e = field.elevation_m(d).to_double();
    if (e > best_elev) {
      best_elev = e;
      best = d;
    }
  }
  REQUIRE(best_elev > planet.sea_level_m.to_double());
  const double ground = field.ground_radius_m(best).to_double();
  const double cx = best.x.to_double() * (ground + 1.8);
  const double cy = best.y.to_double() * (ground + 1.8);
  const double cz = best.z.to_double() * (ground + 1.8);
  world::ChunkManagerConfig config;
  config.worker_count = 4;
  config.max_lod = 14;
  config.uploads_per_update = 100000;
  world::ChunkManager manager(sampler, config);
  for (int round = 0; round < 80; ++round) {
    (void)manager.update(cx, cy, cz);
    manager.drain();
  }
  (void)manager.update(cx, cy, cz);
  const auto chunks = manager.resident_chunks();
  const gen::FaceUV fuv = gen::dir_to_face_uv(best);
  int finest = -1;
  int ancestors_resident = 0;
  for (const auto& chunk : chunks) {
    if (chunk->addr.face != fuv.face) continue;
    const double cells = static_cast<double>(std::uint64_t{1} << chunk->addr.lod);
    const auto ci = static_cast<std::uint32_t>((fuv.u.to_double() + 1.0) * 0.5 * cells);
    const auto cj = static_cast<std::uint32_t>((fuv.v.to_double() + 1.0) * 0.5 * cells);
    if (chunk->addr.i == ci && chunk->addr.j == cj) {
      finest = std::max(finest, static_cast<int>(chunk->addr.lod));
      if (chunk->addr.lod < config.max_lod) {
        ++ancestors_resident;
      }
    }
  }
  CHECK(finest == config.max_lod);
  CHECK(ancestors_resident == 0);
  const double voxel = 2.0 * planet.radius_m.to_double() / static_cast<double>(1 << 14) / 32.0;
  double nearest = 1e9;
  const double ox = best.x.to_double();
  const double oy = best.y.to_double();
  const double oz = best.z.to_double();
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
      const double e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]};
      const double e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};
      const double h[3] = {oy * e2[2] - oz * e2[1], oz * e2[0] - ox * e2[2], ox * e2[1] - oy * e2[0]};
      const double det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
      if (std::abs(det) < 1e-12) continue;
      const double inv = 1.0 / det;
      const double s0[3] = {-P[0][0], -P[0][1], -P[0][2]};
      const double u = inv * (s0[0] * h[0] + s0[1] * h[1] + s0[2] * h[2]);
      if (u < 0.0 || u > 1.0) continue;
      const double q[3] = {s0[1] * e1[2] - s0[2] * e1[1], s0[2] * e1[0] - s0[0] * e1[2],
                           s0[0] * e1[1] - s0[1] * e1[0]};
      const double w = inv * (ox * q[0] + oy * q[1] + oz * q[2]);
      if (w < 0.0 || u + w > 1.0) continue;
      const double r = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
      nearest = std::min(nearest, std::abs(r - ground));
    }
  }
  CHECK(nearest < voxel);
}
