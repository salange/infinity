#include "garden_frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cb {
namespace {
const Vec3 kUp{0, 1, 0};
constexpr float kReach = 3.7f;
constexpr std::array<float, 3> kArmAngles{-78.f, 65.f, 145.f};

Vec2 arm_direction(int arm) {
  const float angle = kArmAngles[arm] * kPi / 180;
  return {std::cos(angle), std::sin(angle)};
}

void curved_quad(Mesh &mesh, std::uint32_t material, std::array<Vec3, 4> p,
                 std::array<Vec3, 4> normals) {
  if (dot(cross(p[1] - p[0], p[3] - p[0]),
          normals[0] + normals[1] + normals[2] + normals[3]) < 0) {
    std::reverse(p.begin(), p.end());
    std::reverse(normals.begin(), normals.end());
  }
  const float width = length(p[1] - p[0]), height = length(p[3] - p[0]);
  const std::array<Vec2, 4> uv{
      {{0, 0}, {width, 0}, {width, height}, {0, height}}};
  const auto first = std::uint32_t(mesh.vertices.size());
  for (int i = 0; i < 4; ++i) {
    Vertex v;
    v.position = p[i];
    v.normal = normalize(normals[i]);
    const Vec3 tangent =
        normalize((p[1] - p[0]) - v.normal * dot(p[1] - p[0], v.normal));
    v.tangent = {tangent, 1};
    v.uv = uv[i];
    v.material = material;
    v.aux = {uv[i].x, uv[i].y, 0, 1};
    mesh.add_vertex(v);
  }
  mesh.add_triangle(first, first + 1, first + 2);
  mesh.add_triangle(first, first + 2, first + 3);
}

void turned(Mesh &mesh, std::uint32_t material, Vec3 a, Vec3 b,
            float bottom_radius, float top_radius, int sides) {
  const auto first = mesh.vertices.size();
  Emit(&mesh, material).frustum(a, b, bottom_radius, top_radius, sides, true);
  const Vec3 axis = normalize(b - a);
  // The shared primitive stores an axial side tangent, while its U coordinate
  // runs around the part. Set the real circumferential derivative locally.
  for (int i = 0; i < 2 * (sides + 1); ++i) {
    auto &vertex = mesh.vertices[first + i];
    vertex.tangent = {normalize(cross(axis, vertex.normal)), 1};
  }
  const auto lower_cap = first + 2 * (sides + 1);
  for (int i = 0; i < sides + 2; ++i)
    mesh.vertices[lower_cap + i].tangent.w = -1;
}

void journal(Scene &scene, Vec3 center, Vec3 normal, float radial_scale) {
  const Vec3 across = normalize(cross(kUp, normal));
  auto point = [&](Vec2 p, float depth) {
    return center + across * (p.x * radial_scale) + kUp * (p.y * radial_scale) +
           normal * depth;
  };
  auto radial = [](float angle) {
    return Vec2{std::cos(angle), std::sin(angle)};
  };
  constexpr int bolts = 8;
  constexpr float pitch = .720f, bore_radius = .064f, face_depth = 1.10f;
  const float half_bore_angle = std::asin(bore_radius / pitch);
  std::vector<float> angles;
  for (int i = 0; i <= 128; ++i)
    angles.push_back(i * 2 * kPi / 128);
  for (int bolt = 0; bolt < bolts; ++bolt) {
    const float angle = (bolt + .5f) * 2 * kPi / bolts;
    for (int i = 0; i <= 20; ++i)
      angles.push_back(angle - half_bore_angle +
                       i * (2 * half_bore_angle / 20));
  }
  std::sort(angles.begin(), angles.end());
  angles.erase(
      std::unique(angles.begin(), angles.end(),
                  [](float a, float b) { return std::fabs(a - b) < 1e-6f; }),
      angles.end());

  // A closed annular bearing housing has a genuine open central recess. Its
  // front annulus is generated separately so the counterbores are actual holes.
  const std::array<Vec2, 9> profile{{{.90f, .795f},
                                     {.925f, .82f},
                                     {.925f, .94f},
                                     {.88f, 1.055f},
                                     {.83f, face_depth},
                                     {.60f, face_depth},
                                     {.555f, 1.025f},
                                     {.555f, .85f},
                                     {.62f, .795f}}};
  for (std::size_t j = 0; j < profile.size(); ++j) {
    if (j == 4)
      continue;
    const Vec2 a = profile[j], b = profile[(j + 1) % profile.size()];
    const Vec2 n =
        normalize(Vec2{b.y - a.y, (a.x - b.x) * radial_scale});
    for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
      const Vec2 r0 = radial(angles[i]), r1 = radial(angles[i + 1]);
      auto surface_normal = [&](Vec2 r) {
        return (across * r.x + kUp * r.y) * n.x + normal * n.y;
      };
      const Vec3 n0 = surface_normal(r0), n1 = surface_normal(r1);
      curved_quad(scene.opaque, M_BRONZE,
                  {point(r0 * a.x, a.y), point(r1 * a.x, a.y),
                   point(r1 * b.x, b.y), point(r0 * b.x, b.y)},
                  {n0, n1, n1, n0});
    }
  }
  auto radial_bore = [&](float angle, float bolt_angle) {
    const float delta = angle - bolt_angle;
    const float along = pitch * std::cos(delta),
                sideways = pitch * std::sin(delta);
    const float half = std::sqrt(
        std::max(0.f, bore_radius * bore_radius - sideways * sideways));
    return Vec2{along - half, along + half};
  };
  auto cap_strip = [&](float a, float b, float inner_a, float inner_b,
                       float outer_a, float outer_b) {
    curved_quad(scene.opaque, M_BRONZE,
                {point(radial(a) * inner_a, face_depth),
                 point(radial(a) * outer_a, face_depth),
                 point(radial(b) * outer_b, face_depth),
                 point(radial(b) * inner_b, face_depth)},
                {normal, normal, normal, normal});
  };
  for (std::size_t i = 0; i + 1 < angles.size(); ++i) {
    const float a = angles[i], b = angles[i + 1], middle = (a + b) * .5f;
    const int bolt = std::min(bolts - 1, int(middle / (2 * kPi / bolts)));
    const float bolt_angle = (bolt + .5f) * 2 * kPi / bolts;
    if (std::fabs(middle - bolt_angle) >= half_bore_angle) {
      cap_strip(a, b, .60f, .60f, .83f, .83f);
    } else {
      const Vec2 ra = radial_bore(a, bolt_angle),
                 rb = radial_bore(b, bolt_angle);
      cap_strip(a, b, .60f, .60f, ra.x, rb.x);
      cap_strip(a, b, ra.y, rb.y, .83f, .83f);
    }
  }

  Emit bronze(&scene.opaque, M_BRONZE), dark(&scene.opaque, M_DARK_METAL);
  turned(scene.opaque, M_DARK_METAL, center + normal * .642f,
         center + normal * .83f, .96f * radial_scale, .96f * radial_scale, 128);
  // Separate central hub, seated on the bearing backing. The radial gap to
  // the outer housing exposes real depth rather than a painted dark circle.
  turned(scene.opaque, M_BRONZE, center + normal * .81f, center + normal * .90f,
         .51f * radial_scale, .48f * radial_scale, 96);
  turned(scene.opaque, M_BRONZE, center + normal * .90f,
         center + normal * 1.055f, .455f * radial_scale, .455f * radial_scale,
         96);
  turned(scene.opaque, M_BRONZE, center + normal * 1.055f,
         center + normal * 1.095f, .455f * radial_scale, .415f * radial_scale,
         96);
  dark.torus(center + normal * .915f, normal, .505f * radial_scale,
             .025f * radial_scale, 96, 12);

  for (int bolt = 0; bolt < bolts; ++bolt) {
    const float angle = (bolt + .5f) * 2 * kPi / bolts;
    const Vec2 origin = radial(angle) * pitch;
    std::vector<float> samples;
    for (float theta : angles)
      if (theta >= angle - half_bore_angle - 1e-6f &&
          theta <= angle + half_bore_angle + 1e-6f)
        samples.push_back(theta);
    std::vector<Vec2> bore;
    for (float theta : samples)
      bore.push_back(radial(theta) * radial_bore(theta, angle).y);
    for (std::size_t i = samples.size() - 1; i-- > 1;)
      bore.push_back(radial(samples[i]) * radial_bore(samples[i], angle).x);
    for (std::size_t i = 0; i < bore.size(); ++i) {
      const Vec2 a = bore[i], b = bore[(i + 1) % bore.size()];
      const Vec2 da = normalize(a - origin), db = normalize(b - origin);
      const Vec2 aa = origin + da * .048f, bb = origin + db * .048f;
      const Vec3 na =
          normalize(-(across * da.x + kUp * da.y) * .041f +
                    normal * (.016f * radial_scale));
      const Vec3 nb =
          normalize(-(across * db.x + kUp * db.y) * .041f +
                    normal * (.016f * radial_scale));
      curved_quad(scene.opaque, M_BRONZE,
                  {point(a, face_depth), point(b, face_depth),
                   point(bb, 1.059f), point(aa, 1.059f)},
                  {na, nb, nb, na});
      dark.triangle(point(origin, 1.059f), point(aa, 1.059f),
                    point(bb, 1.059f));
    }
    const Vec3 seat = point(origin, 1.059f);
    turned(scene.opaque, M_BRONZE, seat - normal * .065f, seat + normal * .020f,
           .028f * radial_scale, .028f * radial_scale, 12);
    // A closed six-sided head with a recessed hex socket. It seats on the
    // counterbore floor and stays within the former cap's depth envelope.
    for (int edge = 0; edge < 6; ++edge) {
      const Vec2 a = radial(edge * kPi / 3 + .16f),
                 b = radial((edge + 1) * kPi / 3 + .16f);
      const Vec3 outer_a = point(origin + a * .046f, 1.128f),
                 outer_b = point(origin + b * .046f, 1.128f),
                 inner_a = point(origin + a * .020f, 1.128f),
                 inner_b = point(origin + b * .020f, 1.128f);
      const Vec3 side = normalize(across * (a.x + b.x) + kUp * (a.y + b.y));
      curved_quad(scene.opaque, M_BRONZE,
                  {point(origin + a * .046f, 1.064f),
                   point(origin + b * .046f, 1.064f), outer_b, outer_a},
                  {side, side, side, side});
      curved_quad(scene.opaque, M_BRONZE, {outer_a, outer_b, inner_b, inner_a},
                  {normal, normal, normal, normal});
      curved_quad(scene.opaque, M_DARK_METAL,
                  {inner_a, inner_b, point(origin + b * .020f, 1.108f),
                   point(origin + a * .020f, 1.108f)},
                  {-side, -side, -side, -side});
      dark.triangle(point(origin, 1.108f), point(origin + a * .020f, 1.108f),
                    point(origin + b * .020f, 1.108f));
      bronze.triangle(point(origin, 1.064f), point(origin + b * .046f, 1.064f),
                      point(origin + a * .046f, 1.064f));
    }
  }
}

// A closed formed seat connects the broad web to the canted service bearing.
// Its planar root mates with the casting face; its other end supports the
// complete bearing backplate. The housing is never a floating rotated disc.
void journal_seat(Scene &scene, Vec3 joint, Vec3 front, Vec3 service,
                  std::uint32_t ceramic) {
  constexpr int sides = 128, rings = 12;
  const Vec3 original_across = normalize(cross(kUp, front));
  const Vec3 service_across = normalize(cross(kUp, service));
  const Vec3 start = joint + front * .64f;
  const Vec3 finish = joint + front * .55f + service * .642f;
  auto point = [&](float t, float angle) {
    const Vec3 across = normalize(lerp(original_across, service_across, t));
    const float radius =
        1.43f * (1 - t) + 1.225f * t + .055f * std::sin(kPi * t);
    return lerp(start, finish, t) +
           (across * std::cos(angle) + kUp * std::sin(angle)) * radius;
  };
  auto normal = [&](float t, float angle) {
    // Differentiate the small local profile analytically. World-coordinate
    // finite differences lose precision across this shallow formed shoulder.
    const Vec3 raw = lerp(original_across, service_across, t);
    const Vec3 across = normalize(raw),
               delta = service_across - original_across;
    const Vec3 derivative = (delta - across * dot(across, delta)) * (1.f / length(raw));
    const float radius =
        1.43f * (1 - t) + 1.225f * t + .055f * std::sin(kPi * t);
    const float radius_derivative =
        1.225f - 1.43f + .055f * kPi * std::cos(kPi * t);
    const Vec3 radial = across * std::cos(angle) + kUp * std::sin(angle);
    const Vec3 around =
        (-across * std::sin(angle) + kUp * std::cos(angle)) * radius;
    const Vec3 along = front * (.55f - .64f) + service * .642f +
                       derivative * (std::cos(angle) * radius) +
                       radial * radius_derivative;
    Vec3 n = normalize(cross(around, along));
    if (dot(n, radial) < 0)
      n = -n;
    return n;
  };
  for (int ring = 0; ring < rings; ++ring)
    for (int side = 0; side < sides; ++side) {
      const float a = float(ring) / rings, b = float(ring + 1) / rings;
      const float u = side * 2 * kPi / sides, v = (side + 1) * 2 * kPi / sides;
      curved_quad(scene.opaque, ceramic,
                  {point(a, u), point(a, v), point(b, v), point(b, u)},
                  {normal(a, u), normal(a, v), normal(b, v), normal(b, u)});
    }
  Emit cap(&scene.opaque, ceramic);
  for (int end = 0; end < 2; ++end)
    for (int side = 0; side < sides; ++side) {
      const Vec3 center = end ? finish : start,
                 outward = end ? service : -front;
      Vec3 a = point(float(end), side * 2 * kPi / sides);
      Vec3 b = point(float(end), (side + 1) * 2 * kPi / sides);
      if (dot(cross(a - center, b - center), outward) < 0)
        std::swap(a, b);
      cap.triangle(center, a, b);
    }
}

// The fork is one closed, bevelled casting. Concave curved shoulders spread
// each load into the next blade; the journal sits flush inside the broad face.
void casting(Scene &scene, Vec3 center, std::uint32_t ceramic, Vec3 front) {
  const Vec3 across = normalize(cross(kUp, front));
  std::vector<Vec2> outline;
  std::array<std::array<std::size_t, 2>, 3> terminals;
  constexpr float width = .95f, root = 2.45f;
  for (int arm = 0; arm < 3; ++arm) {
    const Vec2 d = arm_direction(arm), p{-d.y, d.x};
    const Vec2 nd = arm_direction((arm + 1) % 3), np{-nd.y, nd.x};
    terminals[arm] = {outline.size(), outline.size() + 1};
    outline.push_back(d * kReach - p * width);
    outline.push_back(d * kReach + p * width);
    const Vec2 a = d * root + p * width, b = normalize(d + nd) * 1.45f,
               c = nd * root - np * width;
    outline.push_back(a);
    for (int i = 1; i <= 18; ++i) {
      const float t = i / 18.f, u = 1 - t;
      outline.push_back(a * (u * u) + b * (2 * u * t) + c * (t * t));
    }
  }
  if (plan_area(outline) < 0) {
    std::reverse(outline.begin(), outline.end());
    for (auto &terminal : terminals)
      for (auto &i : terminal)
        i = outline.size() - 1 - i;
  }
  auto inset = plan_offset(outline, -.076f);
  // An attached blade shares the casting's full terminal section. Bevel
  // the exposed shoulder edges, but do not recess the three mating planes.
  for (int arm = 0; arm < 3; ++arm)
    for (int edge = 0; edge < 2; ++edge) {
      const auto i = terminals[arm][edge];
      const Vec2 d = arm_direction(arm);
      inset[i] = inset[i] + d * (kReach - dot(inset[i], d));
    }
  auto world = [&](Vec2 p, float z) {
    return center + across * p.x + kUp * p.y + front * z;
  };
  const auto cap = triangulate(inset);
  for (float side : {-1.f, 1.f}) {
    Emit face(&scene.opaque, ceramic);
    for (std::size_t i = 0; i < cap.size(); i += 3) {
      const Vec3 a = world(inset[cap[i]], side * .64f),
                 b = world(inset[cap[i + 1]], side * .64f),
                 c = world(inset[cap[i + 2]], side * .64f);
      if (side > 0)
        face.triangle(a, b, c);
      else
        face.triangle(c, b, a);
    }
  }
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const std::size_t j = (i + 1) % outline.size();
    auto vertex_normal = [&](std::size_t n) {
      const Vec2 before = normalize(
          outline[n] - outline[(n + outline.size() - 1) % outline.size()]);
      const Vec2 after =
          normalize(outline[(n + 1) % outline.size()] - outline[n]);
      const Vec2 tangent = normalize(before + after);
      return normalize(across * tangent.y - kUp * tangent.x);
    };
    const Vec3 ni = vertex_normal(i), nj = vertex_normal(j);
    curved_quad(scene.opaque, ceramic,
                {world(outline[i], -.4864f), world(outline[j], -.4864f),
                 world(outline[j], .4864f), world(outline[i], .4864f)},
                {ni, nj, nj, ni});
    for (float side : {-1.f, 1.f})
      curved_quad(scene.opaque, ceramic,
                  {world(outline[i], side * .4864f),
                   world(outline[j], side * .4864f),
                   world(inset[j], side * .64f), world(inset[i], side * .64f)},
                  {ni + front * side, nj + front * side, nj + front * side,
                   ni + front * side});
  }
  // A fixed service axis opens toward the supported western balcony. It is a
  // property of the manufactured node, independent of every camera and view.
  const Vec3 service = normalize(front * std::cos(radians(20.f)) +
                                 across * std::sin(radians(20.f)));
  for (float side : {-1.f, 1.f}) {
    journal_seat(scene, center, front * side, service * side, ceramic);
    journal(scene, center + front * (side * .55f), service * side,
            1.15f / .925f);
  }
}

struct Curve {
  Vec3 a, b, c, d;
  Vec3 shoulder_relief{};
  float relief(float t) const {
    if (t <= 0 || t >= .44f) return 0;
    const float s = std::sin(kPi*t/.44f);
    return s*s*s*s;
  }
  float relief_derivative(float t) const {
    if (t <= 0 || t >= .44f) return 0;
    const float angle = kPi*t/.44f, s = std::sin(angle);
    return 4*s*s*s*std::cos(angle)*kPi/.44f;
  }
  Vec3 point(float t) const {
    const float u = 1 - t;
    return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) +
           d * (t * t * t) + shoulder_relief*relief(t);
  }
  Vec3 tangent(float t) const {
    const float u = 1 - t;
    return normalize((b - a) * (3 * u * u) + (c - b) * (6 * u * t) +
                     (d - c) * (3 * t * t) + shoulder_relief*relief_derivative(t));
  }
};

void blade(Scene &scene, const Curve &curve, std::uint32_t ceramic,
           Vec3 face_normal) {
  const std::array<Vec2, 8> polygon{{{-1, -.76f},
                                     {-.92f, -1},
                                     {.92f, -1},
                                     {1, -.76f},
                                     {1, .76f},
                                     {.92f, 1},
                                     {-.92f, 1},
                                     {-1, .76f}}};
  struct SectionPoint {
    Vec2 terminal, rounded;
  };
  std::vector<SectionPoint> section;
  // Corresponding samples preserve the old exact eight-sided mating section
  // at both structural terminals. Away from them, the corners round inward
  // and the broad ceramic skins acquire a shallow manufactured crown.
  for (int i = 0; i < 8; ++i) {
    const Vec2 p = polygon[i], before = normalize(p - polygon[(i + 7) % 8]),
               after = normalize(polygon[(i + 1) % 8] - p);
    const Vec2 a = p - before * .035f, b = p + after * .035f;
    for (int j = 0; j < 4; ++j) {
      const float t = j * .25f, u = 1 - t;
      const Vec2 old = t <= .5f ? a * (1 - 2 * t) + p * (2 * t)
                                : p * (2 - 2 * t) + b * (2 * t - 1);
      section.push_back({old, a * (u * u) + p * (2 * u * t) + b * (t * t)});
    }
    const Vec2 next = polygon[(i + 1) % 8] - after * .035f;
    const int steps = i == 1 || i == 5 ? 6 : 2;
    for (int j = 0; j < steps; ++j) {
      const Vec2 q = b + (next - b) * (float(j) / steps);
      section.push_back({q, q});
    }
  }
  float length_m = 0;
  Vec3 previous = curve.a;
  for (int i = 1; i <= 128; ++i) {
    const Vec3 p = curve.point(i / 128.f);
    length_m += length(p - previous);
    previous = p;
  }
  auto smooth = [](float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
  };
  const int panels = std::max(1, int(std::ceil(length_m / 3.4f)));
  auto ring = [&](float t, float inset) {
    const Vec3 axis = curve.tangent(t);
    const Vec3 front = normalize(face_normal - axis * dot(face_normal, axis));
    const Vec3 across = normalize(cross(axis, front));
    const float w =
        (.95f * (1 - t) + .72f * t + .14f * std::sin(kPi * t)) * inset;
    const float depth = (.64f * (1 - t) + .47f * t) * inset;
    const float blend = smooth(std::min(t, 1 - t) * length_m / .8f);
    std::vector<Vec3> points;
    points.reserve(section.size());
    for (const auto &sample : section) {
      Vec2 p = sample.terminal * (1 - blend) + sample.rounded * blend;
      const float shoulder = smooth((std::fabs(p.y) - .76f) / .24f);
      p.y *= 1 - blend * .060f * (p.x * p.x / (.92f * .92f)) * shoulder;
      points.push_back(curve.point(t) + across * (p.x * w) +
                       front * (p.y * depth));
    }
    return points;
  };
  auto span = [&](float a, float b, std::uint32_t material, float inset,
                  bool caps) {
    const int steps = std::max(2, int(std::ceil((b - a) * length_m / .15f)));
    auto shell_ring = [&](float t, bool inside) {
      if (inside)
        return ring(t, .964f);
      // A 45 mm formed end return retreats into the existing envelope. The
      // first and last structural contacts retain their original full section.
      const float left = a > 0 ? (t - a) * length_m : 1.f;
      const float right = b < 1 ? (b - t) * length_m : 1.f;
      const float lip =
          caps ? 1 - .018f * (1 - smooth(std::min(left, right) / .045f)) : 1;
      return ring(t, inset * lip);
    };
    auto normals = [&](float t, bool inside, const std::vector<Vec3> &here) {
      const auto before = shell_ring(std::max(a, t - .0001f), inside),
                 after = shell_ring(std::min(b, t + .0001f), inside);
      std::vector<Vec3> result;
      result.reserve(here.size());
      const Vec3 center = curve.point(t);
      for (std::size_t i = 0; i < here.size(); ++i) {
        const std::size_t next = (i + 1) % here.size(),
                          previous = (i + here.size() - 1) % here.size();
        Vec3 n =
            normalize(cross(here[next] - here[previous], after[i] - before[i]));
        if (dot(n, here[i] - center) < 0)
          n = -n;
        result.push_back(inside ? -n : n);
      }
      return result;
    };
    for (int s = 0; s < steps; ++s) {
      const float t0 = a + (b - a) * s / steps,
                  t1 = a + (b - a) * (s + 1) / steps;
      for (int layer = 0; layer < (caps ? 2 : 1); ++layer) {
        const bool inside = layer == 1;
        const auto p = shell_ring(t0, inside), q = shell_ring(t1, inside);
        const auto np = normals(t0, inside, p), nq = normals(t1, inside, q);
        for (std::size_t i = 0; i < p.size(); ++i) {
          const std::size_t j = (i + 1) % p.size();
          curved_quad(scene.opaque, material, {p[i], p[j], q[j], q[i]},
                      {np[i], np[j], nq[j], nq[i]});
        }
      }
    }
    if (caps)
      for (float t : {a, b}) {
        const auto outer = shell_ring(t, false), inner = shell_ring(t, true);
        const Vec3 n = curve.tangent(t) * (t == a ? -1.f : 1.f);
        for (std::size_t i = 0; i < outer.size(); ++i) {
          const std::size_t j = (i + 1) % outer.size();
          curved_quad(scene.opaque, material,
                      {outer[i], outer[j], inner[j], inner[i]}, {n, n, n, n});
        }
      }
  };
  // The continuous structural backing is separate from closed ceramic shells.
  // Genuine 24 mm recessed joints reveal it between the formed panel returns.
  span(0, 1, M_DARK_METAL, .96f, false);
  for (int panel = 0; panel < panels; ++panel) {
    const float gap = .012f / std::max(length_m, 1.f);
    span(panel / float(panels) + (panel ? gap : 0),
         (panel + 1) / float(panels) - (panel + 1 < panels ? gap : 0), ceramic,
         1, true);
  }
}
} // namespace

void build_sculpted_garden_frame(Scene &scene, Vec3 joint, Vec3 upper,
                                 Vec3 upper_anchor, Vec3 lower,
                                 std::uint32_t ceramic, Vec3 front) {
  front = normalize(Vec3{front.x, 0, front.z});
  const Vec3 across = normalize(cross(kUp, front));
  // The city supplies the primary vertical attachment first and the
  // companion attachment second; their roles are structural, not sorted
  // by the current view or the orientation of the journal face.
  casting(scene, joint, ceramic, front);
  auto direction = [&](int arm) {
    const Vec2 d = arm_direction(arm);
    return across * d.x + kUp * d.y;
  };
  const Vec3 left = direction(2), right = direction(1), down = direction(0);
  const Vec3 a = joint + right * kReach, b = joint + left * kReach,
             d = joint + down * kReach;
  // A shallow return toward the tower clears the existing fig's real limbs.
  // The smooth relief has zero endpoint derivatives, retaining both exact
  // structural contacts and the manufactured 65-degree departure tangent.
  blade(scene, {a, a + right * 8, upper - kUp * 8, upper,
                -front * .65f - across * 1.2f}, ceramic, front);
  blade(scene, {b, b + left * 6, upper_anchor - kUp * 8, upper_anchor}, ceramic,
        front);
  const Vec3 towards_joint =
      normalize(Vec3{joint.x - lower.x, 0, joint.z - lower.z}) * 11.3f;
  blade(scene, {d, d + down * 7, lower + towards_joint + kUp * 2, lower},
        ceramic, front);
}
} // namespace cb
