#include "core/key.hpp"

namespace inf::core {

namespace {

// Layout tags (seeding spec section 9, NORMATIVE). The low 8 bits of c0.
constexpr std::uint64_t kTagDeriveName = 0x01;
constexpr std::uint64_t kTagDeriveChild = 0x02;
constexpr std::uint64_t kTagDrawChunk = 0x03;
constexpr std::uint64_t kTagDrawTick = 0x04;
constexpr std::uint64_t kTagDrawPoint = 0x05;

det::PhiloxKey as_philox(const Key& key) { return det::PhiloxKey{key.k0, key.k1}; }

Key child_from(const det::PhiloxOutput& out) {
  // Fixed slice, always: output words (0, 1).
  return Key{out[0], out[1]};
}

std::uint64_t as_u64(std::int64_t v) { return static_cast<std::uint64_t>(v); }

std::uint64_t c0_with_channel(std::uint64_t tag, ChannelId channel) {
  return tag | (static_cast<std::uint64_t>(channel) << 8U);
}

}  // namespace

Key universe_key(const Seed128& seed) { return Key{seed.hi, seed.lo}; }

Key derive_named(const Key& parent, NameId name) {
  const det::PhiloxCounter counter{kTagDeriveName, static_cast<std::uint64_t>(name), 0, 0};
  return child_from(det::philox4x64_10(as_philox(parent), counter));
}

Key derive_child(const Key& parent, KindId kind, std::int64_t x, std::int64_t y,
                 std::int64_t z, std::int32_t level) {
  // The octree level (Cell::w) rides in counter bits 48..63: level 0 is
  // bit-identical to the historical 3-coordinate derivation, so adding it
  // moved no existing key — while two octree cells that share x/y/z at
  // different levels no longer collide (T0017 §5.1).
  const std::uint64_t c0 = kTagDeriveChild | (static_cast<std::uint64_t>(kind) << 8U) |
                           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(level))
                            << 48U);
  const det::PhiloxCounter counter{c0, as_u64(x), as_u64(y), as_u64(z)};
  return child_from(det::philox4x64_10(as_philox(parent), counter));
}

det::PhiloxOutput draw_chunk(const Key& layer, ChannelId channel, const ChunkAddr& addr,
                             std::uint64_t sub_index, std::uint64_t draw_index) {
  const std::uint64_t c0 = c0_with_channel(kTagDrawChunk, channel) |
                           (static_cast<std::uint64_t>(addr.face) << 32U) |
                           (static_cast<std::uint64_t>(addr.lod) << 40U) |
                           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(addr.shell))
                            << 48U);
  const std::uint64_t c1 =
      (static_cast<std::uint64_t>(addr.i) << 32U) | static_cast<std::uint64_t>(addr.j);
  const det::PhiloxCounter counter{c0, c1, sub_index, draw_index};
  return det::philox4x64_10(as_philox(layer), counter);
}

det::PhiloxOutput draw_tick(const Key& layer, ChannelId channel, std::uint64_t object_id,
                            std::uint64_t tick, std::uint64_t draw_index) {
  const det::PhiloxCounter counter{c0_with_channel(kTagDrawTick, channel), object_id, tick,
                                   draw_index};
  return det::philox4x64_10(as_philox(layer), counter);
}

det::PhiloxOutput draw_point(const Key& layer, ChannelId channel, std::int64_t x, std::int64_t y,
                             std::int64_t z) {
  const det::PhiloxCounter counter{c0_with_channel(kTagDrawPoint, channel), as_u64(x), as_u64(y),
                                   as_u64(z)};
  return det::philox4x64_10(as_philox(layer), counter);
}

std::uint64_t lattice_key(const Key& layer, ChannelId channel) {
  return draw_point(layer, channel, 0, 0, 0)[2];
}

}  // namespace inf::core
