#include "market_climbers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
constexpr std::string_view kResource = "market_coping_guided_climber";
std::uint32_t material(const Scene& scene, std::string_view name) {
  for (std::uint32_t i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name) return i;
  throw std::runtime_error("Guided market climber requires material: " + std::string(name));
}
void triangle(Mesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  const Vec3 geometric = cross(mesh.vertices[b].position - mesh.vertices[a].position,
                               mesh.vertices[c].position - mesh.vertices[a].position);
  if (length(geometric) < 1e-9f) return;
  if (dot(geometric, mesh.vertices[a].normal + mesh.vertices[b].normal + mesh.vertices[c].normal) < 0)
    std::swap(b, c);
  mesh.add_triangle(a, b, c);
}
void stem(Mesh& mesh, std::uint32_t mat, const std::vector<Vec3>& points) {
  constexpr int sides = 6;
  constexpr float radius = .006f;
  const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  std::vector<Vec3> along;
  float arc = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (i) arc += length(points[i] - points[i - 1]);
    const Vec3 direction = normalize(points[std::min(i + 1, points.size() - 1)] - points[i ? i - 1 : 0]);
    along.push_back(direction);
    const Vec3 side = normalize(cross(direction, {1, 0, 0}));
    const Vec3 other = cross(direction, side);
    for (int j = 0; j < sides; ++j) {
      const float a = j * 2 * kPi / sides;
      Vertex v; v.normal = side * std::cos(a) + other * std::sin(a);
      v.position = points[i] + v.normal * radius;
      v.tangent = {direction, -1}; v.material = mat;
      v.uv = {arc, a * radius}; v.aux = {v.uv.x, v.uv.y, 0, 1};
      mesh.add_vertex(v);
    }
    if (i) for (int j = 0; j < sides; ++j) {
      const auto a = first + static_cast<std::uint32_t>(i - 1) * sides + j;
      const auto b = first + static_cast<std::uint32_t>(i - 1) * sides + (j + 1) % sides;
      triangle(mesh, a, a + sides, b); triangle(mesh, b, a + sides, b + sides);
    }
  }
  for (bool last : {false, true}) {
    const auto ring = first + static_cast<std::uint32_t>(last ? points.size() - 1 : 0) * sides;
    const Vec3 centre = last ? points.back() : points.front();
    const Vec3 normal = (last ? along.back() : along.front()) * (last ? 1.f : -1.f);
    for (int j = 0; j < sides; ++j) {
      Vec3 b = mesh.vertices[ring + j].position, c = mesh.vertices[ring + (j + 1) % sides].position;
      if (dot(cross(b - centre, c - centre), normal) < 0) std::swap(b, c);
      Emit(&mesh, mat).triangle(centre, b, c);
    }
  }
}
void leaf(Mesh& mesh, std::uint32_t mat, Vec3 base, Vec3 tip) {
  // Same five-segment, 95 mm curled blade recipe as the authored climber.
  // Each blade is generated in its own natural orientation; no stretched mesh.
  constexpr int segments = 5;
  constexpr float width = .095f, curl = .035f;
  const Vec3 direction = tip - base;
  const Vec3 side = normalize(cross(direction, {0, 1, 0}));
  const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  for (int j = 0; j <= segments; ++j) for (int across = -1; across <= 1; ++across) {
    const float t = float(j) / segments, arc = std::sin(kPi * t);
    const float derivative = kPi * std::cos(kPi * t);
    const Vec3 du = direction + side * (width * across * derivative) +
                    Vec3{0, (curl - std::abs(across) * width * .18f) * derivative, 0};
    const Vec3 dv = side + Vec3{0, -float(across) * .18f, 0};
    Vertex v; v.position = base + direction * t + side * (width * across * arc) +
                           Vec3{0, (curl - std::abs(across) * width * .18f) * arc, 0};
    v.normal = normalize(cross(du, dv));
    v.tangent = {normalize(du - v.normal * dot(du, v.normal)), 1};
    v.uv = {t * length(direction), across * width}; v.aux = {v.uv.x, v.uv.y, 0, 1}; v.material = mat;
    mesh.add_vertex(v);
  }
  for (int j = 0; j < segments; ++j) for (int k = 0; k < 2; ++k) {
    const auto a = first + j * 3 + k;
    triangle(mesh, a, a + 3, a + 1); triangle(mesh, a + 1, a + 3, a + 4);
  }
}
void ensure_resource(Scene& scene) {
  for (const auto& resource : scene.asset_library.resources) if (resource.name == kResource) return;
  MeshResource resource; resource.name = kResource;
  const auto bark = material(scene, "bark_ridged"), dark = material(scene, "leaf_deep"),
             green = material(scene, "leaf_middle");
  for (int vine = 0; vine < 11; ++vine) {
    const Vec3 root{(vine - 5) * .15f + std::sin(float(vine)) * .12f, 0, std::cos(float(vine)) * .11f};
    std::vector<Vec3> path;
    for (int sample = 0; sample <= 16; ++sample) {
      const float t = sample / 16.f, u = 1 - t;
      // A real upright shoot crosses the coping only after it has risen.
      const Vec3 arch = Vec3{0, .35f, 0} * (3 * u * u * t) +
                        Vec3{0, .66f, .18f} * (3 * u * t * t) +
                        Vec3{0, .62f, .80f} * (t * t * t);
      path.push_back(root + arch);
    }
    for (int sample = 1; sample <= 15; ++sample) {
      const float d = sample * .27f;
      path.push_back(root + Vec3{.12f * (std::sin(d * 2.59f + vine) - std::sin(float(vine))),
                                .62f - d,
                                .80f + .15f * (1 - std::exp(-d)) + .045f * (std::sin(d * 2.07f + vine) - std::sin(float(vine)))});
    }
    stem(resource.mesh, bark, path);
    for (std::size_t j = 10; j < path.size(); ++j) {
      if (j < 16 && j % 2) continue;
      for (float sign : {-1.f, 1.f})
        leaf(resource.mesh, j % 3 ? green : dark, path[j], path[j] + Vec3{sign * .17f, -.10f, .14f});
    }
  }
  scene.asset_library.resources.push_back(std::move(resource));
}
}  // namespace
void stage_market_canopy_climbers(Scene& scene, Vec3 soil_point, float yaw, float scale) {
  if (scene.asset_library.resources.empty()) return;
  ensure_resource(scene);
  add_asset_instance(scene, kResource, soil_point - Vec3{0, .006f * scale, 0}, yaw, scale);
}
}  // namespace cb
