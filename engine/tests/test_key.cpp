#include <doctest/doctest.h>

#include <set>
#include <vector>

#include "core/key.hpp"

using namespace inf::core;

namespace {
// Local opaque ids for mechanics tests (real registries live in the game).
constexpr inf::core::NameId kNameA{0xf9b59960daf8dc2eULL};
constexpr inf::core::NameId kNameB{0xe930f9d5ff800838ULL};
constexpr inf::core::NameId kNameTest{0x5f75b2f645a90e20ULL};
constexpr inf::core::KindId kKindGalaxy{1};
constexpr inf::core::KindId kKindSystem{2};
constexpr inf::core::KindId kKindBody{3};
constexpr inf::core::ChannelId kChanParams{1};
constexpr inf::core::ChannelId kChanTest{0xFFFFFF};
}  // namespace


TEST_CASE("key tree: derivations are distinct and stable") {
  const Key universe = universe_key(Seed128{0x1234, 0x5678});

  const Key galaxy_a = derive_child(universe, kKindGalaxy, 0, 0, 0);
  const Key galaxy_b = derive_child(universe, kKindGalaxy, 1, 0, 0);
  const Key system_a = derive_child(universe, kKindSystem, 0, 0, 0);
  const Key named = derive_named(universe, kNameA);

  // Same-index children of different kinds never collide (domain separation).
  CHECK(galaxy_a != system_a);
  CHECK(galaxy_a != galaxy_b);
  CHECK(galaxy_a != named);
  // Deterministic: same derivation, same key.
  CHECK(galaxy_a == derive_child(universe, kKindGalaxy, 0, 0, 0));
  // Negative coordinates are distinct from positive.
  CHECK(derive_child(universe, kKindGalaxy, -1, 0, 0) !=
        derive_child(universe, kKindGalaxy, 1, 0, 0));
}

TEST_CASE("key tree: sibling layers are independent") {
  const Key body = derive_child(universe_key(Seed128{1, 2}), kKindBody, 0);
  const Key terrain = derive_named(body, kNameA);
  const Key provinces = derive_named(body, kNameB);
  CHECK(terrain != provinces);
  // A draw under one layer never equals the same draw under a sibling —
  // the extension-safety property in miniature.
  const ChunkAddr addr{2, 5, 10, 11, 0};
  CHECK(draw_chunk(terrain, kChanParams, addr) !=
        draw_chunk(provinces, kChanParams, addr));
}

TEST_CASE("draw counters: every field matters") {
  const Key layer = derive_named(universe_key(Seed128{7, 9}), kNameTest);
  const ChunkAddr base{1, 4, 100, 200, -3};

  std::vector<inf::det::PhiloxOutput> outputs;
  outputs.push_back(draw_chunk(layer, kChanTest, base, 0, 0));
  ChunkAddr addr = base;
  addr.face = 2;
  outputs.push_back(draw_chunk(layer, kChanTest, addr, 0, 0));
  addr = base;
  addr.lod = 5;
  outputs.push_back(draw_chunk(layer, kChanTest, addr, 0, 0));
  addr = base;
  addr.i = 101;
  outputs.push_back(draw_chunk(layer, kChanTest, addr, 0, 0));
  addr = base;
  addr.j = 201;
  outputs.push_back(draw_chunk(layer, kChanTest, addr, 0, 0));
  addr = base;
  addr.shell = 3;
  outputs.push_back(draw_chunk(layer, kChanTest, addr, 0, 0));
  outputs.push_back(draw_chunk(layer, kChanTest, base, 1, 0));
  outputs.push_back(draw_chunk(layer, kChanTest, base, 0, 1));
  outputs.push_back(draw_chunk(layer, kChanParams, base, 0, 0));

  std::set<std::uint64_t> first_words;
  for (const auto& out : outputs) {
    first_words.insert(out[0]);
  }
  CHECK(first_words.size() == outputs.size());
}

TEST_CASE("draw layouts: tick and point are tag-separated") {
  const Key layer = derive_named(universe_key(Seed128{7, 9}), kNameTest);
  // Identical word patterns under different tags must differ.
  CHECK(draw_tick(layer, kChanTest, 5, 6, 7) != draw_point(layer, kChanTest, 5, 6, 7));
}

TEST_CASE("key: octree level participates in child derivation (T0017)") {
  using namespace inf::core;
  constexpr KindId kKindSystem{2};
  const Key universe = universe_key(Seed128{7, 13});
  // Same coordinates at different levels must NOT collide — this was the
  // entry_key bug: Address::str()/hash() included w, the key did not.
  const Key level0 = derive_child(universe, kKindSystem, 3, 5, 7, 0);
  const Key level1 = derive_child(universe, kKindSystem, 3, 5, 7, 1);
  const Key level9 = derive_child(universe, kKindSystem, 3, 5, 7, 9);
  CHECK(level0 != level1);
  CHECK(level1 != level9);
  CHECK(level0 != level9);
  // Level 0 is bit-identical to the historical 3-coordinate derivation —
  // no existing key (and no golden) moved.
  CHECK(level0 == derive_child(universe, kKindSystem, 3, 5, 7));
}
