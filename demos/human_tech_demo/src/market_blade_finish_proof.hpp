#pragma once

#include "market_structure.hpp"

#include <stdexcept>
#include <string>

namespace cb {
inline constexpr const char *kMarketBladeProofShot = "street";

// The production formed panels, backing, bearings and upper outriggers retain
// their world coordinates. Only the named blade finish changes between cases.
// The small fixture omits the building, its shadows and all occupied rooms.
inline Scene generate_market_blade_finish_proof(const std::string &name) {
  const bool baseline = name == "reflection-proof-market-blade-baseline";
  const bool honed = name == "reflection-proof-market-blade-honed84";
  if (!baseline && !honed)
    throw std::runtime_error("Unknown market blade finish proof");
  Scene scene;
  scene.materials = make_materials();
  // These are the production palette's two used overrides. The CPU fixture
  // audit compares them with palette()/lattice_ceramic() from scene.cpp.
  auto &bronze = scene.materials[M_BRONZE];
  bronze.base_color = {.43f, .235f, .095f};
  bronze.roughness = .27f;
  bronze.metallic = .86f;
  bronze.normal_strength = .2f;
  MaterialDesc ceramic = scene.materials[M_WHITE_METAL];
  ceramic.name = "satin ivory ceramic lattice";
  ceramic.base_color = {.71f, .75f, .72f};
  ceramic.roughness = .29f;
  ceramic.normal_strength = .085f;
  ceramic.metallic = 0;
  ceramic.albedo_set = "concrete_white";
  ceramic.uv_scale = 1.8f;
  ceramic.flags = kMatTriplanar;
  const auto source = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(ceramic);
  build_market_structure(scene, source);
  bool found = false;
  for (auto &material : scene.materials)
    if (material.name == "honed ivory market structural cladding") {
      // Retain the captured predecessor after the local production finish changes.
      material.roughness = honed ? .84f : .48f;
      found = true;
    }
  if (!found)
    throw std::runtime_error("Production blade material missing");
  if (!shot_camera(kMarketBladeProofShot, scene.camera_position,
                   scene.camera_target))
    throw std::runtime_error("Blade proof requires canonical Street camera");
  scene.camera_fov_degrees = shot_fov_degrees(kMarketBladeProofShot);
  scene.city_size = name;
  scene.city_radius = 80;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
