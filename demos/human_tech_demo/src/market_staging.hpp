#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Furnishes the west market inside the city-owned shell. Metres, Y up.
// Shop floor: 1.2; public east facade: X=-122.5; clear street: X>=-116.5.
void stage_cinematic_market(Scene& scene, Rng rng);
}  // namespace cb
