#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Shared with the structural member endpoints in scene.cpp.
inline const Vec3 kGardenJointPosition{-393.534f, 276.360f, 375.827f};
// One shared floor union for structural slabs/guards and supported dressing.
std::vector<std::vector<Vec2>> garden_supported_floor_plans();
// Explicit runtime material variant; geometry and source kit remain identical.
void add_dry_fern_variant(Scene& scene);
// Eye-level garden and arrival-terrace detail, in metres and y-up.
// The scene owns the structural floors, guards and unobstructed public routes.
void stage_cinematic_gardens(Scene& scene, Rng rng);
}  // namespace cb
