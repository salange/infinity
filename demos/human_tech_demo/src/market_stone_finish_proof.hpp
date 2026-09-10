#pragma once

#include "street_forecourt.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cb {
inline constexpr const char *kMarketStoneProofShot = "street";
namespace market_stone_proof_detail {
inline std::vector<Vec2> intersect(std::vector<Vec2> subject,
                                   const std::vector<Vec2> &clip) {
  const float sign = plan_area(clip) > 0 ? 1.f : -1.f;
  for (std::size_t i = 0; i < clip.size() && subject.size() >= 3; ++i) {
    const Vec2 a = clip[i], d = clip[(i + 1) % clip.size()] - a;
    subject = clip_halfplane(subject, a, Vec2{-d.y, d.x} * sign);
  }
  return subject;
}
inline std::vector<std::vector<Vec2>> subtract(std::vector<Vec2> subject,
                                               const std::vector<Vec2> &cut) {
  std::vector<std::vector<Vec2>> result;
  const float sign = plan_area(cut) > 0 ? 1.f : -1.f;
  for (std::size_t i = 0; i < cut.size() && subject.size() >= 3; ++i) {
    const Vec2 a = cut[i], d = cut[(i + 1) % cut.size()] - a,
               normal = Vec2{-d.y, d.x} * sign;
    auto outside = clip_halfplane(subject, a, normal * -1.f);
    if (outside.size() >= 3 && std::abs(plan_area(outside)) > .005f)
      result.push_back(std::move(outside));
    subject = clip_halfplane(subject, a, normal);
  }
  return result;
}
} // namespace market_stone_proof_detail

// Pair this fixed receiver with the correspondingly named temporary shader
// variant. All candidates use the same actual Street tile geometry and maps.
// The entire scene contains only the receiver and its original recessed joints.
inline Scene generate_market_stone_finish_proof(const std::string &name,
                                                bool reviewed_maps) {
  if (!reviewed_maps)
    throw std::runtime_error(
        "Market stone proof requires decoded reviewed surface maps");
  const bool polished = name == "reflection-proof-market-stone-polished";
  const bool honed62 = name == "reflection-proof-market-stone-honed62";
  const bool honed72 = name == "reflection-proof-market-stone-honed72";
  if (!polished && !honed62 && !honed72)
    throw std::runtime_error("Unknown market finish proof");
  Scene scene;
  scene.materials = make_materials();
  scene.reviewed_material_maps = true;
  const auto stone = market_paving_material(scene);
  // Retain the pre-honing scalar for the historical polished control. Pair it
  // with the frozen original shader response; the candidate shader interprets
  // its scalar as the explicit authored dry finish.
  scene.materials[stone].roughness = polished ? .76f : honed62 ? .62f : .72f;
  MaterialDesc joints = scene.materials[stone];
  joints.name = "market finish proof original recessed joints";
  joints.base_color = {.035f, .038f, .034f};
  joints.albedo_set = "";
  joints.roughness = .85f;
  joints.flags = kMatPlanarXZ;
  const auto grout = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(joints);
  const std::vector<Vec2> promenade{
      {-121, 163}, {-84, 163}, {-35, 84}, {-111, 84}};
  const auto patch = market_stone_proof_detail::intersect(
      plan_rect(14.55f, 20, {-106.55f, 140}), promenade);
  const auto drain = plan_rect(.4f, .85f, {-106.5f, 131});
  for (const auto &p : market_stone_proof_detail::subtract(patch, drain))
    Emit(&scene.opaque, grout).polygon(p, 1.2005f, true);
  Rng rng = root_rng("83").child(792).child(90);
  for (int row = 0; row < 26; ++row)
    for (int col = 0; col < 16; ++col) {
      const float z = 120 + row * 1.6f,
                  x = -123 + col * 2.25f + (row % 3) * .75f;
      const auto tile = market_stone_proof_detail::intersect(
          plan_rect(1.120f, .795f, {x, z}), patch);
      if (tile.size() < 3 || std::abs(plan_area(tile)) < .018f)
        continue;
      for (const auto &p : market_stone_proof_detail::subtract(tile, drain)) {
        Emit slab(&scene.opaque, stone);
        slab.element_random = rng.next();
        slab.polygon(p, 1.218f, true);
        slab.wall(p, 1.2f, 1.218f, true);
      }
    }
  if (!shot_camera(kMarketStoneProofShot, scene.camera_position,
                   scene.camera_target))
    throw std::runtime_error(
        "Market finish proof requires canonical Street camera");
  scene.camera_fov_degrees = shot_fov_degrees(kMarketStoneProofShot);
  scene.city_radius = 80;
  scene.city_size = name;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
