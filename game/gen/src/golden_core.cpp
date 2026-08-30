#include "gen/golden.hpp"

#include <array>

#include "core/det/fixed64.hpp"
#include "core/det/mix.hpp"
#include "core/golden.hpp"
#include "core/key.hpp"
#include "gen/names.hpp"


namespace inf::gen {

namespace {

void feed_output(core::GoldenHash& hash, const det::PhiloxOutput& out) {
  for (const std::uint64_t word : out) {
    hash.feed(word);
  }
}

void feed_key(core::GoldenHash& hash, const core::Key& key) {
  hash.feed(key.k0);
  hash.feed(key.k1);
}

}  // namespace

std::uint64_t hash_core_script(const core::Seed128& seed) {
  core::GoldenHash hash;

  // Key tree: one full derivation chain plus name-derived layers.
  const core::Key universe = core::universe_key(seed);
  const core::Key galaxy = core::derive_child(universe, kind::Galaxy, 0, 0, 0);
  const core::Key system = core::derive_child(galaxy, kind::System, 0);
  const core::Key body = core::derive_child(system, kind::Body, 0);
  const core::Key terrain = core::derive_named(body, name::TerrainV1);
  const core::Key provinces = core::derive_named(body, name::ProvincesV1);
  feed_key(hash, galaxy);
  feed_key(hash, system);
  feed_key(hash, body);
  feed_key(hash, terrain);
  feed_key(hash, provinces);

  // Negative-coordinate child derivation.
  feed_key(hash, core::derive_child(universe, kind::Galaxy, -1, -2, -3));

  // DRAW_CHUNK across address extremes and draw indices.
  const std::array<core::ChunkAddr, 4> addrs = {
      core::ChunkAddr{0, 0, 0, 0, 0},
      core::ChunkAddr{5, 31, 0x7FFFFFFFU, 1U, -4},
      core::ChunkAddr{3, 12, 4096U, 4095U, 7},
      core::ChunkAddr{1, 255, 0xFFFFFFFFU, 0xFFFFFFFFU, -32768},
  };
  for (const core::ChunkAddr& addr : addrs) {
    for (std::uint64_t draw = 0; draw < 3; ++draw) {
      feed_output(hash, core::draw_chunk(terrain, channel::Params, addr, 0, draw));
    }
  }

  // DRAW_TICK and DRAW_POINT.
  feed_output(hash, core::draw_tick(provinces, channel::Archetype, 42, 123456789, 0));
  feed_output(hash, core::draw_point(terrain, channel::Lattice, -1000, 2000, -3000));

  // Cheap-mixer path per usage rule: derived_key ^ mix64(coords).
  const std::uint64_t lattice = core::lattice_key(terrain, channel::Lattice);
  for (std::uint64_t coord = 0; coord < 8; ++coord) {
    hash.feed(lattice ^ det::mix64(coord * 0x9E3779B97F4A7C15ULL));
  }

  // Fixed64 op sequence: exercises add/sub/mul/div/sqrt/lerp/floor.
  det::Fixed64 acc = det::Fixed64::from_int(1);
  const det::Fixed64 half = det::Fixed64::from_int(1) / det::Fixed64::from_int(2);
  for (int i = 1; i <= 32; ++i) {
    const det::Fixed64 x = det::Fixed64::from_raw(static_cast<std::int64_t>(
        core::draw_point(terrain, channel::Test, i, 0, 0)[0] >> 16U));
    acc = acc + det::sqrt(det::abs(x)) / det::Fixed64::from_int(i);
    acc = det::lerp(acc, x, half);
    acc = acc - det::floor(acc / det::Fixed64::from_int(3));
    hash.feed(static_cast<std::uint64_t>(acc.raw()));
  }

  return hash.value();
}

std::string hash_core_report() {
  static constexpr std::array<core::Seed128, 4> kSeeds = {
      core::Seed128{0x0000000000000000ULL, 0x0000000000000000ULL},
      core::Seed128{0x0000000000000000ULL, 0x0000000000000001ULL},
      core::Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},  // pi digits
      core::Seed128{0x0000000000000000ULL, 0x00000000DEADBEEFULL},
  };
  std::string report = "hash-core v1\n";
  for (const core::Seed128& seed : kSeeds) {
    const std::uint64_t hash = hash_core_script(seed);
    report += "seed=" + core::to_hex(seed) + " fnv=";
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
      report += kDigits[(hash >> (i * 4)) & 0xFU];
    }
    report += "\n";
  }
  return report;
}

}  // namespace inf::gen
