#pragma once
#include "scene.hpp"
#include <array>
#include <cmath>

namespace cb {
// Flat metric panes at different physical distances. The same 4.5x3.6m room
// grid is rotated in its plane; no screen-space replacement geometry is used.
inline Scene generate_window_filter_proof() {
  Scene scene;
  scene.materials = make_materials();
  constexpr float image_w = 1280, image_h = 720;
  constexpr float top = 20, left = 16, step_x = 139, step_y = 136;
  constexpr float pane_w = 132, pane_h = 128;
  constexpr std::array<float, 3> probabilities{.045f, .19f, .5f};
  constexpr std::array<float, 3> rolls{0.f, 27.f, 63.f};
  constexpr std::array<float, 5> sizes{.75f, 1.2f, 3.f, 8.f, 24.f};
  std::array<std::uint32_t, 3> materials{};
  for (std::size_t p = 0; p < probabilities.size(); ++p) {
    auto glass = scene.materials[M_GLASS_CLEAR];
    glass.name = "window filtering probability " + std::to_string(probabilities[p]);
    glass.base_color = {1, 1, 1};
    glass.tint2 = {.94f, .90f, .81f};
    glass.metallic = 0;
    glass.roughness = .075f;
    glass.albedo_set = "";
    glass.flags = kMatGlass;
    glass.emissive = 0;
    glass.room_w = 4.5f;
    glass.room_h = 3.6f;
    glass.room_d = 6;
    glass.lit_probability = probabilities[p];
    materials[p] = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(glass);
  }
  const float tangent = std::tan(radians(40.f) * .5f);
  for (std::size_t row = 0; row < sizes.size(); ++row)
    for (std::size_t p = 0; p < probabilities.size(); ++p)
      for (std::size_t roll = 0; roll < rolls.size(); ++roll) {
        const float distance = image_h * 4.5f / (2 * tangent * sizes[row]);
        const float angle = radians(rolls[roll]);
        const Vec3 along{std::cos(angle), std::sin(angle), 0};
        const Vec3 up{-std::sin(angle), std::cos(angle), 0};
        const float x0 = left + (p * 3 + roll) * step_x;
        const float y0 = top + row * step_y;
        auto world = [&](float x, float y) {
          return Vec3{(2 * x / image_w - 1) * (image_w / image_h) * tangent * distance,
                      (1 - 2 * y / image_h) * tangent * distance, -distance};
        };
        const auto physical = world(x0 + pane_w, y0 + pane_h) - world(x0, y0);
        const int nx = std::max(1, int(std::ceil(std::abs(physical.x) / 160)));
        const int ny = std::max(1, int(std::ceil(std::abs(physical.y) / 160)));
        for (int yy = 0; yy < ny; ++yy)
          for (int xx = 0; xx < nx; ++xx) {
            const float a = x0 + pane_w * float(xx) / nx;
            const float b = x0 + pane_w * float(xx + 1) / nx;
            const float c = y0 + pane_h * float(yy) / ny;
            const float d = y0 + pane_h * float(yy + 1) / ny;
            const std::array<Vec3, 4> corners{world(a, d), world(b, d), world(b, c), world(a, c)};
            float minimum_u = 1e20f, minimum_v = 1e20f;
            for (Vec3 v : corners) {
              minimum_u = std::min(minimum_u, dot(v, along));
              minimum_v = std::min(minimum_v, dot(v, up));
            }
            // Whole-room offsets preserve the physical mullion phase while
            // each GPU auxiliary coordinate stays within its packed range.
            const float offset_u = -std::floor(minimum_u / 4.5f) * 4.5f;
            const float offset_v = -std::floor(minimum_v / 3.6f) * 3.6f;
            const auto first = static_cast<std::uint32_t>(scene.opaque.vertices.size());
            const float seed = std::fmod(.137f + xx * .071f + yy * .043f + p * .17f + roll * .037f, .99f);
            for (Vec3 v : corners) {
              const float u = dot(v, along) + offset_u, h = dot(v, up) + offset_v;
              scene.opaque.add_vertex({v, {0, 0, 1}, Vec4{along, 1}, {u, h}, materials[p], {u, h, seed, 1}});
            }
            scene.opaque.add_triangle(first, first + 1, first + 2);
            scene.opaque.add_triangle(first, first + 2, first + 3);
          }
      }
  scene.camera_position = {0, 0, 0};
  scene.camera_target = {0, 0, -1};
  scene.camera_fov_degrees = 40;
  scene.city_radius = 20;
  scene.city_size = "occupied window filtering atlas";
  scene.has_lighting_override = true;
  scene.sun_direction = {0, 1, 0};
  scene.sun_irradiance = {0, 0, 0};
  scene.lighting_exposure = 1;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
