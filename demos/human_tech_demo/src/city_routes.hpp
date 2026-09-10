#pragma once

#include "scene.hpp"
#include <string>
#include <vector>

namespace cb {
struct RouteWaypoint {
  Vec3 position;
  Vec3 target;
};
struct SceneRoute {
  std::string id;
  bool stairs = false;
  std::vector<RouteWaypoint> waypoints;
};

// Authored walking camera paths through the same scene as the six stills.
// A runtime character controller/navmesh is not supplied by this benchmark.
std::vector<SceneRoute> scene_routes();
} // namespace cb
