#include "gen/golden.hpp"

#include <array>
#include <bit>

#include "core/golden.hpp"
#include "gen/civilization.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

namespace {

void feed_double(core::GoldenHash& hash, double value) {
  hash.feed(std::bit_cast<std::uint64_t>(value));
}

void feed_key(core::GoldenHash& hash, const core::Key& key) {
  hash.feed(key.k0);
  hash.feed(key.k1);
}

void append_hex(std::string* report, std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; --i) {
    *report += kDigits[(value >> (i * 4)) & 0xFU];
  }
}

// WP1: civilization/v1 + the race block around the home system.
std::uint64_t hash_races_script(const core::Seed128& seed) {
  core::GoldenHash hash;
  const core::Key galaxy_key = home_galaxy_key(seed);
  const GalaxyParams galaxy = home_galaxy_params(seed);
  const CivilizationParams civ = derive_civilization(galaxy_key, galaxy, true);
  hash.feed(civ.race_count);
  hash.feed(civ.teeming ? 1U : 0U);
  hash.feed(static_cast<std::uint64_t>(civ.l_civ));
  feed_double(hash, civ.cell_width_ly);
  const RaceRegistry registry(galaxy_key, galaxy, civ);
  const auto& races = registry.races_around(home_system_position_m(galaxy));
  hash.feed(races.size());
  for (const Race& race : races) {
    feed_key(hash, race.key);
    hash.feed(static_cast<std::uint64_t>(race.cell.x));
    hash.feed(static_cast<std::uint64_t>(race.cell.y));
    hash.feed(static_cast<std::uint64_t>(race.cell.z));
    hash.feed(static_cast<std::uint64_t>(race.home_system.x));
    hash.feed(static_cast<std::uint64_t>(race.home_system.y));
    hash.feed(static_cast<std::uint64_t>(race.home_system.z));
    hash.feed(static_cast<std::uint64_t>(race.home_system.level));
    const RaceParams& p = race.params;
    hash.feed(static_cast<std::uint64_t>(p.type));
    hash.feed(p.variant);
    hash.feed(static_cast<std::uint64_t>(p.tech_tier));
    hash.feed(static_cast<std::uint64_t>(p.peak_level));
    hash.feed(static_cast<std::uint64_t>(p.home_level));
    feed_double(hash, p.dome_affinity);
    hash.feed(static_cast<std::uint64_t>(p.t_0.ns_since_epoch));
    feed_double(hash, p.speed_ly_per_year);
    feed_double(hash, p.reproduction);
    feed_double(hash, p.settle_prob);
    feed_double(hash, p.r_max_ly);
    feed_double(hash, p.falloff_ly);
    feed_double(hash, p.anisotropy);
    hash.feed(p.extinct_ever ? 1U : 0U);
    hash.feed(static_cast<std::uint64_t>(p.extinct_ever ? p.t_end.ns_since_epoch : 0));
    hash.feed(race.factions.size());
    for (const FactionParams& f : race.factions) {
      hash.feed(static_cast<std::uint64_t>(f.type));
      hash.feed(static_cast<std::uint64_t>(f.t_start.ns_since_epoch));
      feed_double(hash, f.speed_mul);
      feed_double(hash, f.reproduction_mul);
      feed_double(hash, f.settle_mul);
      hash.feed(f.centres.size());
    }
  }
  return hash.value();
}

}  // namespace

std::string hash_civ_report() {
  static constexpr std::array<core::Seed128, 4> kSeeds = {
      core::Seed128{0, 1},
      core::Seed128{0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL},
      core::Seed128{0, 0xDEADBEEFULL},
      core::Seed128{0, 0x83},
  };
  std::string report = "hash-civ v1\n";
  for (const core::Seed128& seed : kSeeds) {
    report += "races seed=" + core::to_hex(seed) + " fnv=";
    append_hex(&report, hash_races_script(seed));
    report += "\n";
  }
  return report;
}

}  // namespace inf::gen
