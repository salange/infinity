#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
enum class RoofUse { ResidentialGarden, WorkTerrace, CivicRoof, ServiceRoof, MarketCourt };

// The supplied surface is an actual horizontal roof in metres, not a bounding
// box. Exclusions are occupied upper floors, existing beds and access reserves.
// Every complete assembly is contained, including foliage and canopy overhangs.
// A 1.5 m border around the roof/exclusions and between assemblies stays clear.
// The city owns floor construction, perimeter protection and vertical access.
void stage_occupied_roof(Scene& scene, const std::vector<Vec2>& roof, float floor_y,
                         const std::vector<std::vector<Vec2>>& exclusions,
                         RoofUse use, Rng rng);
}  // namespace cb
