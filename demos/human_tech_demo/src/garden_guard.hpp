#pragma once
#include "scene.hpp"

#include <utility>
#include <vector>

namespace cb {
using GardenGuardSpan = std::pair<Vec3, Vec3>;
struct GardenGuardPlan {
  std::vector<GardenGuardSpan> spans;
  std::vector<Vec3> posts;
};
// The scene supplies the exact exposed floor-union edges. Tessellation affects
// the curved glazing only; support spacing follows continuous world distance.
GardenGuardPlan plan_garden_guard(const std::vector<GardenGuardSpan> &edges);
void build_garden_guard(Scene &scene,
                        const std::vector<GardenGuardSpan> &edges);
} // namespace cb
