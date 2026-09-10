#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Subtract these exact convex openings from the real court slab before staging.
// The basin bottom stays above the existing terrain; court level is 1.2 m.
std::vector<std::vector<Vec2>> west_civic_floor_openings();
void stage_civic_forecourt(Scene& scene, Rng rng);
}  // namespace cb
