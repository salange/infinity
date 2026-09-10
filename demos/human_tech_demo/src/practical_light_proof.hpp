#pragma once
// Tiny native controls for finite source reach and real optical pass lighting.
#include "scene.hpp"
#include <algorithm>
#include <string>

namespace cb {
inline Scene generate_practical_light_proof(const std::string &name) {
  Scene scene;
  scene.materials = make_materials();
  const bool enabled = name.ends_with("-on");
  auto neutral = scene.materials[M_WHITE_METAL];
  neutral.name = "practical proof diffuse receiver";
  neutral.base_color = {.82f, .82f, .82f};
  neutral.metallic = 0;
  neutral.roughness = .85f;
  neutral.flags = 0;
  neutral.emissive = 0;
  neutral.albedo_set = "flat";
  scene.materials[M_WHITE_METAL] = neutral;
  auto occupied = scene.materials[M_GLASS_BLUE];
  occupied.name = "practical proof occupied pane";
  occupied.base_color = {.85f, .90f, .94f};
  occupied.flags = kMatGlass;
  occupied.metallic = 0;
  occupied.roughness = .18f;
  occupied.albedo_set = "flat";
  occupied.emissive = 0;
  occupied.room_w = 4.5f;
  occupied.room_h = 3.6f;
  occupied.room_d = 6;
  occupied.lit_probability = .19f;
  scene.materials[M_GLASS_BLUE] = occupied;
  auto optical = occupied;
  optical.name = "practical proof true pane";
  optical.flags = 128u;
  optical.room_w = 1.5f;
  optical.room_h = .9f;
  optical.room_d = 1;
  optical.lit_probability = .022f;
  scene.materials[M_GLASS_CLEAR] = optical;

  if (name.starts_with("indirect-proof-practical-visibility-")) {
    Emit(&scene.opaque, M_WHITE_METAL).box({0, -.1f, 0}, {5, .1f, 5});
    const bool open = name.find("-open-") != std::string::npos;
    const bool behind = name.find("-behind-") != std::string::npos;
    const bool thin = name.find("-thin-") != std::string::npos;
    const bool oblique = name.find("-oblique-") != std::string::npos;
    const bool contact = name.find("-contact-") != std::string::npos;
    const bool aperture = name.find("-aperture-") != std::string::npos;
    const bool boundary = name.find("-boundary-") != std::string::npos;
    const bool cavity = name.find("-cavity-") != std::string::npos;
    const bool endpoint = name.find("-endpoint-") != std::string::npos;
    const float wall_x = boundary  ? -6.2f
                         : behind  ? -4.2f
                         : contact ? -.22f
                                   : -1.5f;
    if (cavity) {
      auto absorber = neutral;
      absorber.name = "opaque black sealed point-source enclosure";
      absorber.base_color = {0, 0, 0};
      const auto id = static_cast<std::uint32_t>(scene.materials.size());
      scene.materials.push_back(absorber);
      Emit(&scene.opaque, id).box({-3, 2, 0}, {.65f, .65f, .65f});
    } else if (!open && !endpoint) {
      if (aperture) {
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({wall_x, 2, -1.3f}, {.1f, 2, .7f});
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({wall_x, 2, 1.3f}, {.1f, 2, .7f});
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({wall_x, 3.5f, 0}, {.1f, .5f, .6f});
      } else {
        const auto first = scene.opaque.vertices.size();
        Emit(&scene.opaque, M_WHITE_METAL)
            .box({wall_x, 2, 0}, {thin ? .0125f : .1f, 2, 2});
        if (oblique) {
          constexpr float angle = .47f;
          const float c = std::cos(angle), s = std::sin(angle);
          for (auto i = first; i < scene.opaque.vertices.size(); ++i) {
            auto &v = scene.opaque.vertices[i];
            const auto p = v.position - Vec3{wall_x, 0, 0};
            v.position =
                Vec3{c * p.x + s * p.z + wall_x, p.y, -s * p.x + c * p.z};
            auto turn = [&](Vec3 n) {
              return Vec3{c * n.x + s * n.z, n.y, -s * n.x + c * n.z};
            };
            v.normal = turn(v.normal);
            const auto tangent = turn({v.tangent.x, v.tangent.y, v.tangent.z});
            v.tangent = Vec4{tangent, v.tangent.w};
          }
        }
      }
    }
    if (boundary) {
      // A real offscreen bounds marker places the fine grid's west boundary
      // between the source and wall. Their finite segment crosses both grids.
      // With actual bounds[-6.3,32.5], the adaptive fine west edge is
      // approximately-9.07m, between source-10m and wall-6.2m.
      Emit(&scene.opaque, M_WHITE_METAL)
          .box({32.45f, 0, 0}, {.05f, .05f, .05f});
    }
    Vec3 lamp{boundary ? -10.f : -3.f, 2, 0};
    if (endpoint) {
      // A small real fixture entirely within the source endpoint voxel. Source
      // and fixture sit at the voxel centre, not on an ambiguous shared corner.
      lamp = {-3.0625f, 2.0625f, .0625f};
      // The real marker keeps assembly vertical bounds[-.2,4], making the
      // fixture's full2cm width lie inside a single fine cell on every axis.
      Emit(&scene.opaque, M_WHITE_METAL).box({-4, 2, 4}, {.02f, 2, .02f});
      Emit(&scene.opaque, M_WHITE_METAL).box(lamp, {.01f, .01f, .01f});
    }
    scene.lights.push_back(
        {lamp,
         name.find("-radius-") != std::string::npos ? 1.f : 8.f,
         {1, .7f, .4f},
         enabled ? 40.f : 0.f});
    scene.camera_position = {boundary ? -4.f : 0.f, 5, 10};
    scene.camera_target = {boundary ? -4.f : 0.f, 0, 0};
    scene.camera_fov_degrees = 40;
  } else if (name.starts_with("indirect-proof-practical-selection-")) {
    Emit(&scene.opaque, M_WHITE_METAL)
        .quad_metric({-60, -40, -100}, {60, -40, -100}, {60, 40, -100},
                     {-60, 40, -100});
    if (name.find("reference") == std::string::npos)
      for (int i = 0; i < 64; ++i)
        scene.lights.push_back({{0, 0, -10.f - .01f * i}, 2, {1, 1, 1}, 40});
    scene.lights.push_back(
        {{0, 0, -98}, 5, {1, .7f, .4f}, enabled ? 40.f : 0.f});
    if (name.find("reversed") != std::string::npos)
      std::reverse(scene.lights.begin(), scene.lights.end());
    scene.camera_position = {0, 0, 0};
    scene.camera_target = {0, 0, -100};
    scene.camera_fov_degrees = 50;
  } else if (name.starts_with("indirect-proof-practical-wall-")) {
    Emit(&scene.opaque, M_WHITE_METAL).box({0, -.1f, 0}, {5, .1f, 5});
    Emit(&scene.opaque, M_WHITE_METAL).box({-1.5f, 2, 0}, {.1f, 2, 2});
    // A segment from this source to the visible floor origin crosses the real
    // wall. The source-reach suite deliberately disables point visibility so
    // this remains its negative control; the visibility suite tests occlusion.
    scene.lights.push_back(
        {{-3, 2, 0}, 8, {1, .7f, .4f}, enabled ? 40.f : 0.f});
    scene.camera_position = {0, 5, 10};
    scene.camera_target = {0, 0, 0};
    scene.camera_fov_degrees = 40;
  } else {
    const bool reflection = name.starts_with("reflection-proof-practical-");
    const bool zero = name.find("zero") != std::string::npos;
    const bool beyond = name.find("beyond") != std::string::npos;
    const float plane = name.find("pond") != std::string::npos     ? 1.04f
                        : name.find("puddle") != std::string::npos ? 1.222f
                                                                   : 0.f;
    scene.camera_position = reflection ? Vec3{0, plane + 2, 6} : Vec3{0, 2, 6};
    scene.camera_target = reflection ? Vec3{0, plane, 0} : Vec3{0, 2, -6};
    scene.camera_fov_degrees = 40;
    if (reflection) {
      auto floor = neutral;
      floor.name = "practical proof physical reflection plane";
      floor.base_color = {.025f, .045f, .06f};
      floor.roughness = .045f;
      floor.flags = kMatPlanarXZ | 64u;
      if (plane == 1.222f)
        floor.flags = kMatPlanarXZ;
      if (plane == 1.04f) {
        floor = optical;
        floor.name = "practical proof shallow pond";
        floor.flags = 128u | 1024u;
        floor.room_w = 1.333f;
        floor.room_h = .96f;
        floor.room_d = 1;
        floor.lit_probability = .26f;
        floor.roughness = .045f;
      }
      const auto id = static_cast<std::uint32_t>(scene.materials.size());
      scene.materials.push_back(floor);
      Emit(&scene.opaque, id)
          .quad_metric({-10, plane, 10}, {10, plane, 10}, {10, plane, -20},
                       {-10, plane, -20});
    }
    Vec3 optical_camera = scene.camera_position;
    if (reflection)
      optical_camera.y = 2 * plane - optical_camera.y;
    if (zero)
      scene.materials[M_GLASS_CLEAR].room_d = 0;
    for (int panel = 0; panel < (zero ? 1 : 3); ++panel) {
      const auto material = zero         ? M_GLASS_CLEAR
                            : panel == 0 ? M_WHITE_METAL
                            : panel == 1 ? M_GLASS_BLUE
                                         : M_GLASS_CLEAR;
      const float x = zero ? 0 : (panel - 1) * 1.4f;
      const Vec3 centre{x, reflection ? plane + 4 : 2,
                        reflection ? -3.f : -6.f};
      Emit(&scene.opaque, material)
          .quad_metric(
              centre + Vec3{-.52f, -.8f, 0}, centre + Vec3{.52f, -.8f, 0},
              centre + Vec3{.52f, .8f, 0}, centre + Vec3{-.52f, .8f, 0});
      const Vec3 toward_camera = normalize(optical_camera - centre);
      const Vec3 toward_source{-toward_camera.x, -toward_camera.y,
                               toward_camera.z};
      // Each source's mirror direction puts a glint on its own pane. In the
      // reflection scene, all source spheres remain well above the water and
      // every receiver is outside the direct camera: no emissive substitute.
      scene.lights.push_back({centre + toward_source * (beyond ? 3.f : 1.2f),
                              1.8f,
                              {1, .7f, .4f},
                              enabled ? 8.f : 0.f});
    }
    if (reflection && name.find("-visibility-") != std::string::npos)
      Emit(&scene.opaque, M_WHITE_METAL)
          .box({0, plane + 4.25f, -2.5f}, {4, .05f, 1.5f});
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
