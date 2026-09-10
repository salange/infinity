#include "landing_canopies.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace cb {
namespace {
const Vec3 kUp{0, 1, 0};
using Material = std::uint32_t;

void quad(Mesh& mesh, Material material, std::array<Vec3, 4> p,
          std::array<Vec3, 4> normal, std::array<Vec2, 4> uv) {
  if (dot(cross(p[1] - p[0], p[2] - p[0]), normal[0] + normal[1] + normal[2]) < 0) {
    std::swap(p[1], p[3]); std::swap(normal[1], normal[3]); std::swap(uv[1], uv[3]);
  }
  const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
  const Vec3 edge = p[1] - p[0], across = p[3] - p[0];
  const Vec2 du = uv[1] - uv[0], dv = uv[3] - uv[0];
  const float determinant = du.x * dv.y - du.y * dv.x;
  const Vec3 derivative_u = std::abs(determinant) > 1e-8f
                                ? (edge * dv.y - across * du.y) * (1.f / determinant) : edge;
  const Vec3 derivative_v = std::abs(determinant) > 1e-8f
                                ? (across * du.x - edge * dv.x) * (1.f / determinant) : across;
  for (int i = 0; i < 4; ++i) {
    Vertex v; v.position = p[i]; v.normal = normalize(normal[i]);
    Vec3 tangent = derivative_u - v.normal * dot(derivative_u, v.normal);
    if (length(tangent) < .00001f) {
      tangent = cross(v.normal, std::abs(v.normal.y) < .90f ? kUp : Vec3{1, 0, 0});
    }
    tangent = normalize(tangent);
    v.tangent = {tangent, dot(cross(v.normal, tangent), derivative_v) < 0 ? -1.f : 1.f};
    v.uv = uv[i]; v.aux = {uv[i].x, uv[i].y, 0, 1}; v.material = material;
    mesh.add_vertex(v);
  }
  mesh.add_triangle(base, base + 1, base + 2);
  mesh.add_triangle(base, base + 2, base + 3);
}

Vec3 radial(Vec2 centre, float angle, float radius, float y) {
  return {centre.x + std::cos(angle) * radius, y, centre.y + std::sin(angle) * radius};
}

void end_return(Mesh& mesh, Material material, const std::vector<Vec2>& profile,
                Vec2 centre, float angle, float top, bool last) {
  const Vec3 outward = Vec3{-std::sin(angle), 0, std::cos(angle)} * (last ? 1.f : -1.f);
  // This section is star-shaped about its interior bearing centre. A fan
  // avoids nearly collinear ear-clipping diagonals after world-float rounding.
  const Vec3 bearing = radial(centre, angle, 25.025f, top - .09f);
  for (std::size_t i = 0; i < profile.size(); ++i) {
    const auto p = profile[i], q = profile[(i + 1) % profile.size()];
    Vec3 a = bearing, b = radial(centre, angle, p.x, top + p.y),
         c = radial(centre, angle, q.x, top + q.y);
    if (dot(cross(b - a, c - a), outward) < 0) std::swap(b, c);
    Emit(&mesh, material).triangle(a, b, c);
  }
}

void annular_panel(Scene& scene, Vec2 centre, float top, float begin, float end) {
  // Closed manufactured section, contained within the original r23.9..26.15,
  // top..top-.18 envelope. Crown and recessed underside are actual surfaces.
  std::vector<Vec2> profile;
  constexpr int samples = 16;
  for (int i = 0; i <= samples; ++i) {
    const float t = float(i) / samples;
    profile.push_back({23.9f + 2.17f * t, -.045f * std::pow(2 * t - 1, 2.f)});
  }
  profile.push_back({26.15f, -.105f});
  profile.push_back({26.12f, -.16f});
  profile.push_back({26.05f, -.18f});
  for (int i = 1; i <= samples; ++i) {
    const float t = 1 - float(i) / samples;
    profile.push_back({23.9f + 2.15f * t, -.18f + .055f * std::pow(std::sin(kPi * t), 2.f)});
  }
  const int divisions = std::max(2, static_cast<int>(std::ceil((end - begin) / radians(.65f))));
  for (int angular = 0; angular < divisions; ++angular) {
    const float a = begin + (end - begin) * angular / divisions;
    const float b = begin + (end - begin) * (angular + 1) / divisions;
    for (std::size_t j = 0; j < profile.size(); ++j) {
      const auto p = profile[j], q = profile[(j + 1) % profile.size()];
      const Vec2 d = q - p;
      auto normal = [&](float angle) {
        return Vec3{-std::cos(angle) * d.y, d.x, -std::sin(angle) * d.y};
      };
      quad(scene.opaque, M_BRONZE,
           {radial(centre, a, p.x, top + p.y), radial(centre, b, p.x, top + p.y),
            radial(centre, b, q.x, top + q.y), radial(centre, a, q.x, top + q.y)},
           {normal(a), normal(b), normal(b), normal(a)},
           {Vec2{a * 25, float(j) * .15f}, Vec2{b * 25, float(j) * .15f},
            Vec2{b * 25, float(j + 1) * .15f}, Vec2{a * 25, float(j + 1) * .15f}});
    }
  }
  end_return(scene.opaque, M_BRONZE, profile, centre, begin, top, false);
  end_return(scene.opaque, M_BRONZE, profile, centre, end, top, true);
}

void pavilion_overhang(Scene& scene, float top) {
  const Vec2 centre{224, 144};
  constexpr int panels = 12;
  const float first = radians(144), last = radians(210);
  const float half_joint = .006f / 25.f;
  for (int panel = 0; panel < panels; ++panel) {
    const float a = first + (last - first) * panel / panels + (panel ? half_joint : 0);
    const float b = first + (last - first) * (panel + 1) / panels - (panel + 1 < panels ? half_joint : 0);
    annular_panel(scene, centre, top, a, b);
  }
  Emit bronze(&scene.opaque, M_BRONZE), dark(&scene.opaque, M_DARK_METAL);
  // A closed inner bearing follows the actual facade, tying every shell panel
  // to the existing radius23.45 structural columns at150/180/210 degrees.
  for (int i = 0; i < 88; ++i) {
    const float a = first + (last - first) * i / 88;
    const float b = first + (last - first) * (i + 1) / 88;
    dark.beam(radial(centre, a, 23.94f, top - .115f),
              radial(centre, b, 23.94f, top - .115f), .12f, .10f);
  }
  for (float degrees : {150.f, 180.f, 210.f}) {
    const float angle = radians(degrees);
    bronze.beam(radial(centre, angle, 23.45f, top - .125f),
                radial(centre, angle, 25.73f, top - .125f), .105f, .10f);
  }
}

Vec3 ellipse(Vec2 centre, Vec2 half, float rho, float angle, float y) {
  return {centre.x + half.x * rho * std::cos(angle), y,
          centre.y + half.y * rho * std::sin(angle)};
}
void shade_disk(Scene& scene, Vec2 centre, Vec2 half, float top, bool upper) {
  constexpr int rings = 20, sides = 120;
  auto y = [&](float rho) { return top + (upper ? -.12f * rho * rho : -.21f - .04f * rho * rho); };
  auto normal = [&](float rho, float angle) {
    const float derivative = upper ? .24f : .08f;
    Vec3 n{derivative * rho * std::cos(angle) / half.x, 1,
           derivative * rho * std::sin(angle) / half.y};
    return upper ? n : n * -1.f;
  };
  const Material material = upper ? M_BRONZE : M_PANEL_WARM;
  for (int ring = 0; ring < rings; ++ring) {
    const float r0 = .985f * ring / rings, r1 = .985f * (ring + 1) / rings;
    for (int side = 0; side < sides; ++side) {
      const float a = side * 2 * kPi / sides, b = (side + 1) * 2 * kPi / sides;
      if (ring == 0) {
        Vec3 p = ellipse(centre, half, 0, 0, y(0));
        Vec3 q = ellipse(centre, half, r1, a, y(r1));
        Vec3 r = ellipse(centre, half, r1, b, y(r1));
        if (dot(cross(q - p, r - p), normal(0, 0)) < 0) std::swap(q, r);
        const auto base = scene.opaque.vertices.size();
        Emit(&scene.opaque, material).triangle(p, q, r);
        for (std::size_t i = base; i < scene.opaque.vertices.size(); ++i) {
          auto& vertex = scene.opaque.vertices[i];
          const Vec3 old_tangent = vertex.tangent.xyz();
          const Vec3 old_bitangent = cross(vertex.normal, old_tangent) * vertex.tangent.w;
          const float x = (vertex.position.x - centre.x) / half.x;
          const float z = (vertex.position.z - centre.y) / half.y;
          vertex.normal = normalize(normal(std::sqrt(x * x + z * z), std::atan2(z, x)));
          const Vec3 tangent = normalize(old_tangent - vertex.normal * dot(old_tangent, vertex.normal));
          vertex.tangent = {tangent, dot(cross(vertex.normal, tangent), old_bitangent) < 0 ? -1.f : 1.f};
        }
      } else {
        quad(scene.opaque, material,
             {ellipse(centre, half, r0, a, y(r0)), ellipse(centre, half, r0, b, y(r0)),
              ellipse(centre, half, r1, b, y(r1)), ellipse(centre, half, r1, a, y(r1))},
             {normal(r0, a), normal(r0, b), normal(r1, b), normal(r1, a)},
             {Vec2{half.x * r0 * std::cos(a), half.y * r0 * std::sin(a)},
              Vec2{half.x * r0 * std::cos(b), half.y * r0 * std::sin(b)},
              Vec2{half.x * r1 * std::cos(b), half.y * r1 * std::sin(b)},
              Vec2{half.x * r1 * std::cos(a), half.y * r1 * std::sin(a)}});
      }
    }
  }
}

void freestanding_shade(Scene& scene, Vec2 centre, Vec2 half, float top) {
  constexpr float floor = 34;
  shade_disk(scene, centre, half, top, true);
  shade_disk(scene, centre, half, top, false);
  // Four joined returns form the rolled edge between the crowned metal skin
  // and curved timber underside. Both ring seams share exact disk positions.
  const std::array<Vec2, 4> section{{{.985f, -.12f * .985f * .985f},
                                     {1, -.15f}, {1, -.20f},
                                     {.985f, -.21f - .04f * .985f * .985f}}};
  for (int side = 0; side < 120; ++side) {
    const float a = side * 2 * kPi / 120, b = (side + 1) * 2 * kPi / 120;
    for (int i = 0; i < 3; ++i) {
      const auto p = section[i], q = section[i + 1];
      const Vec2 d = q - p;
      auto normal = [&](float angle) {
        return Vec3{-d.y * std::cos(angle) / half.x, d.x,
                     -d.y * std::sin(angle) / half.y};
      };
      quad(scene.opaque, M_BRONZE,
           {ellipse(centre, half, p.x, a, top + p.y), ellipse(centre, half, p.x, b, top + p.y),
            ellipse(centre, half, q.x, b, top + q.y), ellipse(centre, half, q.x, a, top + q.y)},
           {normal(a), normal(b), normal(b), normal(a)},
           {Vec2{a * half.x, float(i)}, Vec2{b * half.x, float(i)},
            Vec2{b * half.x, float(i + 1)}, Vec2{a * half.x, float(i + 1)}});
    }
  }
  Emit bronze(&scene.opaque, M_BRONZE);
  for (float sign : {-1.f, 1.f}) {
    const Vec2 column = centre + Vec2{sign * half.x * .55f, 0};
    const auto first = scene.opaque.vertices.size();
    bronze.frustum({column.x, floor, column.y}, {column.x, top - .20f, column.y}, .18f, .13f, 16);
    for (std::size_t i = first; i < scene.opaque.vertices.size(); ++i) {
      auto& v = scene.opaque.vertices[i];
      if (std::abs(v.normal.y) < .99f) v.tangent = {normalize(cross(kUp, v.normal)), 1};
      else if (v.normal.y < 0) v.tangent.w = -1;
    }
    Emit(&scene.opaque, M_LOBBY_LIGHT).box({column.x, top - .28f, column.y}, {.32f, .025f, .32f});
    scene.lights.push_back({{column.x, top - .50f, column.y}, 8, {1, .76f, .48f}, 3.5f});
  }
  bronze.beam({centre.x - half.x * .55f, top - .223f, centre.y},
              {centre.x + half.x * .55f, top - .223f, centre.y}, .13f, .05f);
  for (float sign : {-1.f, 1.f}) for (float branch : {-1.f, 1.f}) {
    const Vec3 root{centre.x + sign * half.x * .55f, top - .22f, centre.y};
    const Vec3 end{centre.x + sign * half.x * .56f, top - .22f, centre.y + branch * half.y * .65f};
    bronze.beam(root, end, .10f, .06f);
  }
}
}  // namespace

void stage_landing_canopies(Scene& scene) {
  for (int floor = 0; floor < 3; ++floor) pavilion_overhang(scene, 40.6f + floor * 7.2f);
  freestanding_shade(scene, {186, 132}, {9, 4.5f}, 40.2f);
  freestanding_shade(scene, {201, 126}, {6.7f, 3.8f}, 40.45f);
  freestanding_shade(scene, {253, 174}, {5.4f, 3.0f}, 39.8f);
}
}  // namespace cb
