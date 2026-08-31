#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/key.hpp"
#include "gen/effective_field.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "world/chunk_manager.hpp"
#include "world/edit_store.hpp"

using namespace inf;

namespace {

world::SphereEdit sphere(double x, double y, double z, double radius, bool subtract) {
  world::SphereEdit edit;
  edit.center_raw[0] = det::Fixed64::from_double(x).raw();
  edit.center_raw[1] = det::Fixed64::from_double(y).raw();
  edit.center_raw[2] = det::Fixed64::from_double(z).raw();
  edit.radius_raw = det::Fixed64::from_double(radius).raw();
  edit.subtract = subtract;
  return edit;
}

struct Ground {
  gen::BodyHandle body;
  gen::PlanetParams planet;
  gen::TerrainField field;
  gen::Dir3 dir;
  double r0;

  explicit Ground(std::uint64_t lo)
      : body(gen::default_body(core::Seed128{0, lo})),
        planet(gen::derive_planet_params(body, gen::PlanetType::Barren)),
        field(body.entity, planet) {
    const double raw[3] = {0.45, 0.65, 0.61};
    const double len = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    dir = gen::Dir3{det::Real(raw[0] / len), det::Real(raw[1] / len), det::Real(raw[2] / len)};
    r0 = field.ground_radius_m(dir).to_double();
  }

  double px() const { return dir.x.to_double() * r0; }
  double py() const { return dir.y.to_double() * r0; }
  double pz() const { return dir.z.to_double() * r0; }
};

}  // namespace

TEST_CASE("edit store: append order, overlap query, save/load round-trip") {
  world::CsgEditStore store;
  store.append(sphere(10.0, 0.0, 0.0, 3.0, true));
  store.append(sphere(-50.0, 2.0, 1.0, 2.0, false));
  store.append(sphere(10.5, 0.5, 0.0, 1.5, true));
  REQUIRE(store.size() == 3);

  const det::Fixed64 near10[3] = {det::Fixed64::from_double(10.0), det::Fixed64::from_double(0.0),
                                  det::Fixed64::from_double(0.0)};
  const auto hits = store.overlapping(near10, det::Fixed64::from_double(1.0));
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].op_id < hits[1].op_id);  // op order preserved

  const std::string path = "test-edits-roundtrip.bin";
  REQUIRE(store.save(path));
  world::CsgEditStore loaded;
  REQUIRE(loaded.load(path));
  CHECK(loaded.edits() == store.edits());  // bit-exact (fixed64 raws)
  // A fresh append after load continues the op-id sequence.
  const std::uint64_t next = loaded.append(sphere(0, 0, 0, 1.0, true));
  CHECK(next == 4);
  std::remove(path.c_str());
}

TEST_CASE("apply_edits: CSG fold semantics in op order") {
  std::vector<world::SphereEdit> ops;
  ops.push_back(sphere(0.0, 0.0, 0.0, 3.0, true));  // carve
  // Solid point 1 m from center: carve wins (dist 1 - r 3 = -2).
  CHECK(world::apply_edits(5.0, ops, 1.0, 0.0, 0.0) == doctest::Approx(-2.0));
  // Later add refills part of the hole — order matters.
  ops.push_back(sphere(1.0, 0.0, 0.0, 1.5, false));
  CHECK(world::apply_edits(5.0, ops, 1.0, 0.0, 0.0) == doctest::Approx(1.5));
  // Outside every op: base untouched.
  CHECK(world::apply_edits(5.0, ops, 100.0, 0.0, 0.0) == doctest::Approx(5.0));
}

TEST_CASE("effective field: digging lowers the ground, adding raises it") {
  const Ground g(21);
  world::CsgEditStore store;
  const gen::EffectiveField effective(g.field, &store);

  // Empty overlay: passthrough.
  CHECK(effective.ground_radius_m(g.dir).to_double() == doctest::Approx(g.r0));

  // Dig a 3 m sphere centered on the surface point.
  store.append(sphere(g.px(), g.py(), g.pz(), 3.0, true));
  const double dug = effective.ground_radius_m(g.dir).to_double();
  CHECK(dug < g.r0 - 1.0);
  CHECK(dug > g.r0 - 4.5);

  // Refill and pile on top: ground climbs above the original surface.
  store.append(sphere(g.px(), g.py(), g.pz(), 3.5, false));
  const double piled = effective.ground_radius_m(g.dir).to_double();
  CHECK(piled > g.r0 + 1.0);
  CHECK(piled < g.r0 + 4.5);

  // Point queries agree with the fold.
  const gen::Dir3 above{det::Real(g.dir.x.to_double() * (g.r0 + 2.0)),
                        det::Real(g.dir.y.to_double() * (g.r0 + 2.0)),
                        det::Real(g.dir.z.to_double() * (g.r0 + 2.0))};
  CHECK(effective.density(above).to_double() > 0.0);  // inside the pile
}

TEST_CASE("terrain sampler folds edits into padded grids; empty store is bit-identical") {
  const Ground g(5);
  world::CsgEditStore empty;
  const gen::TerrainSampler plain(g.field);
  const gen::TerrainSampler with_empty(g.field, &empty);

  const core::ChunkAddr addr = [&] {
    // Chunk at the surface point, mid lod.
    const world::FaceUV face_uv = world::dir_to_face_uv(g.dir);
    const std::uint32_t cells = 1U << 6U;
    const auto to_cell = [&](double c) {
      const double f = (c + 1.0) * 0.5 * cells;
      return static_cast<std::uint32_t>(f < 0 ? 0 : (f >= cells ? cells - 1 : f));
    };
    return core::ChunkAddr{face_uv.face, 6, to_cell(face_uv.u.to_double()),
                           to_cell(face_uv.v.to_double()), 0};
  }();
  const world::ChunkGrid grid = world::ChunkGrid::from_addr(addr, g.planet.radius_m);

  const world::PaddedDensity base = plain.sample_padded(grid);
  const world::PaddedDensity same = with_empty.sample_padded(grid);
  REQUIRE(base.values.size() == same.values.size());
  bool identical = true;
  for (std::size_t i = 0; i < base.values.size(); ++i) {
    identical = identical && std::bit_cast<std::uint64_t>(base.values[i].to_double()) ==
                            std::bit_cast<std::uint64_t>(same.values[i].to_double());
  }
  CHECK(identical);

  // A dig inside the chunk changes some samples, all toward air.
  world::CsgEditStore store;
  const world::Dir3 inside = grid.corner_position(16, 16, 16);
  const double voxel = 2.0 * g.planet.radius_m.to_double() / 64.0 / 32.0;
  store.append(sphere(inside.x.to_double(), inside.y.to_double(), inside.z.to_double(),
                      3.0 * voxel, true));
  const gen::TerrainSampler edited(g.field, &store);
  const world::PaddedDensity carved = edited.sample_padded(grid);
  int changed = 0;
  bool monotone = true;
  for (std::size_t i = 0; i < base.values.size(); ++i) {
    if (std::bit_cast<std::uint64_t>(carved.values[i].to_double()) !=
        std::bit_cast<std::uint64_t>(base.values[i].to_double())) {
      ++changed;
      monotone = monotone && carved.values[i].to_double() <= base.values[i].to_double();
    }
  }
  CHECK(changed > 0);
  CHECK(monotone);  // subtract only ever removes material
}

TEST_CASE("chunk manager: invalidate_sphere re-meshes edited chunks") {
  const Ground g(7);
  const double radius = g.planet.radius_m.to_double();

  world::CsgEditStore store;
  const gen::TerrainSampler sampler(g.field, &store);
  world::ChunkManagerConfig config;
  config.worker_count = 4;
  config.max_lod = 6;  // small scene for test speed
  config.uploads_per_update = 100000;
  world::ChunkManager manager(sampler, config);

  const auto load_all = [&] {
    std::map<std::string, std::uint64_t> result;
    for (int i = 0; i < 1000; ++i) {
      const auto events = manager.update(g.px(), g.py(), g.pz());
      manager.drain();
      bool any = false;
      for (const auto& event : events) {
        any = any || event.kind == world::ChunkEvent::Kind::Ready;
      }
      if (i > 0 && !any) {
        break;
      }
    }
    for (const auto& chunk : manager.resident_chunks()) {
      char key[64];
      std::snprintf(key, sizeof(key), "%u/%u/%u/%u/%d", chunk->addr.face, chunk->addr.lod,
                    chunk->addr.i, chunk->addr.j, static_cast<int>(chunk->addr.shell));
      result[key] = chunk->density_hash;
    }
    return result;
  };

  const auto before = load_all();
  REQUIRE(!before.empty());

  // Carve at the surface point (big enough to cross corner samples at this
  // coarse lod: voxels are ~radius/1000 here).
  const double dig_radius = radius / 500.0;
  store.append(sphere(g.px(), g.py(), g.pz(), dig_radius, true));
  manager.invalidate_sphere(g.px(), g.py(), g.pz(), dig_radius * 1.5);

  const auto after = load_all();
  REQUIRE(!after.empty());
  int differing = 0;
  for (const auto& [key, hash] : after) {
    const auto it = before.find(key);
    if (it != before.end() && it->second != hash) {
      ++differing;
    }
  }
  CHECK(differing > 0);  // the edited chunks were re-sampled and re-meshed

  // Restart simulation: replaying the same diff over the same seed gives
  // the same effective scene (persistence = diff only).
  const std::string path = "test-edits-restart.bin";
  REQUIRE(store.save(path));
  world::CsgEditStore replay;
  REQUIRE(replay.load(path));
  const gen::TerrainSampler sampler2(g.field, &replay);
  world::ChunkManager manager2(sampler2, config);
  // Fresh manager, same camera: identical density hashes chunk for chunk.
  std::map<std::string, std::uint64_t> rebuilt;
  {
    for (int i = 0; i < 1000; ++i) {
      const auto events = manager2.update(g.px(), g.py(), g.pz());
      manager2.drain();
      bool any = false;
      for (const auto& event : events) {
        any = any || event.kind == world::ChunkEvent::Kind::Ready;
      }
      if (i > 0 && !any) {
        break;
      }
    }
    for (const auto& chunk : manager2.resident_chunks()) {
      char key[64];
      std::snprintf(key, sizeof(key), "%u/%u/%u/%u/%d", chunk->addr.face, chunk->addr.lod,
                    chunk->addr.i, chunk->addr.j, static_cast<int>(chunk->addr.shell));
      rebuilt[key] = chunk->density_hash;
    }
  }
  CHECK(rebuilt == after);
  std::remove(path.c_str());
}
