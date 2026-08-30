#include <doctest/doctest.h>

#include <set>
#include <vector>

#include "core/key.hpp"

using namespace inf::core;

TEST_CASE("key tree: derivations are distinct and stable") {
  const Key universe = universe_key(Seed128{0x1234, 0x5678});

  const Key galaxy_a = derive_child(universe, Kind::Galaxy, 0, 0, 0);
  const Key galaxy_b = derive_child(universe, Kind::Galaxy, 1, 0, 0);
  const Key system_a = derive_child(universe, Kind::System, 0, 0, 0);
  const Key named = derive_named(universe, NameId::TerrainV1);

  // Same-index children of different kinds never collide (domain separation).
  CHECK(galaxy_a != system_a);
  CHECK(galaxy_a != galaxy_b);
  CHECK(galaxy_a != named);
  // Deterministic: same derivation, same key.
  CHECK(galaxy_a == derive_child(universe, Kind::Galaxy, 0, 0, 0));
  // Negative coordinates are distinct from positive.
  CHECK(derive_child(universe, Kind::Galaxy, -1, 0, 0) !=
        derive_child(universe, Kind::Galaxy, 1, 0, 0));
}

TEST_CASE("key tree: sibling layers are independent") {
  const Key body = derive_child(universe_key(Seed128{1, 2}), Kind::Body, 0);
  const Key terrain = derive_named(body, NameId::TerrainV1);
  const Key provinces = derive_named(body, NameId::ProvincesV1);
  CHECK(terrain != provinces);
  // A draw under one layer never equals the same draw under a sibling —
  // the extension-safety property in miniature.
  const ChunkAddr addr{2, 5, 10, 11, 0};
  CHECK(draw_chunk(terrain, Channel::Params, addr) !=
        draw_chunk(provinces, Channel::Params, addr));
}

TEST_CASE("draw counters: every field matters") {
  const Key layer = derive_named(universe_key(Seed128{7, 9}), NameId::TestV1);
  const ChunkAddr base{1, 4, 100, 200, -3};

  std::vector<inf::det::PhiloxOutput> outputs;
  outputs.push_back(draw_chunk(layer, Channel::Test, base, 0, 0));
  ChunkAddr addr = base;
  addr.face = 2;
  outputs.push_back(draw_chunk(layer, Channel::Test, addr, 0, 0));
  addr = base;
  addr.lod = 5;
  outputs.push_back(draw_chunk(layer, Channel::Test, addr, 0, 0));
  addr = base;
  addr.i = 101;
  outputs.push_back(draw_chunk(layer, Channel::Test, addr, 0, 0));
  addr = base;
  addr.j = 201;
  outputs.push_back(draw_chunk(layer, Channel::Test, addr, 0, 0));
  addr = base;
  addr.shell = 3;
  outputs.push_back(draw_chunk(layer, Channel::Test, addr, 0, 0));
  outputs.push_back(draw_chunk(layer, Channel::Test, base, 1, 0));
  outputs.push_back(draw_chunk(layer, Channel::Test, base, 0, 1));
  outputs.push_back(draw_chunk(layer, Channel::Params, base, 0, 0));

  std::set<std::uint64_t> first_words;
  for (const auto& out : outputs) {
    first_words.insert(out[0]);
  }
  CHECK(first_words.size() == outputs.size());
}

TEST_CASE("draw layouts: tick and point are tag-separated") {
  const Key layer = derive_named(universe_key(Seed128{7, 9}), NameId::TestV1);
  // Identical word patterns under different tags must differ.
  CHECK(draw_tick(layer, Channel::Test, 5, 6, 7) != draw_point(layer, Channel::Test, 5, 6, 7));
}
