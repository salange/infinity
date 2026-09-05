#include "gen/golden.hpp"

#include <array>
#include <bit>
#include <cmath>

#include "core/golden.hpp"
#include "core/key.hpp"
#include "gen/caves.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"
#include "gen/provinces.hpp"
#include "gen/material.hpp"
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
  std::string report = "hash-density v2\n";
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
    const auto emit = [&](const core::ChunkAddr& addr, const char* tag) {
      const ChunkGrid grid = ChunkGrid::from_addr(addr, planet.radius_m);
      const std::uint64_t hash = hash_chunk_density(field, grid);
      char line[112];
      std::snprintf(line, sizeof(line), "type=%u%s face=%u lod=%u i=%u j=%u shell=%d fnv=",
                    type, tag, addr.face, addr.lod, addr.i, addr.j,
                    static_cast<int>(addr.shell));
      report += line;
      for (int i = 15; i >= 0; --i) {
        report += kDigits[(hash >> (i * 4)) & 0xFU];
      }
      report += "\n";
    };
    for (const core::ChunkAddr& addr : kAddrs) {
      emit(addr, "");
    }
    // caves/v1 (T0015 WP7): chunks centred on the first hosted cave cell
    // of face 0, straddling its anchor depth — two clients must agree on
    // cave geometry bit-exactly.
    const CaveField& caves = field.caves();
    bool found = false;
    for (std::uint32_t ci = 0; ci < caves.cells_per_face() && !found; ++ci) {
      for (std::uint32_t cj = 0; cj < caves.cells_per_face() && !found; ++cj) {
        const CellId cell{0, ci, cj};
        if (!caves.hosted(cell)) {
          continue;
        }
        found = true;
        const Dir3 dir = caves.anchor_dir(cell);
        const FaceUV uv = dir_to_face_uv(dir);
        constexpr std::uint8_t kLod = 12;
        const double cells = static_cast<double>(1U << kLod);
        const auto to_index = [&](det::Real coord) {
          double scaled = (coord.to_double() + 1.0) * 0.5 * cells;
          if (scaled < 0.0) scaled = 0.0;
          if (scaled > cells - 1.0) scaled = cells - 1.0;
          return static_cast<std::uint32_t>(scaled);
        };
        const double thickness = 2.0 * planet.radius_m.to_double() / cells;
        const double elevation = field.elevation_m(dir).to_double();
        const int mid = static_cast<int>(std::floor(elevation / thickness));
        for (int shell = mid - 2; shell <= mid; ++shell) {
          emit(core::ChunkAddr{uv.face, kLod, to_index(uv.u), to_index(uv.v),
                               static_cast<std::int16_t>(shell)},
               " cave");
        }
      }
    }
    if (!found) {
      char line[32];
      std::snprintf(line, sizeof(line), "type=%u cave=none\n", type);
      report += line;
    }
    // drainage/v1 (T0015 WP6): fingerprint of the whole river forest —
    // vertex classification, parent links, Strahler orders and
    // representative directions. Two clients must grow the identical
    // network from the seed alone.
    if (field.drainage().enabled()) {
      core::GoldenHash drain_hash;
      for (const DrainageField::Vertex& vertex : field.drainage().vertices()) {
        drain_hash.feed(static_cast<std::uint64_t>(
            static_cast<std::int64_t>(vertex.parent)));
        drain_hash.feed(vertex.order);
        drain_hash.feed(vertex.sea ? 1U : 0U);
        feed_real(drain_hash, vertex.dir.x);
        feed_real(drain_hash, vertex.dir.y);
        feed_real(drain_hash, vertex.dir.z);
      }
      char line[48];
      std::snprintf(line, sizeof(line), "type=%u drainage fnv=", type);
      report += line;
      const std::uint64_t hash = drain_hash.value();
      for (int i = 15; i >= 0; --i) {
        report += kDigits[(hash >> (i * 4)) & 0xFU];
      }
      report += "\n";
    } else {
      char line[40];
      std::snprintf(line, sizeof(line), "type=%u drainage=none\n", type);
      report += line;
    }
  }
  return report;
}

// T0019: the surface layers (climate/v1, life/v1, biome/v1, material/v2)
// at the same fixed directions. A separate script so the pre-existing
// planet section keeps its hashes byte for byte.
std::uint64_t hash_surface_script(const core::Seed128& seed, std::uint32_t forced_type) {
  core::GoldenHash hash;
  const BodyHandle body = test_body(seed);
  const PlanetParams planet =
      derive_planet_params(body, static_cast<PlanetType>(forced_type & 3U));
  feed_real(hash, planet.star_temperature_k);
  feed_real(hash, planet.star_age_gyr);
  feed_real(hash, planet.flux_rel);
  feed_real(hash, planet.obliquity_rad);
  feed_real(hash, planet.pressure_rel);
  hash.feed(planet.tidally_locked ? 1U : 0U);
  const TerrainField field(body.entity, planet);
  const LifeParams& life = field.life();
  hash.feed(life.habitable ? 1U : 0U);
  hash.feed(life.occupied ? 1U : 0U);
  hash.feed(static_cast<std::uint64_t>(life.chemistry));
  hash.feed(static_cast<std::uint64_t>(life.stage));
  hash.feed(life.variant);
  static constexpr std::array<std::array<int, 3>, 18> kDirs = {{
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
      {1, 1, 0}, {1, -1, 0}, {0, 1, 1}, {0, 1, -1}, {1, 0, 1}, {-1, 0, 1},
      {1, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {1, 1, -1},
      {3, 2, 1}, {-2, 5, -3},
  }};
  const double radius = planet.radius_m.to_double();
  TerrainField::ParamCache cache;
  for (const auto& d : kDirs) {
    const double len = std::sqrt(static_cast<double>(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    const double dx = d[0] / len;
    const double dy = d[1] / len;
    const double dz = d[2] / len;
    const double elevation =
        field.elevation_m(Dir3{det::Real(dx), det::Real(dy), det::Real(dz)}).to_double();
    const double r = radius + elevation;
    const MaterialInputs in = field.material_inputs(dx * r, dy * r, dz * r, dx, dy, dz, &cache);
    hash.feed(std::bit_cast<std::uint64_t>(in.climate.temperature_k));
    hash.feed(std::bit_cast<std::uint64_t>(in.climate.humidity));
    hash.feed(std::bit_cast<std::uint64_t>(in.climate.t01));
    hash.feed(std::bit_cast<std::uint64_t>(in.climate.h01));
    hash.feed(static_cast<std::uint64_t>(in.biome.primary));
    const VertexMaterial vm = field.material().classify(in);
    hash.feed(static_cast<std::uint64_t>(vm.mat0));
    hash.feed(static_cast<std::uint64_t>(vm.mat1));
    hash.feed(std::bit_cast<std::uint32_t>(vm.blend));
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
  // Surface layers (T0019): appended so the planet lines above are
  // byte-identical to the pre-T0019 goldens.
  for (const core::Seed128& seed : kSeeds) {
    for (std::uint32_t type = 0; type < 4; ++type) {
      const std::uint64_t hash = hash_surface_script(seed, type);
      report += "surface seed=" + core::to_hex(seed) + " type=";
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
