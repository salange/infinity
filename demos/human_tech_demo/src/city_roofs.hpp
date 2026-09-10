#pragma once
#include "city/math.hpp"
#include "city/rng.hpp"
#include <vector>

namespace cb {
// Authored horizontal surfaces are kept until all neighbouring structures
// exist, so occupied roof programs use their real remaining area.
struct AuthoredRoof {
  std::vector<inf::city::Vec2> polygon;
  float y;
  int style;
  inf::city::Rng rng;
};
struct RoofObstruction {
  std::vector<inf::city::Vec2> polygon;
  float bottom, top;
};
} // namespace cb
