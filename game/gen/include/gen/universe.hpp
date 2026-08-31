#pragma once

#include <memory>

#include "core/seed.hpp"
#include "core/tree/tree.hpp"
#include "gen/names.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// Infinity's InfinityTree wiring (T0011): registers the game's kinds and
// axes with the engine framework and provides the standard walk to the
// default body (origin cluster -> galaxy 0 -> system 0 -> planet slot 0),
// which replaces the old hard-coded key chain. Key paths CHANGED with
// this migration (two-seed rule + axis keys) — all goldens regenerated,
// called out in the commit.

core::tree::GeneratorRegistry make_registry();

// The tree for a universe seed (no inceptions by default).
std::unique_ptr<core::tree::InfinityTree> make_tree(const core::Seed128& seed);

// Address of the default body: clusters(0,0,0)/galaxies(0)/systems(0)/
// planets(slot 0).
core::tree::Address default_body_address();
core::tree::Address default_system_address();

// Convenience: materialize the default body and return its keys.
struct BodyHandle {
  core::Key entity;  // layer keys (terrain/provinces/...) derive from this
  core::Key params;  // planet-params draw from this (two-seed rule)
};
BodyHandle default_body(const core::Seed128& seed);

// The default system's entity key (system layers stellar/disk/... hang
// off it — feed to generate_system) and the body keys for one of its
// planet slots. Used by the client to live on a system-generated world
// (map mode, T0013).
core::Key default_system_key(const core::Seed128& seed);
BodyHandle body_for_slot(const core::Seed128& seed, int slot);
// A moon of the default system's planet at `slot` (moons axis under the
// planet node) — moons are full bodies with their own layer keys.
BodyHandle body_for_moon(const core::Seed128& seed, int slot, int moon_index);

// Convenience: planet params from a BodyHandle (macro reads the entity
// key, the parameter draws read the params key — two-seed rule).
PlanetParams derive_planet_params(const BodyHandle& body,
                                  std::optional<PlanetType> forced_type = std::nullopt);

// --- octree system addressing (T0017 WP6) --------------------------------
// A star system anywhere in the home galaxy is addressed by its octree
// cell; {0,0,0,0} is the DEFAULT (home) system with its historical key.
struct SystemCell {
  std::int64_t x{0}, y{0}, z{0};
  std::int32_t level{0};
  bool is_home() const { return x == 0 && y == 0 && z == 0 && level == 0; }
  friend bool operator==(const SystemCell&, const SystemCell&) = default;
};
core::tree::Address system_address_for(const SystemCell& cell);
core::Key system_key_for(const core::Seed128& seed, const SystemCell& cell);
BodyHandle body_for_system_slot(const core::Seed128& seed, const SystemCell& cell,
                                int slot);
BodyHandle body_for_system_moon(const core::Seed128& seed, const SystemCell& cell,
                                int slot, int moon_index);
// The home galaxy's entity key (galaxy-params/v1 and the octree hang off
// it).
core::Key home_galaxy_key(const core::Seed128& seed);

}  // namespace inf::gen
