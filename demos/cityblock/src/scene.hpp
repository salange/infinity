#pragma once
// The demo's scene: the shared city scene type plus the demo's own
// generation parameters (seed, size, showcase, context rings).
#include <string>

#include "city/materials.hpp"
#include "city/scene.hpp"

namespace cb {
using namespace inf::city;

struct SceneParams {
  std::string seed{"83"};
  bool context_buildings{true};
  int context_rings{2};  // rings of context towers around the block
  int context_detail{-1};  // -1 = by ring (1 near, 0 far); 0..2 forces one level
  int size{-1};  // -1 = from the seed; 0 small, 1 medium, 2 large, 3 metropolis
  bool showcase{false};  // asset-catalog scene: every tower family and the ground kit side by side
  int detail{2};  // 0 coarse … 2 full
};

Scene generate_scene(const SceneParams& params);

}  // namespace cb
