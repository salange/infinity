#pragma once

// A fixed native fixture for comparing complete world-sky radiance. The main
// application selects the civic weather preset; changing the source environment
// never changes this geometry, material recipe, view direction or finite light.
#include "civic_landmarks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace cb {
inline constexpr const char* kEnvironmentProofShot = "civic";

namespace environment_proof_detail {
inline MaterialDesc production_silver() {
  // Ask the production builder for its actual palette entry, including its
  // linear map compensation and optical flags. Do not maintain a second recipe
  // that can silently diverge from the ring being diagnosed. Scratch landmark
  // geometry is discarded; the final fixture contains only the objects below.
  Scene source;
  source.materials = make_materials();
  build_cinematic_ring(source, {140, -80}, 1.2f, 58, radians(14));
  for (const auto& material : source.materials)
    if (material.name == "civic brushed silver shell") return material;
  throw std::runtime_error("Environment proof requires production civic silver");
}

inline void bevelled_cube(Mesh& mesh, std::uint32_t material, Vec3 centre,
                         Vec3 right, Vec3 front) {
  constexpr float half = 1.5f, bevel = .2f, core = half - bevel;
  // Rounded-box surface: six planar centres, cylindrical edges and spherical
  // corner patches. Shared boundary samples coincide, with analytic normals.
  // The bevel is 20 cm of physical geometry rather than a normal-map illusion.
  const std::array<float, 10> knots{
      -half, -1.45f, -1.40f, -1.35f, -core,
      core, 1.35f, 1.40f, 1.45f, half};
  auto world = [&](Vec3 p) { return right * p.x + Vec3{0, p.y, 0} + front * p.z; };
  auto component = [](Vec3& p, int axis) -> float& {
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
  };
  for (int axis = 0; axis < 3; ++axis) for (float sign : {-1.f, 1.f}) {
    const int u_axis = (axis + 1) % 3, v_axis = (axis + 2) % 3;
    for (std::size_t v = 0; v + 1 < knots.size(); ++v)
      for (std::size_t u = 0; u + 1 < knots.size(); ++u) {
        const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
        for (auto corner : std::array<std::pair<int, int>, 4>{
                 {{0, 0}, {1, 0}, {1, 1}, {0, 1}}}) {
          Vec3 p{};
          component(p, axis) = sign * half;
          component(p, u_axis) = knots[u + corner.first];
          component(p, v_axis) = knots[v + corner.second];
          const Vec3 nearest{std::clamp(p.x, -core, core),
                             std::clamp(p.y, -core, core),
                             std::clamp(p.z, -core, core)};
          const Vec3 normal = normalize(p - nearest);
          Vec3 tangent{}, bitangent{};
          component(tangent, u_axis) = 1;
          component(bitangent, v_axis) = 1;
          tangent = normalize(tangent - normal * dot(tangent, normal));
          Vertex vertex;
          vertex.position = centre + world(nearest + normal * bevel);
          vertex.normal = world(normal);
          vertex.tangent = {world(tangent),
                            dot(cross(normal, tangent), bitangent) < 0 ? -1.f : 1.f};
          vertex.uv = {knots[u + corner.first], knots[v + corner.second]};
          vertex.aux = {vertex.uv.x, vertex.uv.y, .5f, 1};
          vertex.material = material;
          mesh.add_vertex(vertex);
        }
        const auto& a = mesh.vertices[start];
        const auto& b = mesh.vertices[start + 1];
        const auto& c = mesh.vertices[start + 2];
        if (dot(cross(b.position - a.position, c.position - a.position), a.normal) > 0) {
          mesh.add_triangle(start, start + 1, start + 2);
          mesh.add_triangle(start, start + 2, start + 3);
        } else {
          mesh.add_triangle(start, start + 2, start + 1);
          mesh.add_triangle(start, start + 3, start + 2);
        }
      }
  }
}
}  // namespace environment_proof_detail

inline Scene generate_environment_proof() {
  Scene scene;
  scene.materials = make_materials();
  const auto silver = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(environment_proof_detail::production_silver());
  MaterialDesc neutral;
  neutral.name = "environment proof dry neutral floor and plinths";
  neutral.base_color = {.22f, .22f, .22f};
  neutral.roughness = .85f;
  neutral.normal_strength = 0;
  const auto floor = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(neutral);

  Vec3 civic_eye, civic_target;
  if (!shot_camera(kEnvironmentProofShot, civic_eye, civic_target))
    throw std::runtime_error("Environment proof requires the canonical civic view");
  const Vec3 forward = normalize(civic_target - civic_eye);
  const Vec3 right = normalize(cross(forward, {0, 1, 0}));
  const Vec3 up = cross(right, forward);
  const Vec3 front = -normalize(Vec3{forward.x, 0, forward.z});
  const Vec3 target{0, 4, 0};
  scene.camera_target = target;
  scene.camera_position = target - forward * 15.f;
  scene.camera_fov_degrees = shot_fov_degrees(kEnvironmentProofShot);
  const Vec3 sphere = target - right * 3.2f, cube = target + right * 3.2f;

  Emit(&scene.opaque, floor).box({0, -.15f, 0}, {11, .15f, 11});
  Emit(&scene.opaque, floor).tube({sphere.x, 0, sphere.z},
                                 {sphere.x, 2, sphere.z}, 1.05f, 32, true);
  Emit(&scene.opaque, floor).box({cube.x, 1.25f, cube.z}, {1.1f, 1.25f, 1.1f},
                                right, {0, 1, 0}, front);
  const auto first_sphere_index = scene.opaque.indices.size();
  Emit(&scene.opaque, silver).sphere(sphere, 2, 32, 64);
  // Remove collapsed pole triangles emitted by the shared sphere primitive.
  // The smooth poles and every nonzero physical face remain present.
  std::size_t write = first_sphere_index;
  for (std::size_t at = first_sphere_index; at < scene.opaque.indices.size(); at += 3) {
    const auto a = scene.opaque.indices[at], b = scene.opaque.indices[at + 1],
               c = scene.opaque.indices[at + 2];
    const auto& vertices = scene.opaque.vertices;
    if (length(cross(vertices[b].position - vertices[a].position,
                     vertices[c].position - vertices[a].position)) < 1e-7f) continue;
    scene.opaque.indices[write++] = a;
    scene.opaque.indices[write++] = b;
    scene.opaque.indices[write++] = c;
  }
  scene.opaque.indices.resize(write);
  environment_proof_detail::bevelled_cube(scene.opaque, silver, cube, right, front);

  // Same physical civic light as main's preset. Main must still choose the
  // civic environment (including sky scale 1.45 and evening factor .65).
  const float ty = std::tan(radians(scene.camera_fov_degrees) * .5f);
  scene.has_lighting_override = true;
  scene.sun_direction = normalize(forward + right * (.60f * ty * 1280.f / 720.f) +
                                  up * (.16f * ty));
  scene.sun_irradiance = {.40f, .25f, .15f};
  scene.lighting_exposure = 1.25f;
  scene.city_size = "environment proof: production civic silver";
  scene.city_radius = 20;
  scene.finalize_draws();
  return scene;
}
}  // namespace cb
