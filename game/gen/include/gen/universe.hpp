#pragma once

#include <memory>

#include "core/seed.hpp"
#include "core/tree/tree.hpp"
#include "gen/names.hpp"

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

}  // namespace inf::gen
