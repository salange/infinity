#include "core/golden.hpp"

#include <array>

#include "core/det/fixed64.hpp"
#include "core/det/mix.hpp"
#include "core/key.hpp"
#include "core/seed.hpp"

namespace inf::core {

namespace {

void feed_output(GoldenHash& hash, const det::PhiloxOutput& out) {
  for (const std::uint64_t word : out) {
    hash.feed(word);
  }
}

void feed_key(GoldenHash& hash, const Key& key) {
  hash.feed(key.k0);
  hash.feed(key.k1);
}

}  // namespace

std::uint64_t hash_core_script(const Seed128& seed) {
  GoldenHash hash;

  // Key tree: one full derivation chain plus name-derived layers.
  const Key universe = universe_key(seed);
  const Key galaxy = derive_child(universe, Kind::Galaxy, 0, 0, 0);
  const Key system = derive_child(galaxy, Kind::System, 0);
  const Key body = derive_child(system, Kind::Body, 0);
  const Key terrain = derive_named(body, NameId::TerrainV1);
  const Key provinces = derive_named(body, NameId::ProvincesV1);
  feed_key(hash, galaxy);
  feed_key(hash, system);
  feed_key(hash, body);
  feed_key(hash, terrain);
  feed_key(hash, provinces);

  // Negative-coordinate child derivation.
  feed_key(hash, derive_child(universe, Kind::Galaxy, -1, -2, -3));

  // DRAW_CHUNK across address extremes and draw indices.
  const std::array<ChunkAddr, 4> addrs = {
      ChunkAddr{0, 0, 0, 0, 0},
      ChunkAddr{5, 31, 0x7FFFFFFFU, 1U, -4},
      ChunkAddr{3, 12, 4096U, 4095U, 7},
      ChunkAddr{1, 255, 0xFFFFFFFFU, 0xFFFFFFFFU, -32768},
  };
  for (const ChunkAddr& addr : addrs) {
    for (std::uint64_t draw = 0; draw < 3; ++draw) {
      feed_output(hash, draw_chunk(terrain, Channel::Params, addr, 0, draw));
    }
  }

  // DRAW_TICK and DRAW_POINT.
  feed_output(hash, draw_tick(provinces, Channel::Archetype, 42, 123456789, 0));
  feed_output(hash, draw_point(terrain, Channel::Lattice, -1000, 2000, -3000));

  // Cheap-mixer path per usage rule: derived_key ^ mix64(coords).
  const std::uint64_t lattice = lattice_key(terrain, Channel::Lattice);
  for (std::uint64_t coord = 0; coord < 8; ++coord) {
    hash.feed(lattice ^ det::mix64(coord * 0x9E3779B97F4A7C15ULL));
  }

  // Fixed64 op sequence: exercises add/sub/mul/div/sqrt/lerp/floor.
  det::Fixed64 acc = det::Fixed64::from_int(1);
  const det::Fixed64 half = det::Fixed64::from_int(1) / det::Fixed64::from_int(2);
  for (int i = 1; i <= 32; ++i) {
    const det::Fixed64 x = det::Fixed64::from_raw(static_cast<std::int64_t>(
        draw_point(terrain, Channel::Test, i, 0, 0)[0] >> 16U));
    acc = acc + det::sqrt(det::abs(x)) / det::Fixed64::from_int(i);
    acc = det::lerp(acc, x, half);
    acc = acc - det::floor(acc / det::Fixed64::from_int(3));
    hash.feed(static_cast<std::uint64_t>(acc.raw()));
  }

  return hash.value();
}

std::string hash_core_report() {
  static constexpr std::array<Seed128, 4> kSeeds = {
      Seed128{0x0000000000000000ULL, 0x0000000000000000ULL},
      Seed128{0x0000000000000000ULL, 0x0000000000000001ULL},
      Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},  // pi digits
      Seed128{0x0000000000000000ULL, 0x00000000DEADBEEFULL},
  };
  std::string report = "hash-core v1\n";
  for (const Seed128& seed : kSeeds) {
    const std::uint64_t hash = hash_core_script(seed);
    report += "seed=" + to_hex(seed) + " fnv=";
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
      report += kDigits[(hash >> (i * 4)) & 0xFU];
    }
    report += "\n";
  }
  return report;
}

}  // namespace inf::core
