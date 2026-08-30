#pragma once

#include <cstdint>

#include "core/key.hpp"
#include "core/tree/tree.hpp"

namespace inf::gen {

// Infinity's frozen id registries over the engine's opaque id types
// (seeding spec section 9). NameId values are the first 8 bytes (LE) of
// MD5(name string) — verified against a reference MD5 in
// tests/test_names.cpp (drift + collision check). All registries are
// append-only; a version bump is a NEW name.

namespace name {
// Layers.
inline constexpr core::NameId PlanetParamsV1{0x620c7e80903e60e7ULL};  // "planet-params/v1"
inline constexpr core::NameId ProvincesV1{0xe930f9d5ff800838ULL};     // "provinces/v1"
inline constexpr core::NameId TerrainV1{0xf9b59960daf8dc2eULL};       // "terrain/v1"
inline constexpr core::NameId MaterialV1{0x81f55ecfb63bb426ULL};      // "material/v1"
inline constexpr core::NameId TestV1{0x5f75b2f645a90e20ULL};          // "test/v1" (tests only)
// System-generation layers (planetary-systems spec section 3).
inline constexpr core::NameId StellarV1{0x06a250b05b53875eULL};       // "stellar/v1"
inline constexpr core::NameId DiskV1{0x0dd643ed81136c54ULL};          // "disk/v1"
inline constexpr core::NameId ArchitectureV1{0xc84a4530235f9e42ULL};  // "architecture/v1"
inline constexpr core::NameId PlanetsV1{0xbe9efd4e11872704ULL};       // "planets/v1"
inline constexpr core::NameId MoonsV1{0xb228c7ac1177455aULL};         // "moons/v1"
inline constexpr core::NameId BeltsV1{0x067c9da641973dd1ULL};         // "belts/v1"
// InfinityTree axes.
inline constexpr core::NameId ClustersAxis{0x7ec0fe4d89436ce4ULL};    // "clusters/v1"
inline constexpr core::NameId GalaxiesAxis{0x033ac9f54f8c9dc4ULL};    // "galaxies/v1"
inline constexpr core::NameId SystemsAxis{0x9dd08cd7977763e7ULL};     // "systems/v1"
inline constexpr core::NameId PlanetsAxis{0xbe9efd4e11872704ULL};     // "planets/v1" (axis)
inline constexpr core::NameId MoonsAxis{0xb228c7ac1177455aULL};       // "moons/v1" (axis)
inline constexpr core::NameId StarsAxis{0x2d0c04caf746a81bULL};       // "stars/v1"
inline constexpr core::NameId BeltsAxis{0x067c9da641973dd1ULL};       // "belts/v1" (axis)
}  // namespace name

namespace kind {
inline constexpr core::KindId Galaxy{1};
inline constexpr core::KindId System{2};
inline constexpr core::KindId Body{3};
inline constexpr core::KindId Province{4};
inline constexpr core::KindId Universe{10};
inline constexpr core::KindId Cluster{11};
inline constexpr core::KindId Star{12};
inline constexpr core::KindId Belt{13};
inline constexpr core::KindId Barycenter{14};
}  // namespace kind

namespace channel {
inline constexpr core::ChannelId Params{1};
inline constexpr core::ChannelId Archetype{2};
inline constexpr core::ChannelId Lattice{3};
inline constexpr core::ChannelId Test{0xFFFFFF};
}  // namespace channel

// The registry as (string, id) pairs for the drift/collision test.
// Note: an axis and a layer may deliberately share a string (e.g.
// "planets/v1"); they can never collide in key space because axis keys
// hang under a node's childrenKey while layer keys hang off the entity
// key directly.
struct NameEntry {
  const char* name;
  core::NameId id;
};

inline constexpr NameEntry kNameRegistry[] = {
    {"planet-params/v1", name::PlanetParamsV1},
    {"provinces/v1", name::ProvincesV1},
    {"terrain/v1", name::TerrainV1},
    {"material/v1", name::MaterialV1},
    {"test/v1", name::TestV1},
    {"stellar/v1", name::StellarV1},
    {"disk/v1", name::DiskV1},
    {"architecture/v1", name::ArchitectureV1},
    {"planets/v1", name::PlanetsV1},
    {"moons/v1", name::MoonsV1},
    {"belts/v1", name::BeltsV1},
    {"clusters/v1", name::ClustersAxis},
    {"galaxies/v1", name::GalaxiesAxis},
    {"systems/v1", name::SystemsAxis},
    {"stars/v1", name::StarsAxis},
    // Engine framework names (frozen in engine core/tree/tree.hpp):
    {"params", core::tree::kParamsName},
    {"children", core::tree::kChildrenName},
};

}  // namespace inf::gen
