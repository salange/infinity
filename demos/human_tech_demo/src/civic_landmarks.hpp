#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Demo-specific landmarks, in metres and Y-up. Their anchor and silhouette
// match the established stage-five civic hall and the 1.9-height oval ring.
void build_cinematic_ring(Scene& scene, Vec2 centre, float base_y,
                          float radius, float facing_radians);
void build_cinematic_dome(Scene& scene, Vec2 centre, float base_y,
                          float half, float yaw, Rng rng);
}  // namespace cb
