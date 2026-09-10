#pragma once

#include "scene.hpp"

#include <cmath>

namespace cb {
// The solar-control screen follows structural columns 14..21 of the 22-bay
// tower. The west loggias stay outside their existing occupied inner curtain.
// Weather panels use the same boundaries (24..80 on the northern half grid).
inline constexpr float kGardenGraphiteBegin = 14.f * 2.f * kPi / 22.f;
inline constexpr float kGardenGraphiteEnd = 21.f * 2.f * kPi / 22.f;

inline bool garden_graphite_sector(float angle) {
  angle = std::fmod(angle, 2.f * kPi);
  if (angle < 0) angle += 2.f * kPi;
  return angle >= kGardenGraphiteBegin && angle < kGardenGraphiteEnd;
}

inline bool garden_graphite_sector(Vec3 position, Vec2 centre) {
  return garden_graphite_sector(std::atan2(position.z - centre.y, position.x - centre.x));
}
}  // namespace cb
