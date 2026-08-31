#include "gen/golden.hpp"

#include <array>
#include <bit>

#include "core/golden.hpp"
#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"
#include "gen/terrain.hpp"

namespace inf::gen {

namespace {

void feed_real(core::GoldenHash& hash, det::Real value) {
  hash.feed(std::bit_cast<std::uint64_t>(value.to_double()));
}

BodyHandle test_body(const core::Seed128& seed) { return default_body(seed); }

}  // namespace

std::uint64_t hash_planet_script(const core::Seed128& seed, std::uint32_t forced_type) {
  core::GoldenHash hash;
  const BodyHandle body = test_body(seed);
  const PlanetParams planet =
      derive_planet_params(body, static_cast<PlanetType>(forced_type & 3U));

  hash.feed(static_cast<std::uint64_t>(planet.type));
  feed_real(hash, planet.radius_m);
  feed_real(hash, planet.core_radius_m);
  feed_real(hash, planet.gravity);
  feed_real(hash, planet.sea_level_m);
  feed_real(hash, planet.atmosphere_height_m);
  hash.feed(planet.sky_palette);
  hash.feed(planet.cells_per_face);
  hash.feed(planet.palette_id);

  const ProvinceField field(body.entity, planet);
  for (const CellId& cell : field.all_cells()) {
    const ProvinceParams params = field.cell_params(cell);
    hash.feed(static_cast<std::uint64_t>(params.archetype));
    feed_real(hash, params.relief_amplitude_m);
    feed_real(hash, params.base_elevation_m);
    feed_real(hash, params.ruggedness);
    feed_real(hash, params.carving);
    hash.feed(params.palette_shift);
  }

  // Blended samples at fixed integer-lattice directions: face centers,
  // edge midpoints, corners, and asymmetric off-axis points.
  static constexpr std::array<std::array<int, 3>, 18> kDirs = {{
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
      {1, 1, 0}, {1, -1, 0}, {0, 1, 1}, {0, 1, -1}, {1, 0, 1}, {-1, 0, 1},
      {1, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {1, 1, -1},
      {3, 2, 1}, {-2, 5, -3},
  }};
  for (const auto& raw : kDirs) {
    const Dir3 dir = normalize(Dir3{det::Real(static_cast<double>(raw[0])),
                                    det::Real(static_cast<double>(raw[1])),
                                    det::Real(static_cast<double>(raw[2]))});
    const BlendedParams blended = field.sample(dir);
    feed_real(hash, blended.relief_amplitude_m);
    feed_real(hash, blended.base_elevation_m);
    feed_real(hash, blended.ruggedness);
    feed_real(hash, blended.carving);
    hash.feed(blended.dominant.face);
    hash.feed(blended.dominant.ci);
    hash.feed(blended.dominant.cj);
    hash.feed(static_cast<std::uint64_t>(blended.dominant_archetype));
  }
  return hash.value();
}

std::string hash_density_report() {
  static constexpr char kDigits[] = "0123456789abcdef";
  const core::Seed128 seed{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL};
  std::string report = "hash-density v1\n";
  const BodyHandle body = test_body(seed);
  for (std::uint32_t type = 0; type < 4; ++type) {
    const PlanetParams planet = derive_planet_params(body, static_cast<PlanetType>(type));
    const TerrainField field(body.entity, planet);
    static constexpr std::array<core::ChunkAddr, 5> kAddrs = {{
        {0, 11, 1024, 1024, 0},    // surface shell, face 0 center
        {0, 11, 1024, 1024, 5},    // elevated shell (likely air)
        {4, 11, 300, 700, 0},      // another face
        {2, 8, 128, 128, 0},       // coarser lod
        {0, 11, 1024, 1024, -310}, // deep interior, near/below the core
    }};
    for (const core::ChunkAddr& addr : kAddrs) {
      const ChunkGrid grid = ChunkGrid::from_addr(addr, planet.radius_m);
      const std::uint64_t hash = hash_chunk_density(field, grid);
      char line[96];
      std::snprintf(line, sizeof(line), "type=%u face=%u lod=%u i=%u j=%u shell=%d fnv=",
                    type, addr.face, addr.lod, addr.i, addr.j, static_cast<int>(addr.shell));
      report += line;
      for (int i = 15; i >= 0; --i) {
        report += kDigits[(hash >> (i * 4)) & 0xFU];
      }
      report += "\n";
    }
  }
  return report;
}

std::string hash_planet_report() {
  static constexpr std::array<core::Seed128, 3> kSeeds = {
      core::Seed128{0x0000000000000000ULL, 0x0000000000000001ULL},
      core::Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},
      core::Seed128{0x0000000000000000ULL, 0x00000000DEADBEEFULL},
  };
  std::string report = "hash-planet v1\n";
  static constexpr char kDigits[] = "0123456789abcdef";
  for (const core::Seed128& seed : kSeeds) {
    for (std::uint32_t type = 0; type < 4; ++type) {
      const std::uint64_t hash = hash_planet_script(seed, type);
      report += "seed=" + core::to_hex(seed) + " type=";
      report += kDigits[type];
      report += " fnv=";
      for (int i = 15; i >= 0; --i) {
        report += kDigits[(hash >> (i * 4)) & 0xFU];
      }
      report += "\n";
    }
  }
  return report;
}

}  // namespace inf::gen
