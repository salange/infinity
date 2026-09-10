#pragma once
#include "scene.hpp"

namespace cb {
// Guided derivative of the authored climber recipe. The root origin is placed
// just inside the supplied soil surface; local +Z passes over the coping.
void stage_market_canopy_climbers(Scene& scene, Vec3 soil_point, float yaw, float scale);
}
