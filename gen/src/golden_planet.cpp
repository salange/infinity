#include "gen/golden.hpp"

#include <array>
#include <bit>

#include "core/golden.hpp"
#include "core/key.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"

namespace inf::gen {

namespace {

void feed_real(core::GoldenHash& hash, det::Real value) {
  hash.feed(std::bit_cast<std::uint64_t>(value.to_double()));
}

core::Key test_body_key(const core::Seed128& seed) {
  const core::Key universe = core::universe_key(seed);
  const core::Key galaxy = core::derive_child(universe, core::Kind::Galaxy, 0, 0, 0);
  const core::Key system = core::derive_child(galaxy, core::Kind::System, 0);
  return core::derive_child(system, core::Kind::Body, 0);
}

}  // namespace

std::uint64_t hash_planet_script(const core::Seed128& seed, std::uint32_t forced_type) {
  core::GoldenHash hash;
  const core::Key body = test_body_key(seed);
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

  const ProvinceField field(body, planet);
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
