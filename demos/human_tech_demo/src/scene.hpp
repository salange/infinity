#pragma once
// Fixed visual set, composed with the shared cosmetic city geometry kit.
#include <string>
#include "city/materials.hpp"
#include "city/scene.hpp"
namespace cb {
using namespace inf::city;
struct SceneParams {
  std::string shot{"aerial"};
  std::string seed{"83"};
  bool far_patterns{true};
  int detail{2};
  std::string asset;
};
bool shot_camera(const std::string& shot, Vec3& position, Vec3& target);
Scene generate_scene(const SceneParams& params);
}  // namespace cb
