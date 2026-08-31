#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "core/key.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"
#include "world/chunk_manager.hpp"

using namespace inf;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

// Runs the manager at a fixed camera until every scheduled chunk is
// resident, then returns (addr -> density_hash), fully sorted.
std::map<std::string, std::uint64_t> load_all(world::ChunkManager& manager, double x, double y,
                                              double z) {
  manager.update(x, y, z);
  manager.drain();
  for (int i = 0; i < 1000; ++i) {
    const auto events = manager.update(x, y, z);
    bool any_ready = false;
    for (const auto& event : events) {
      any_ready = any_ready || event.kind == world::ChunkEvent::Kind::Ready;
    }
    if (!any_ready) {
      break;
    }
    manager.drain();
  }
  std::map<std::string, std::uint64_t> result;
  for (const auto& chunk : manager.resident_chunks()) {
    char key[64];
    std::snprintf(key, sizeof(key), "%u/%u/%u/%u/%d", chunk->addr.face, chunk->addr.lod,
                  chunk->addr.i, chunk->addr.j, static_cast<int>(chunk->addr.shell));
    result[key] = chunk->density_hash;
  }
  return result;
}

}  // namespace

TEST_CASE("chunk manager: output independent of worker count") {
  const gen::BodyHandle body = body_for(99);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const double radius = planet.radius_m.to_double();

  world::ChunkManagerConfig config_a;
  config_a.worker_count = 1;
  config_a.max_lod = 6;  // small scene for test speed
  config_a.uploads_per_update = 100000;
  world::ChunkManagerConfig config_b = config_a;
  config_b.worker_count = 4;

  const gen::TerrainField field(body.entity, planet);
  const gen::TerrainSampler sampler(field);
  world::ChunkManager manager_a(sampler, config_a);
  world::ChunkManager manager_b(sampler, config_b);

  const auto scene_a = load_all(manager_a, radius * 1.5, radius * 0.2, radius * 0.1);
  const auto scene_b = load_all(manager_b, radius * 1.5, radius * 0.2, radius * 0.1);

  REQUIRE(!scene_a.empty());
  CHECK(scene_a == scene_b);  // same chunk set, identical density hashes
}

TEST_CASE("chunk manager: closer camera yields finer chunks; eviction respects budget") {
  const gen::BodyHandle body = body_for(7);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::Barren);
  const double radius = planet.radius_m.to_double();

  world::ChunkManagerConfig config;
  config.worker_count = 4;
  config.max_lod = 7;
  config.uploads_per_update = 100000;
  config.resident_budget = 64;
  const gen::TerrainField field(body.entity, planet);
  const gen::TerrainSampler sampler(field);
  world::ChunkManager manager(sampler, config);

  const auto far_scene = load_all(manager, radius * 4.0, 0.0, 0.0);
  std::uint8_t far_max_lod = 0;
  for (const auto& [key, hash] : far_scene) {
    far_max_lod = std::max<std::uint8_t>(far_max_lod, static_cast<std::uint8_t>(
        std::stoi(key.substr(key.find('/') + 1))));
  }

  const auto near_scene = load_all(manager, radius * 1.02, 0.0, 0.0);
  std::uint8_t near_max_lod = 0;
  for (const auto& [key, hash] : near_scene) {
    near_max_lod = std::max<std::uint8_t>(near_max_lod, static_cast<std::uint8_t>(
        std::stoi(key.substr(key.find('/') + 1))));
  }
  CHECK(near_max_lod > far_max_lod);
  CHECK(manager.resident_chunks().size() <= config.resident_budget + 16);
}

TEST_CASE("chunk manager: scene hash is a pure function") {
  const gen::BodyHandle body = body_for(3);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::Ice);
  world::ChunkManagerConfig config;
  config.worker_count = 2;
  const gen::TerrainField field(body.entity, planet);
  const gen::TerrainSampler sampler(field);
  world::ChunkManager manager_a(sampler, config);
  world::ChunkManager manager_b(sampler, config);

  const std::vector<core::ChunkAddr> addrs = {
      {0, 5, 10, 11, 0}, {3, 6, 40, 41, 1}, {5, 4, 3, 2, -1}};
  CHECK(manager_a.scene_hash(addrs) == manager_b.scene_hash(addrs));
}
