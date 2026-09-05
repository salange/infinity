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
inline constexpr core::NameId MultistarV1{0x505708ce7508c26bULL};     // "multistar/v1"
inline constexpr core::NameId MacroV1{0x44fd8a3c80c8f6b1ULL};         // "macro/v1"
inline constexpr core::NameId TerrainV2{0x48d3cb3103db7f17ULL};       // "terrain/v2"
inline constexpr core::NameId TerrainV3{0x033b3f39bf206fb7ULL};       // "terrain/v3"
inline constexpr core::NameId FeaturesV1{0x3dbb2eed252570fbULL};      // "features/v1"
inline constexpr core::NameId CavesV1{0x637ade26b9efa0daULL};         // "caves/v1"
inline constexpr core::NameId DrainageV1{0x6bdc9ecba8d08bebULL};      // "drainage/v1"
inline constexpr core::NameId GalaxyParamsV1{0xc7450eb1aa86af94ULL};  // "galaxy-params/v1"
inline constexpr core::NameId GalaxySystemsV1{0xe5c73a184da935a5ULL}; // "galaxy-systems/v1"
inline constexpr core::NameId NebulaeV1{0x077f97eae9e3adb4ULL};       // "nebulae/v1"
inline constexpr core::NameId StarClustersV1{0xb59ee6f1f35abb5eULL};  // "star-clusters/v1"
inline constexpr core::NameId GalaxyLayoutV1{0xaa91de0c98bdfc07ULL};  // "galaxy-layout/v1"
// Surface texturing layers (T0019, design/surface-texturing.md).
inline constexpr core::NameId ClimateV1{0x0245292d47427e3dULL};       // "climate/v1"
inline constexpr core::NameId LifeV1{0x319503b0591a5e87ULL};          // "life/v1"
inline constexpr core::NameId BiomeV1{0xfb5bca62da72cb4fULL};         // "biome/v1"
inline constexpr core::NameId MaterialV2{0xbe8c258868132a27ULL};      // "material/v2"
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
inline constexpr core::KindId Feature{15};
inline constexpr core::KindId Cave{16};
inline constexpr core::KindId Nebula{17};
inline constexpr core::KindId StarCluster{18};
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
    {"multistar/v1", name::MultistarV1},
    {"macro/v1", name::MacroV1},
    {"terrain/v2", name::TerrainV2},
    {"terrain/v3", name::TerrainV3},
    {"features/v1", name::FeaturesV1},
    {"caves/v1", name::CavesV1},
    {"drainage/v1", name::DrainageV1},
    {"galaxy-params/v1", name::GalaxyParamsV1},
    {"galaxy-systems/v1", name::GalaxySystemsV1},
    {"nebulae/v1", name::NebulaeV1},
    {"star-clusters/v1", name::StarClustersV1},
    {"galaxy-layout/v1", name::GalaxyLayoutV1},
    {"climate/v1", name::ClimateV1},
    {"life/v1", name::LifeV1},
    {"biome/v1", name::BiomeV1},
    {"material/v2", name::MaterialV2},
    {"clusters/v1", name::ClustersAxis},
    {"galaxies/v1", name::GalaxiesAxis},
    {"systems/v1", name::SystemsAxis},
    {"stars/v1", name::StarsAxis},
    // Engine framework names (frozen in engine core/tree/tree.hpp):
    {"params", core::tree::kParamsName},
    {"children", core::tree::kChildrenName},
};

}  // namespace inf::gen
