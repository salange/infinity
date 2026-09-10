#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Continuous curved hall, separate rounded terminal roof, permanent platforms
// and guideway fittings on the city_transit supported deck. No vehicle meshes.
void stage_transit_concourse(Scene &scene, Rng rng);
} // namespace cb
