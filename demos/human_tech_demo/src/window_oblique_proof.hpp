#pragma once
#include "scene.hpp"
#include <array>
#include <cmath>
#include <stdexcept>

namespace cb {
// Real tilted metric panes cropped to disjoint camera rectangles. Changing yaw
// foreshortens horizontal rooms while retaining resolved vertical floor bands.
// Camera roll is a rotation of the actual pane basis, not of a rendered image.
inline Scene generate_window_oblique_proof() {
  Scene scene;
  scene.materials = make_materials();
  constexpr std::array<float, 3> probabilities{.045f, .19f, .5f};
  constexpr std::array<float, 3> rolls{0.f, 27.f, 63.f};
  constexpr std::array<float, 5> horizontal{.9f, 1.2f, 3.f, 8.f, 24.f};
  constexpr std::array<float, 5> vertical{8.f, 8.f, 8.f, 12.f, 30.f};
  std::array<std::uint32_t, 3> materials{};
  for (std::size_t i = 0; i < probabilities.size(); ++i) {
    auto material = scene.materials[M_GLASS_CLEAR];
    material.name =
        "oblique floor probability " + std::to_string(probabilities[i]);
    material.base_color = {1, 1, 1};
    material.tint2 = {.94f, .90f, .81f};
    material.metallic = 0;
    material.roughness = .075f;
    material.flags = kMatGlass;
    material.emissive = 0;
    material.albedo_set = "";
    material.room_w = 4.5f;
    material.room_h = 3.6f;
    material.room_d = 6;
    material.lit_probability = probabilities[i];
    materials[i] = static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(material);
  }
  const float tangent = std::tan(radians(40.f) * .5f);
  auto ray = [&](float x, float y) {
    return Vec3{(2 * x / 1280.f - 1) * (1280.f / 720.f) * tangent,
                (1 - 2 * y / 720.f) * tangent, -1};
  };
  for (std::size_t row = 0; row < horizontal.size(); ++row)
    for (std::size_t p = 0; p < probabilities.size(); ++p)
      for (std::size_t r = 0; r < rolls.size(); ++r) {
        const float x0 = 16 + (p * 3 + r) * 139.f;
        const float y0 = 20 + row * 136.f;
        constexpr float width = 108, height = 108;
        const float depth = 720.f * 3.6f / (2 * tangent * vertical[row]);
        const Vec3 centre = ray(x0 + width * .5f, y0 + height * .5f) * depth;
        const Vec3 camera = normalize(centre * -1.f);
        const Vec3 right = normalize(cross(Vec3{0, 1, 0}, camera));
        const Vec3 up = cross(camera, right);
        const float angle = radians(rolls[r]);
        const Vec3 flat_u = right * std::cos(angle) + up * std::sin(angle);
        const Vec3 vertical_axis =
            up * std::cos(angle) - right * std::sin(angle);
        const float cosine = horizontal[row] / (vertical[row] * 4.5f / 3.6f);
        const float sine = std::sqrt(1 - cosine * cosine);
        const Vec3 normal = camera * cosine - flat_u * sine;
        const Vec3 along = flat_u * cosine + camera * sine;
        auto world = [&](float x, float y) {
          const Vec3 direction = ray(x, y);
          const float denominator = dot(normal, direction);
          if (denominator >= -1e-5f)
            throw std::runtime_error(
                "oblique proof pane crosses the eye plane");
          return direction * (dot(normal, centre) / denominator);
        };
        float minimum_v = 1e20f, maximum_v = -1e20f;
        for (const auto pixel :
             {Vec2{x0, y0}, Vec2{x0 + width, y0}, Vec2{x0, y0 + height},
              Vec2{x0 + width, y0 + height}}) {
          const float v = dot(world(pixel.x, pixel.y) - centre, vertical_axis);
          minimum_v = std::min(minimum_v, v);
          maximum_v = std::max(maximum_v, v);
        }
        const float offset_v = -std::floor(minimum_v / 3.6f) * 3.6f;
        if (maximum_v + offset_v > 655.3f)
          throw std::runtime_error(
              "oblique proof exceeds packed vertical coordinates");
        constexpr int nx = 48, ny = 12;
        for (int yy = 0; yy < ny; ++yy)
          for (int xx = 0; xx < nx; ++xx) {
            const float a = x0 + width * xx / nx,
                        b = x0 + width * (xx + 1) / nx;
            const float c = y0 + height * yy / ny,
                        d = y0 + height * (yy + 1) / ny;
            const std::array<Vec3, 4> corners{world(a, d), world(b, d),
                                              world(b, c), world(a, c)};
            float minimum_u = 1e20f, maximum_u = -1e20f;
            for (auto v : corners) {
              const float u = dot(v - centre, along);
              minimum_u = std::min(minimum_u, u);
              maximum_u = std::max(maximum_u, u);
            }
            const float offset_u = -std::floor(minimum_u / 4.5f) * 4.5f;
            if (maximum_u + offset_u > 655.3f)
              throw std::runtime_error(
                  "oblique proof exceeds packed horizontal coordinates");
            const auto first =
                static_cast<std::uint32_t>(scene.opaque.vertices.size());
            const float seed = .137f + p * .17f + r * .037f;
            for (auto v : corners) {
              const float u = dot(v - centre, along) + offset_u;
              const float h = dot(v - centre, vertical_axis) + offset_v;
              scene.opaque.add_vertex({v,
                                       normal,
                                       Vec4{along, 1},
                                       {u, h},
                                       materials[p],
                                       {u, h, seed, 1}});
            }
            scene.opaque.add_triangle(first, first + 1, first + 2);
            scene.opaque.add_triangle(first, first + 2, first + 3);
          }
      }
  scene.camera_position = {};
  scene.camera_target = {0, 0, -1};
  scene.camera_fov_degrees = 40;
  scene.city_radius = 20;
  scene.city_size = "oblique occupied floor filtering atlas";
  scene.has_lighting_override = true;
  scene.sun_direction = {0, 1, 0};
  scene.sun_irradiance = {0, 0, 0};
  scene.lighting_exposure = 1;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
