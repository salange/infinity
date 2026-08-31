#pragma once

#include <cstdint>

#include "core/chunk_addr.hpp"
#include "core/det/philox.hpp"
#include "core/seed.hpp"

namespace inf::core {

// The 128-bit key tree (seeding spec sections 2-3, counter layouts in
// section 9, NORMATIVE). Every entity/subsystem key is derived purely from
// its parent's key and its own stable identity; leaf draws are
// counter-indexed under a layer key. The typed functions below are the
// only way to build counters — layouts are never assembled by hand.
//
// The id spaces are OPAQUE here (engine framework); the game defines the
// frozen registries of values (layer names, kinds, channels) — see the
// game's names header. NameId values are the first 8 bytes (LE) of
// MD5(name string); KindId/ChannelId are small append-only registry
// constants.

// Distinct from Seed128/PhiloxOutput on purpose: keys don't do arithmetic.
struct Key {
  std::uint64_t k0{0};
  std::uint64_t k1{0};

  friend bool operator==(const Key&, const Key&) = default;
};

enum class NameId : std::uint64_t {};
enum class KindId : std::uint32_t {};
enum class ChannelId : std::uint32_t {};

// The root of the tree: the universe seed IS the root key.
Key universe_key(const Seed128& seed);

// DERIVE_NAME (tag 0x01): subsystem/layer key from a frozen name.
Key derive_named(const Key& parent, NameId name);

// DERIVE_CHILD (tag 0x02): spatial/indexed child key. Unused coordinates
// stay zero by convention (e.g. a linear index goes in x).
// level is the octree subdivision level (Cell::w); it occupies its own
// counter bits, so level 0 derives EXACTLY the key the historical 3-coord
// form produced — fixing the level-collision bug (T0017) without moving
// any existing key.
Key derive_child(const Key& parent, KindId kind, std::int64_t x, std::int64_t y = 0,
                 std::int64_t z = 0, std::int32_t level = 0);

// DRAW_CHUNK (tag 0x03): 256 bits of chunk-addressed randomness.
// sub_index is a channel-documented extra index (0 when unused).
det::PhiloxOutput draw_chunk(const Key& layer, ChannelId channel, const ChunkAddr& addr,
                             std::uint64_t sub_index = 0, std::uint64_t draw_index = 0);

// DRAW_TICK (tag 0x04): time-varying randomness.
det::PhiloxOutput draw_tick(const Key& layer, ChannelId channel, std::uint64_t object_id,
                            std::uint64_t tick, std::uint64_t draw_index = 0);

// DRAW_POINT (tag 0x05): lattice/pointwise randomness.
det::PhiloxOutput draw_point(const Key& layer, ChannelId channel, std::int64_t x,
                             std::int64_t y, std::int64_t z);

// A 64-bit key for the sanctioned cheap mixer path (det::mix64), derived
// once per (layer, channel); see core/det/mix.hpp usage rule.
std::uint64_t lattice_key(const Key& layer, ChannelId channel);

}  // namespace inf::core
