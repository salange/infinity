#pragma once

#include <memory>

#include "core/seed.hpp"
#include "core/tree/tree.hpp"
#include "gen/galaxy.hpp"
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
// The home cluster's entity key (galaxy count/positions hang off it —
// T0018 WP4 renders the neighbour galaxies from here).
core::Key home_cluster_key(const core::Seed128& seed);
// The home galaxy's params, with the morphology FORCED to Barred (T0018,
// 2026-09-01): every seed's starting sky is a grand-design barred spiral
// — arms, dust rift, nebulae — instead of gambling the game's signature
// vista on a 60% roll. Every other galaxy draws its type freely. Still a
// pure function of the seed; all sky/octree consumers of the home galaxy
// must go through this, never through derive_galaxy_params directly.
GalaxyParams home_galaxy_params(const core::Seed128& seed);

// --- cluster and universe levels (T0017 WP5) ------------------------------
// universe.clusters is a CellGrid3D with this cell size (from the 1:10
// interstellar scale: ~32 M game-ly, a typical cluster spacing). The
// home cluster is cell (0,0,0), centred on the origin.
inline constexpr double kClusterCellM = 3.0e22;
// Cluster cube edge within which its galaxies scatter.
inline constexpr double kClusterSizeM = 2.4e22;

// Galaxies per cluster (10-1000, drawn from galaxy-layout/v1 off the
// cluster's entity key) and their positions in the cluster frame. Galaxy
// 0 sits at the cluster origin — for the home cluster that keeps the
// playable galaxy exactly where it always was.
std::uint32_t galaxy_count_in_cluster(const core::Key& cluster_entity_key);
Dir3 galaxy_position_in_cluster(const core::Key& cluster_entity_key, std::uint32_t index);
// Entity key of any galaxy in any cluster — external galaxies need only
// galaxy-params/v1 from this to render as impostors (T0018 WP5).
core::Key galaxy_key_in_cluster(const core::Seed128& seed, std::int64_t cx,
                                std::int64_t cy, std::int64_t cz, std::uint32_t index);

}  // namespace inf::gen
