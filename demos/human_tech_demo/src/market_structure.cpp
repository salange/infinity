#include "market_structure.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cb {
namespace {
constexpr float kGround = 1.2f;
constexpr float kFront = -121.6f;
constexpr float kUpper = 23.f;
using Material = std::uint32_t;

// A panel section has rounded shoulders and a shallow convex glazed face.
// Width narrows into the bearing and spreads smoothly into the loaded blade.
struct Blade {
  Vec3 a, b, along, across;
  float length;
  explicit Blade(float low, float high) {
    const float sign = high > low ? 1.f : -1.f;
    a = {kFront, kGround + .60f, low + sign * .42f};
    b = {kFront, kUpper - .24f, high - sign * .42f};
    along = normalize(b - a);
    across = normalize(cross(Vec3{1, 0, 0}, along));
    length = cb::length(b - a);
  }
  float width(float t) const {
    const float distance = std::min(t, 1.f - t) * length;
    float s = std::clamp(distance / 2.4f, 0.f, 1.f);
    s = s * s * (3.f - 2.f * s);
    return .32f + .22f * s;
  }
  Vec3 point(float t, Vec2 section, float shrink = 0.f) const {
    const float side = section.x * std::max(.05f, width(t) - shrink);
    const float depth = section.y - std::copysign(shrink, section.y);
    // Horizontal termination planes seat the complete end section on the
    // bearing. Sliding along the blade preserves its perpendicular width.
    return lerp(a, b, t) + across * side +
           along * (-across.y * side / along.y) + Vec3{depth, 0, 0};
  }
};

std::vector<Vec2> profile() {
  std::vector<Vec2> p{{-.84f, -.22f}, {.84f, -.22f}};
  // Rounded right shoulder; full thickness remains inside the old bearing.
  for (int i = 1; i <= 5; ++i) {
    const float a = -kPi * .5f + i * kPi / 10.f;
    p.push_back({.84f + .16f * std::cos(a), -.14f + .08f * std::sin(a)});
  }
  p.push_back({1.f, .14f});
  for (int i = 1; i <= 5; ++i) {
    const float a = i * kPi / 10.f;
    p.push_back({.84f + .16f * std::cos(a), .14f + .08f * std::sin(a)});
  }
  for (int i = 1; i <= 16; ++i) {
    const float x = .84f - i * 1.68f / 16.f;
    p.push_back({x, .22f + .030f * (1.f - x * x / (.84f * .84f))});
  }
  for (int i = 1; i <= 5; ++i) {
    const float a = kPi * .5f + i * kPi / 10.f;
    p.push_back({-.84f + .16f * std::cos(a), .14f + .08f * std::sin(a)});
  }
  p.push_back({-1.f, -.14f});
  for (int i = 1; i <= 4; ++i) {
    const float a = kPi + i * kPi / 10.f;
    p.push_back({-.84f + .16f * std::cos(a), -.14f + .08f * std::sin(a)});
  }
  return p;
}

void face(Mesh &mesh, Material material, std::array<Vec3, 4> p,
          std::array<Vec3, 4> n, std::array<Vec2, 4> uv) {
  if (dot(cross(p[1] - p[0], p[3] - p[0]), n[0] + n[1] + n[2] + n[3]) < 0) {
    std::reverse(p.begin(), p.end());
    std::reverse(n.begin(), n.end());
    std::reverse(uv.begin(), uv.end());
  }
  const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  const Vec3 e1 = p[1] - p[0], e2 = p[3] - p[0];
  const Vec2 d1 = uv[1] - uv[0], d2 = uv[3] - uv[0];
  const float determinant = d1.x * d2.y - d1.y * d2.x;
  const Vec3 du = (e1 * d2.y - e2 * d1.y) * (1.f / determinant);
  const Vec3 dv = (e2 * d1.x - e1 * d2.x) * (1.f / determinant);
  for (int i = 0; i < 4; ++i) {
    Vertex v;
    v.position = p[i]; v.normal = normalize(n[i]);
    Vec3 tangent = du;
    tangent = normalize(tangent - v.normal * dot(tangent, v.normal));
    v.tangent = {tangent, dot(cross(v.normal, tangent), dv) < 0 ? -1.f : 1.f};
    v.uv = uv[i]; v.aux = {uv[i].x, uv[i].y, 0, 1}; v.material = material;
    mesh.add_vertex(v);
  }
  mesh.add_triangle(first, first + 1, first + 2);
  mesh.add_triangle(first, first + 2, first + 3);
}

void panel(Scene &scene, const Blade &blade, const std::vector<Vec2> &p,
           float begin, float end, Material material, float inset = 0.f) {
  constexpr int slices = 6;
  const auto count = p.size();
  auto normal = [&](float t, std::size_t i) {
    const auto before = (i + count - 1) % count, after = (i + 1) % count;
    const Vec3 around = blade.point(t, p[after], inset) - blade.point(t, p[before], inset);
    const Vec3 along = blade.point(t + .0001f, p[i], inset) - blade.point(t - .0001f, p[i], inset);
    Vec3 n = normalize(cross(around, along));
    const Vec2 d = p[after] - p[before];
    if (dot(n, blade.across * d.y - Vec3{1, 0, 0} * d.x) < 0) n = -n;
    return n;
  };
  for (int s = 0; s < slices; ++s) {
    const float a = begin + (end - begin) * s / slices;
    const float b = begin + (end - begin) * (s + 1) / slices;
    for (std::size_t i = 0; i < count; ++i) {
      const auto j = (i + 1) % count;
      const float u = float(i) / count * 3.1f, v = float(i + 1) / count * 3.1f;
      face(scene.opaque, material,
           {blade.point(a, p[i], inset), blade.point(a, p[j], inset),
            blade.point(b, p[j], inset), blade.point(b, p[i], inset)},
           {normal(a, i), normal(a, j), normal(b, j), normal(b, i)},
           {{{u, a * blade.length}, {v, a * blade.length},
             {v, b * blade.length}, {u, b * blade.length}}});
    }
  }
  // Closed ceramic panel returns give every expansion joint physical depth.
  for (float t : {begin, end}) {
    Emit cap(&scene.opaque, material);
    Vec3 center = lerp(blade.a, blade.b, t);
    for (std::size_t i = 0; i < count; ++i) {
      const auto j = (i + 1) % count;
      auto a = blade.point(t, p[i], inset), b = blade.point(t, p[j], inset);
      const Vec3 outward = t == begin ? Vec3{0, -1, 0} : Vec3{0, 1, 0};
      if (dot(cross(a - center, b - center), outward) < 0) std::swap(a, b);
      cap.triangle(center, a, b);
    }
  }
}

void bearing(Scene &scene, float z, bool upper) {
  const float y = upper ? kUpper - .24f : kGround;
  Emit bronze(&scene.opaque, M_BRONZE), dark(&scene.opaque, M_DARK_METAL);
  // Ground plate, recessed isolation layer and two seated ceramic sockets.
  // The same six stations transfer both diagonal loads into actual slabs.
  bronze.box({kFront, y + .065f, z}, {.44f, .065f, .97f});
  dark.box({kFront, y + .151f, z}, {.375f, .021f, .88f});
  const float top = upper ? kUpper : kGround + .60f;
  for (float side : {-1.f, 1.f}) {
    const float station = z + side * .42f;
    bronze.box({kFront, (y + .172f + top) * .5f, station},
               {.33f, (top - y - .172f) * .5f, .42f});
    // A real recessed face panel with a lip on all sides; not a decal.
    dark.box({kFront + .333f, (y + .22f + top - .06f) * .5f, station},
             {.006f, std::max(.006f, (top - y - .28f) * .5f), .29f});
    bronze.box({kFront + .342f, top - .032f, station}, {.015f, .026f, .33f});
  }
  for (float dx : {-.30f, .30f}) for (float dz : {-.79f, .79f}) {
    bronze.tube({kFront + dx, y + .13f, z + dz},
                 {kFront + dx, y + .151f, z + dz}, .071f, 24, true);
    const auto first_head = scene.opaque.vertices.size();
    bronze.frustum({kFront + dx, y + .151f, z + dz},
                    {kFront + dx, y + .207f, z + dz}, .049f, .044f, 6);
    // The shared tapered primitive retains its axial tangent. Project it onto
    // the conical side so the machined head has a valid normal-map frame.
    for (auto i = first_head; i < scene.opaque.vertices.size(); ++i) {
      auto &v = scene.opaque.vertices[i];
      Vec3 tangent{v.tangent.x, v.tangent.y, v.tangent.z};
      tangent = normalize(tangent - v.normal * dot(tangent, v.normal));
      v.tangent = {tangent, v.tangent.w};
    }
    dark.tube({kFront + dx, y + .2072f, z + dz},
               {kFront + dx, y + .209f, z + dz}, .020f, 6, true);
  }
}

} // namespace

void build_market_structure(Scene &scene, std::uint32_t ceramic) {
  // Honed street cladding retains its broad formed face under a low sun.
  // The upper skyline's satin material remains a separate finish.
  MaterialDesc street_ceramic = scene.materials.at(ceramic);
  street_ceramic.name = "honed ivory market structural cladding";
  street_ceramic.roughness = .84f;
  street_ceramic.normal_strength = .12f;
  ceramic = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(street_ceramic);
  const auto section = profile();
  for (int bay = 0; bay < 5; ++bay) {
    const float z = 65 + bay * 16.f;
    for (const auto endpoints : {Vec2{z, z + 16}, Vec2{z + 16, z}}) {
      Blade blade(endpoints.x, endpoints.y);
      // Continuous structural backing spans the gasket openings. The ceramic
      // skin is separately panelized and never floats across an unsupported gap.
      panel(scene, blade, section, 0, 1, M_DARK_METAL, .065f);
      const int pieces = static_cast<int>(std::ceil(blade.length / 2.1f));
      for (int i = 0; i < pieces; ++i) {
        const float gap = .010f / blade.length;
        const float a = float(i) / pieces + (i ? gap : 0.f);
        const float b = float(i + 1) / pieces - (i + 1 < pieces ? gap : 0.f);
        panel(scene, blade, section, a, b, ceramic);
      }
    }
  }
  for (int station = 0; station <= 5; ++station) {
    const float z = 65 + station * 16.f;
    bearing(scene, z, false);
    bearing(scene, z, true);
    // The level23 occupied floor is set back from the street. Transfer the
    // exterior seat into that real slab with a closed steel outrigger.
    const Vec3 a{kFront, 22.86f, z}, b{-136.f, 22.86f, std::clamp(z, 84.f, 132.f)};
    Emit steel(&scene.opaque, M_BRONZE);
    steel.beam(a, b, .64f, .28f, {0, 1, 0});
    const Vec3 direction = normalize(b - a);
    const Vec2 side{-direction.z * .36f, direction.x * .36f};
    const Vec2 pa{a.x, a.z}, pb{b.x, b.z};
    scene.roof_obstructions.push_back(
        {{pa + side, pb + side, pb - side, pa - side}, 22.70f, 23.01f});
  }
}
} // namespace cb
