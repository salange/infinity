#pragma once
// Minimal paired scenes whose changed emitter is outside the direct camera.
// They deliberately share geometry, camera, exposure and material factors.
#include "civic_landmarks.hpp"
#include "scene.hpp"
#include "window_filter_proof.hpp"
#include "window_oblique_proof.hpp"
#include "practical_light_proof.hpp"
#include <stdexcept>
#include <string>
namespace cb {
inline Scene generate_renderer_proof(const std::string &name,
                                     const std::string &asset_kit = {}) {
  if (name.starts_with("indirect-proof-practical-") ||
      name.starts_with("reflection-proof-practical-"))
    return generate_practical_light_proof(name);
  if (name == "indirect-proof-window-filter-on")
    return generate_window_filter_proof();
  if (name == "indirect-proof-window-oblique-on")
    return generate_window_oblique_proof();
  const bool reflection = name.starts_with("reflection-proof-");
  if (!reflection && !name.starts_with("indirect-proof-"))
    throw std::runtime_error("unknown renderer proof");
  const bool enabled = name.ends_with("-on");
  Scene scene;
  scene.materials = make_materials();
  auto &white = scene.materials[M_WHITE_METAL];
  white.base_color = {.82f, .82f, .82f};
  white.metallic = 0;
  white.roughness = .85f;
  white.flags = 0;
  white.albedo_set = "flat";
  auto &emitter = scene.materials[M_SIGN];
  emitter.base_color = {.4f, .012f, .018f};
  emitter.tint2 = {1, .015f, .025f};
  emitter.emissive = enabled ? 12.f : 0.f;
  emitter.flags = kMatEmissive;
  emitter.albedo_set = "flat";
  emitter.metallic = 0;
  if (name.starts_with("reflection-proof-moon-")) {
    const std::string shot =
        name.find("landing") != std::string::npos ? "landing" : "galaxy";
    shot_camera(shot, scene.camera_position, scene.camera_target);
    scene.camera_fov_degrees = shot_fov_degrees(shot);
    Emit(&scene.opaque, M_WHITE_METAL)
        .box(scene.camera_position - Vec3{0, 20, 0}, {1, .1f, 1});
    scene.city_radius = 20;
    scene.city_size = name;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("indirect-proof-foliage-")) {
    if (asset_kit.empty())
      throw std::runtime_error("foliage proof requires the authored asset kit");
    scene.asset_library = load_asset_library(
        asset_kit, static_cast<std::uint32_t>(scene.materials.size()));
    scene.materials.insert(scene.materials.end(),
                           scene.asset_library.materials.begin(),
                           scene.asset_library.materials.end());
    if (name == "indirect-proof-foliage-dry-on") {
      auto &fern =
          scene.asset_library
              .resources[asset_resource(scene.asset_library, "fern_arching")]
              .mesh;
      const auto material_count = scene.materials.size();
      for (std::uint32_t old = 0; old < material_count; ++old) {
        if (scene.materials[old].name != "leaf_middle" &&
            scene.materials[old].name != "leaf_sunlit")
          continue;
        auto dry = scene.materials[old];
        dry.name += "_dry_fern_proof";
        dry.roughness = .67f;
        const auto replacement =
            static_cast<std::uint32_t>(scene.materials.size());
        scene.materials.push_back(std::move(dry));
        for (auto &vertex : fern.vertices)
          if (vertex.material == old)
            vertex.material = replacement;
      }
    }
    add_asset_instance(scene, "fern_arching", {-.1f, 0, 0}, .2f, .72f);
    add_asset_instance(scene, "shrub_flowering", {-1.4f, 0, -1.f}, -.35f, .36f);
    auto &floor = scene.materials[M_TERRAZZO];
    floor.base_color = {.25f, .25f, .25f};
    floor.flags = 0;
    floor.roughness = .85f;
    floor.metallic = 0;
    floor.albedo_set = "";
    Emit(&scene.opaque, M_TERRAZZO).box({0, -.05f, 0}, {4, .05f, 3});
    Vec3 position, target;
    shot_camera("garden", position, target);
    // The actual foreground bed sits about six metres ahead and 1.6 metres
    // below the garden eye. Preserve this incidence angle and screen position,
    // instead of centring a plant at the distant skyline's grazing angle.
    scene.camera_position = {0, 1.61f, 6.f};
    scene.camera_target =
        scene.camera_position + normalize(target - position) * 4.3f;
    scene.camera_fov_degrees = 54;
    scene.city_radius = 20;
    scene.city_size = name;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-ocean-")) {
    auto &water = scene.materials[M_WATER];
    water.base_color = {.028f, .065f, .073f};
    water.flags = kMatPlanarXZ | 64u;
    water.metallic = 0;
    water.roughness = .14f;
    water.albedo_set = "";
    Emit(&scene.opaque, M_WATER)
        .box(enabled ? Vec3{0, -.2f, 0} : Vec3{0, -.2f, -2000},
             enabled ? Vec3{150000, .2f, 150000} : Vec3{12000, .2f, 14000});
    shot_camera("galaxy", scene.camera_position, scene.camera_target);
    scene.camera_fov_degrees = shot_fov_degrees("galaxy");
    scene.city_radius = 10000;
    scene.city_size = name;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-dome-")) {
    build_cinematic_dome(scene, {0, 0}, 0, 42, 0, root_rng("83"));
    // Preserve every vertex, triangle, material and draw range; only reverse
    // the submission order of glass triangles. Nearest-pane depth must make
    // the resulting visible shell independent of this order.
    if (enabled) {
      std::vector<std::size_t> slots;
      std::vector<std::array<std::uint32_t, 3>> triangles;
      auto &mesh = scene.opaque;
      for (std::size_t at = 0; at < mesh.indices.size(); at += 3)
        if (scene.materials[mesh.vertices[mesh.indices[at]].material].flags &
            128u) {
          slots.push_back(at);
          triangles.push_back(
              {mesh.indices[at], mesh.indices[at + 1], mesh.indices[at + 2]});
        }
      for (std::size_t index = 0; index < slots.size(); ++index)
        for (int corner = 0; corner < 3; ++corner)
          mesh.indices[slots[index] + corner] =
              triangles[triangles.size() - index - 1][corner];
    }
    scene.camera_position = {105, 75, 130};
    scene.camera_target = {0, 27, 0};
    scene.camera_fov_degrees = 40;
    scene.city_radius = 110;
    scene.city_size = name;
    scene.has_lighting_override = true;
    scene.sun_direction = {.3f, .8f, .2f};
    scene.sun_irradiance = {2.8f, 2.4f, 1.9f};
    scene.lighting_exposure = 1;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-coating-")) {
    const std::array<Vec3, 3> transmission{
        {{.94f, .97f, .98f}, {.62f, .76f, .82f}, {.78f, .58f, .36f}}};
    const std::array<float, 3> coatings{{.02f, .35f, .60f}};
    for (int index = 0; index < 3; ++index) {
      auto material = scene.materials[M_GLASS_STD];
      material.name = "occupied glazing optical proof " + std::to_string(index);
      material.flags = kMatGlass;
      material.albedo_set = "";
      material.base_color = enabled ? transmission[index] : transmission[0];
      material.metallic = enabled ? coatings[index] : coatings[0];
      material.tint2 = {.94f, .89f, .78f};
      material.roughness = .09f;
      material.room_w = 3;
      material.room_h = 4;
      material.room_d = 6;
      material.lit_probability = .2f;
      const auto id = static_cast<std::uint32_t>(scene.materials.size());
      scene.materials.push_back(material);
      const float x = (index - 1) * 3.3f;
      Emit pane(&scene.opaque, id);
      pane.facade = true;
      pane.facade_origin = {x - 1.45f, 0, 0};
      pane.quad_metric({x - 1.45f, 0, 0}, {x + 1.45f, 0, 0}, {x + 1.45f, 4, 0},
                       {x - 1.45f, 4, 0});
      for (float side : {-1.48f, 1.48f})
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({x + side, 2, 0}, {.03f, 2.03f, .04f});
      for (float height : {-.03f, 4.03f})
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({x, height, 0}, {1.51f, .03f, .04f});
    }
    scene.camera_position = {0, 2, 12};
    scene.camera_target = {0, 2, 0};
    scene.camera_fov_degrees = 35;
    scene.city_radius = 20;
    scene.city_size = name;
    scene.has_lighting_override = true;
    scene.sun_direction = {.3f, .8f, .2f};
    scene.sun_irradiance = {1.5f, 1.5f, 1.5f};
    scene.lighting_exposure = 1;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-stone-")) {
    auto &stone = scene.materials[M_TERRAZZO];
    stone.name = "rough wet stone raised-plane proof";
    stone.base_color = {.35f, .35f, .35f};
    stone.roughness = .5f;
    stone.metallic = 0;
    stone.flags = kMatPlanarXZ | 512u;
    stone.albedo_set = "flat";
    Emit(&scene.opaque, M_TERRAZZO)
        .box({0, 1.168f, -5}, {10, .05f, 15});
    // This unchanged cube lies above the direct frame, inside its reflection.
    Emit(&scene.opaque, M_SIGN).box({0, 5.218f, -3}, {1, 1, .5f});
    scene.camera_position = {0, 3.218f, 6};
    scene.camera_target = {0, 1.218f, 0};
    scene.camera_fov_degrees = 40;
    scene.city_radius = 20;
    scene.city_size = name;
    scene.has_lighting_override = true;
    scene.sun_direction = {0, 1, 0};
    scene.sun_irradiance = {0, 0, 0};
    scene.lighting_exposure = 1;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-wet-")) {
    auto &paving = scene.materials[M_TERRAZZO];
    paving.base_color = {.7f, .7f, .7f};
    paving.tint2 = paving.base_color;
    paving.roughness = .8f;
    paving.metallic = 0;
    paving.albedo_set = "flat";
    paving.flags = kMatPlanarXZ | (enabled ? 512u : 0u);
    Emit(&scene.opaque, M_TERRAZZO).box({0, -.2f, 0}, {30, .2f, 30});
    scene.camera_position = {0, 25, 38};
    scene.camera_target = {0, 0, 0};
    scene.camera_fov_degrees = 50;
    scene.city_radius = 50;
    scene.city_size = name;
    scene.has_lighting_override = true;
    scene.sun_direction = {.3f, .8f, .2f};
    scene.sun_irradiance = {2.f, 1.8f, 1.5f};
    scene.lighting_exposure = 1;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-glass-")) {
    auto &glass = scene.materials[M_GLASS_CLEAR];
    glass.base_color = {.2f, .35f, .45f};
    glass.tint2 = {.2f, .35f, .45f};
    glass.flags = 128u;
    glass.albedo_set = "flat";
    glass.roughness = .08f;
    glass.metallic = 0;
    glass.room_w = 1.5f;
    glass.room_h = .78f;
    glass.room_d = 1.f;
    glass.lit_probability = .06f;
    if (enabled)
      Emit(&scene.opaque, M_GLASS_CLEAR).box({0, 6, -2}, {1.7f, 1.5f, .006f});
    for (float x : {-1.75f, 1.75f})
      Emit(&scene.opaque, M_WHITE_METAL).box({x, 6, -2}, {.025f, 1.55f, .035f});
    for (float y : {4.45f, 7.55f})
      Emit(&scene.opaque, M_WHITE_METAL)
          .box({0, y, -2}, {1.775f, .025f, .035f});
    scene.camera_position = {0, 4, 8};
    scene.camera_target = {0, 6, -2};
    scene.camera_fov_degrees = 40;
    scene.city_radius = 20;
    scene.city_size = name;
    scene.has_lighting_override = false;
    scene.finalize_draws();
    return scene;
  }
  if (name.starts_with("reflection-proof-null-")) {
    const bool finite = name.find("finite") != std::string::npos;
    white.base_color = {.30f, .38f, .46f};
    const float left = finite ? -900.f : 0.f;
    Emit(&scene.opaque, M_WHITE_METAL)
        .quad_metric({left, -615, -1000}, {900, -615, -1000},
                     {900, 685, -1000}, {left, 685, -1000});
    auto stripe = white;
    stripe.name = "null-test opaque wall inlay";
    stripe.base_color = {.06f, .07f, .08f};
    const auto stripe_id = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(stripe);
    for (int x = finite ? -800 : 0; x <= 800; x += 100)
      Emit(&scene.opaque, stripe_id)
          .quad_metric({static_cast<float>(x), -615, -999.9f},
                       {static_cast<float>(x + 4), -615, -999.9f},
                       {static_cast<float>(x + 4), 685, -999.9f},
                       {static_cast<float>(x), 685, -999.9f});
    auto &pane = scene.materials[M_GLASS_CLEAR];
    pane.base_color = {1, 1, 1};
    pane.flags = 128u;
    pane.albedo_set = "flat";
    pane.roughness = .045f;
    pane.metallic = 0;
    pane.room_w = finite ? 1.001f : 1.5f;
    pane.room_h = 1;
    pane.room_d = finite ? .5f : 0.f;
    pane.lit_probability = finite ? 0.f : .26f;
    if (enabled)
      Emit(&scene.opaque, M_GLASS_CLEAR)
          .quad_metric({-210, -95, -500}, {210, -95, -500},
                       {210, 165, -500}, {-210, 165, -500});
    scene.camera_position = {0, 35, 0};
    scene.camera_target = {0, 35, -1000};
    scene.camera_fov_degrees = 40;
  } else if (name.starts_with("reflection-proof-pond-")) {
    auto &pond = scene.materials[M_GLASS_CLEAR];
    pond.name = "shallow pond exact-plane proof";
    pond.base_color = {.975f, .989f, .985f};
    pond.flags = 128u | 1024u;
    pond.albedo_set = "flat";
    pond.roughness = .045f;
    pond.metallic = 0;
    pond.room_w = 1.333f;
    pond.room_h = .96f;
    pond.room_d = .98f;
    pond.lit_probability = .26f;
    // The actual bottom is 26 cm beneath this one upward-facing pane. Its
    // visible inlaid stripes establish transmission independently of the
    // changed emitter, which remains wholly outside the direct camera.
    Emit(&scene.opaque, M_GLASS_CLEAR)
        .quad_metric({-10, 1.04f, 10}, {10, 1.04f, 10},
                     {10, 1.04f, -20}, {-10, 1.04f, -20});
    Emit(&scene.opaque, M_WHITE_METAL)
        .box({0, .73f, -5}, {10, .05f, 15});
    auto stripe = white;
    stripe.name = "pond bottom inlay proof";
    stripe.base_color = {.075f, .12f, .16f};
    const auto stripe_id = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(stripe);
    for (int z = -16; z <= 8; z += 2)
      Emit(&scene.opaque, stripe_id)
          .box({0, .783f, static_cast<float>(z)}, {10, .002f, .03f});
    Emit(&scene.opaque, M_SIGN).box({0, 5.04f, -3}, {1, 1, .5f});
    scene.camera_position = {0, 3.04f, 6};
    scene.camera_target = {0, 1.04f, 0};
    scene.camera_fov_degrees = 40;
  } else if (reflection) {
    auto &water = scene.materials[M_WATER];
    water.base_color = {.02f, .035f, .04f};
    water.roughness = .12f;
    water.metallic = 0;
    water.flags = kMatPlanarXZ | 64u;
    water.albedo_set = "flat";
    Emit(&scene.opaque, M_WATER).box({0, -.05f, -5}, {10, .05f, 15});
    // Whole cube is above the direct frame, but inside the reflected frame.
    if (name.starts_with("reflection-proof-transmitted-")) {
      auto pane = scene.materials[M_WHITE_METAL];
      pane.name = "offscreen solar glass proof";
      pane.base_color =
          enabled ? Vec3{.72f, .12f, .09f} : Vec3{.09f, .23f, .72f};
      pane.flags = 128u;
      pane.roughness = .12f;
      pane.metallic = 0;
      pane.room_w = 1.52f;
      pane.room_h = .40f;
      pane.room_d = .97f;
      pane.lit_probability = .045f;
      pane.albedo_set = "";
      const auto material = static_cast<std::uint32_t>(scene.materials.size());
      scene.materials.push_back(pane);
      Emit(&scene.opaque, material).box({0, 4, -3}, {1, 1, .5f});
      // A fixed neutral luminous panel behind the same offscreen pane makes
      // its transmitted response legible independently of the dark sky.
      auto panel = scene.materials[M_WHITE_METAL];
      panel.name = "offscreen transmission backlight";
      panel.flags = kMatEmissive;
      panel.emissive = 4.f;
      panel.tint2 = {1, 1, 1};
      const auto panel_id = static_cast<std::uint32_t>(scene.materials.size());
      scene.materials.push_back(panel);
      Emit(&scene.opaque, panel_id).box({0, 4, -3.6f}, {.95f, .95f, .025f});
    } else
      Emit(&scene.opaque, M_SIGN).box({0, 4, -3}, {1, 1, .5f});
    scene.camera_position = {0, 2, 6};
    scene.camera_target = {0, 0, 0};
    scene.camera_fov_degrees = 40;
  } else {
    Emit(&scene.opaque, M_WHITE_METAL).box({0, -.2f, 1}, {6, .2f, 7});
    Emit(&scene.opaque, M_WHITE_METAL).box({0, 2, -3}, {5, 2, .15f});
    // This luminous wall is behind the camera; no main-view pixel sees it.
    Emit(&scene.opaque, M_SIGN).box({0, 3, 10}, {5, 3, .15f});
    scene.camera_position = {0, 2.4f, 7};
    scene.camera_target = {0, 1.8f, -3};
    scene.camera_fov_degrees = 40;
  }
  scene.city_radius = 20;
  scene.city_size = name;
  scene.has_lighting_override = true;
  scene.sun_direction = {0, 1, 0};
  scene.sun_irradiance = {0, 0, 0};
  scene.lighting_exposure = 1;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
