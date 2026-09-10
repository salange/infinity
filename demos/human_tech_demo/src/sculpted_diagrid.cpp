#include "sculpted_diagrid.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace cb {
namespace {
constexpr float reach = 3.1f, node_width = .83f, mid_width = .625f,
                bevel = .23f;
// The existing loggia guards remain behind the original -0.28 m rear datum.
// All additional structural depth grows outward from that fixed plane.
constexpr float front = 1.24f, rear = -.28f;
Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }
void quad(Mesh &mesh, std::uint32_t material, std::array<Vec3, 4> p,
          std::array<Vec3, 4> n) {
  if (dot(cross(p[1] - p[0], p[3] - p[0]), n[0] + n[1] + n[2] + n[3]) < 0) {
    std::reverse(p.begin(), p.end());
    std::reverse(n.begin(), n.end());
  }
  auto first = static_cast<std::uint32_t>(mesh.vertices.size());
  float width = length(p[1] - p[0]), height = length(p[3] - p[0]);
  std::array<Vec2, 4> uv{{{0, 0}, {width, 0}, {width, height}, {0, height}}};
  for (int i = 0; i < 4; ++i) {
    Vertex v;
    v.position = p[i];
    v.normal = normalize(n[i]);
    auto d = p[1] - p[0];
    v.tangent = {normalize(d - v.normal * dot(d, v.normal)), 1};
    v.uv = uv[i];
    v.material = material;
    v.aux = {uv[i].x, uv[i].y, 0, 1};
    mesh.add_vertex(v);
  }
  mesh.add_triangle(first, first + 1, first + 2);
  mesh.add_triangle(first, first + 2, first + 3);
}
struct Surface {
  const SculptedDiagridSpec &spec;
  float radius;
  Vec3 point(Vec3 p) const {
    float t = p.y / spec.height,
          r = spec.base_radius - spec.taper_metres * t * t + p.z,
          a = -p.x / radius;
    return {spec.centre.x + r * std::cos(a), spec.base_y + p.y,
            spec.centre.y + r * std::sin(a)};
  }
  void append(Mesh &target, Mesh &local) const {
    auto first = static_cast<std::uint32_t>(target.vertices.size());
    for (auto v : local.vertices) {
      float t = v.position.y / spec.height,
            r = spec.base_radius - spec.taper_metres * t * t + v.position.z,
            slope = -2 * spec.taper_metres * t / spec.height,
            a = -v.position.x / radius;
      Vec3 radial{std::cos(a), 0, std::sin(a)},
          clockwise{std::sin(a), 0, -std::cos(a)};
      float arc = r / radius;
      auto n = v.normal, old = v.tangent.xyz();
      v.normal = normalize(clockwise * (n.x / arc) +
                           Vec3{0, n.y - slope * n.z, 0} + radial * n.z);
      Vec3 tangent = clockwise * (old.x * arc) + Vec3{0, old.y, 0} +
                     radial * (old.z + slope * old.y);
      tangent = normalize(tangent - v.normal * dot(tangent, v.normal));
      v.tangent = {tangent, v.tangent.w};
      v.position = point(v.position);
      target.add_vertex(v);
    }
    for (auto i : local.indices)
      target.indices.push_back(first + i);
  }
};
std::array<Vec2, 8> section(float w, float d) {
  auto depth = [&](float z) { return rear + (z - rear) * d; };
  return {{{-w + bevel, rear},
           {w - bevel, rear},
           {w, depth(rear + bevel)},
           {w, depth(front - .30f)},
           {w - bevel, depth(front)},
           {-w + bevel, depth(front)},
           {-w, depth(front - .30f)},
           {-w, depth(rear + bevel)}}};
}
void joined_node(Mesh &mesh, Vec2 center, float angle, std::uint32_t material,
                 float depth) {
  std::vector<Vec2> outline;
  std::array<std::size_t, 4> ports{};
  const std::array<float, 4> angles{{-angle, angle, kPi - angle, kPi + angle}};
  for (int arm = 0; arm < 4; ++arm) {
    Vec2 d{std::cos(angles[arm]), std::sin(angles[arm])},
        next{std::cos(angles[(arm + 1) % 4]), std::sin(angles[(arm + 1) % 4])};
    Vec2 side{-d.y, d.x}, ns{-next.y, next.x};
    ports[arm] = outline.size();
    outline.push_back(d * reach - side * node_width);
    outline.push_back(d * reach + side * node_width);
    Vec2 a = d * 2.35f + side * node_width, b = normalize(d + next) * 1.12f,
         c = next * 2.35f - ns * node_width;
    outline.push_back(a);
    for (int j = 1; j <= 8; ++j) {
      float t = j / 8.f, u = 1 - t;
      outline.push_back(a * (u * u) + b * (2 * u * t) + c * (t * t));
    }
  }
  auto inset = plan_offset(outline, -bevel);
  for (int arm = 0; arm < 4; ++arm) {
    Vec2 d{std::cos(angles[arm]), std::sin(angles[arm])}, side{-d.y, d.x};
    inset[ports[arm]] = d * reach - side * (node_width - bevel);
    inset[ports[arm] + 1] = d * reach + side * (node_width - bevel);
  }
  auto vertex = [&](Vec2 p, float z) {
    return Vec3{center.x + p.x, center.y + p.y, rear + (z - rear) * depth};
  };
  // A convex casting face joins four concave shoulders. The eight-sided
  // terminal contours mate exactly with the neighboring profiled members.
  for (int face = 0; face < 2; ++face) {
    float base = face ? rear : front, crown = face ? 0.f : .27f,
          sign = face ? -1.f : 1.f;
    auto point = [&](std::size_t i, float rho) {
      return vertex(inset[i] * rho, base + crown * (1 - rho * rho));
    };
    auto normal = [&](std::size_t i, float rho) {
      Vec2 p = inset[i],
           tangent = normalize(inset[(i + 1) % inset.size()] -
                               inset[(i + inset.size() - 1) % inset.size()]);
      Vec3 n = normalize(cross(Vec3{p.x, p.y, -2 * crown * rho * depth},
                               Vec3{tangent.x, tangent.y, 0}));
      if (n.z * sign < 0)
        n = -n;
      return n;
    };
    Emit emit(&mesh, material);
    for (std::size_t i = 0; i < inset.size(); ++i) {
      auto j = (i + 1) % inset.size();
      Vec3 a = vertex({0, 0}, base + crown), b = point(i, .25f),
           c = point(j, .25f);
      if (sign < 0)
        std::swap(b, c);
      emit.triangle(a, b, c);
      for (int ring = 1; ring < 4; ++ring) {
        float r0 = ring * .25f, r1 = (ring + 1) * .25f;
        quad(mesh, material,
             {point(i, r0), point(i, r1), point(j, r1), point(j, r0)},
             {normal(i, r0), normal(i, r1), normal(j, r1), normal(j, r0)});
      }
    }
  }
  for (std::size_t i = 0; i < outline.size(); ++i) {
    if (std::find(ports.begin(), ports.end(), i) != ports.end())
      continue;
    auto j = (i + 1) % outline.size();
    Vec2 edge = normalize(outline[j] - outline[i]);
    Vec3 outward{edge.y, -edge.x, 0};
    auto span = [&](Vec2 a, float az, Vec2 b, float bz, Vec2 c, float cz,
                    Vec2 d, float dz) {
      std::array<Vec3, 4> p{vertex(a, az), vertex(b, bz), vertex(c, cz),
                            vertex(d, dz)};
      auto n = normalize(cross(p[1] - p[0], p[3] - p[0]));
      if (dot(n, outward) < 0)
        n = -n;
      quad(mesh, material, p, {n, n, n, n});
    };
    span(inset[i], rear, outline[i], rear + bevel, outline[j], rear + bevel,
         inset[j], rear);
    span(outline[i], rear + bevel, outline[i], front - .30f, outline[j],
         front - .30f, outline[j], rear + bevel);
    span(outline[i], front - .30f, inset[i], front, inset[j], front, outline[j],
         front - .30f);
  }
}
void member(Mesh &mesh, Vec2 a, Vec2 b, bool node_a, bool node_b,
            std::uint32_t material, float depth) {
  Vec2 along = normalize(b - a), side{-along.y, along.x};
  float distance = length(b - a);
  if (distance < .04f)
    return;
  auto ring = [&](float t, float inset) {
    float taper = (2 * t - 1) * (2 * t - 1),
          w = mid_width + (node_width - mid_width) * taper;
    auto profile = section(w, depth * (.92f + .08f * taper));
    std::array<Vec3, 8> points;
    for (int i = 0; i < 8; ++i) {
      auto p = lerp(a, b, t) + side * (profile[i].x * inset);
      points[i] = {p.x, p.y, profile[i].y * inset};
    }
    return points;
  };
  auto span = [&](float start, float finish, std::uint32_t mat, float inset,
                  bool cap_start, bool cap_end) {
    int steps = std::max(2, int(std::ceil((finish - start) * distance / .42f)));
    for (int j = 0; j < steps; ++j) {
      auto p = ring(start + (finish - start) * j / steps, inset),
           q = ring(start + (finish - start) * (j + 1) / steps, inset);
      for (int i = 0; i < 8; ++i) {
        int next = (i + 1) % 8;
        std::array<Vec3, 4> points{p[i], p[next], q[next], q[i]};
        auto mid = (points[0] + points[1] + points[2] + points[3]) * .25f;
        auto c = lerp(a, b, start + (finish - start) * (j + .5f) / steps);
        auto n = normalize(cross(points[1] - points[0], points[3] - points[0]));
        if (dot(n, mid - Vec3{c.x, c.y, .1f * depth}) < 0)
          n = -n;
        quad(mesh, mat, points, {n, n, n, n});
      }
    }
    for (int end = 0; end < 2; ++end) {
      if (!(end ? cap_end : cap_start))
        continue;
      float t = end ? finish : start;
      auto points = ring(t, inset);
      auto c = lerp(a, b, t);
      Emit emit(&mesh, mat);
      Vec3 center{c.x, c.y, .1f * depth};
      for (int i = 0; i < 8; ++i) {
        auto v = points[i], w = points[(i + 1) % 8];
        Vec3 n{along.x * (end ? 1.f : -1.f), along.y * (end ? 1.f : -1.f), 0};
        if (dot(cross(v - center, w - center), n) < 0)
          std::swap(v, w);
        emit.triangle(center, v, w);
      }
    }
  };
  // The backing is continuous under closed 24 mm panel returns. Exterior
  // terminal rings join the casting instead of overlapping bars through it.
  span(0, 1, M_DARK_METAL, .955f, true, true);
  int panels = std::max(1, int(std::ceil(distance / 3.6f)));
  for (int i = 0; i < panels; ++i) {
    float gap = .012f / distance;
    span(i / float(panels) + (i ? gap : 0),
         (i + 1) / float(panels) - (i + 1 < panels ? gap : 0), material, 1,
         i > 0 || !node_a, i + 1 < panels || !node_b);
  }
}
struct OpeningBox {
  Vec2 lo, hi;
};
std::vector<OpeningBox> opening_boxes(const SculptedDiagridSpec &spec,
                                      float radius) {
  std::vector<OpeningBox> result;
  for (auto opening : spec.openings)
    for (float turn : {-2 * kPi, 0.f, 2 * kPi}) {
      float x = -(opening.angle + turn) * radius,
            y = opening.floor_y - spec.base_y;
      result.push_back({{x - 4.2f, y - .85f},
                        {x + 4.2f, y + opening.head_offset}});
    }
  return result;
}
bool node_clear(Vec2 p, const std::vector<OpeningBox> &openings) {
  for (auto box : openings)
    if (p.x > box.lo.x - 3.65f && p.x < box.hi.x + 3.65f &&
        p.y > box.lo.y - 3.65f && p.y < box.hi.y + 3.65f)
      return false;
  return true;
}
std::vector<std::pair<Vec2, Vec2>>
clip_member(Vec2 a, Vec2 b, const std::vector<OpeningBox> &openings) {
  std::vector<std::pair<Vec2, Vec2>> segments{{a, b}};
  for (auto box : openings) {
    std::vector<std::pair<Vec2, Vec2>> next;
    for (auto [p, q] : segments) {
      auto d = q - p;
      float begin = 0, end = 1;
      bool hit = true;
      for (int axis = 0; axis < 2; ++axis) {
        float x = axis ? p.y : p.x, v = axis ? d.y : d.x,
              lo = axis ? box.lo.y : box.lo.x, hi = axis ? box.hi.y : box.hi.x;
        if (std::abs(v) < 1e-7f) {
          if (x < lo || x > hi)
            hit = false;
        } else {
          float t0 = (lo - x) / v, t1 = (hi - x) / v;
          if (t1 < t0)
            std::swap(t0, t1);
          begin = std::max(begin, t0);
          end = std::min(end, t1);
        }
      }
      if (!hit || begin >= end)
        next.push_back({p, q});
      else {
        if (begin > 1e-5f)
          next.push_back({p, lerp(p, q, begin)});
        if (end < .99999f)
          next.push_back({lerp(p, q, end), q});
      }
    }
    segments = std::move(next);
  }
  return segments;
}
} // namespace
std::uint32_t sculpted_diagrid_ceramic(Scene &scene) {
  for (std::size_t i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == "satin ivory ceramic lattice")
      return static_cast<std::uint32_t>(i);
  auto m = scene.materials[M_WHITE_METAL];
  m.name = "satin ivory ceramic lattice";
  m.base_color = {.71f, .75f, .72f};
  m.roughness = .29f;
  m.normal_strength = .085f;
  m.metallic = 0;
  m.albedo_set = "concrete_white";
  m.uv_scale = 1.8f;
  m.flags = kMatTriplanar;
  auto id = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(m);
  return id;
}
void build_sculpted_diagrid(
    Scene &scene, const SculptedDiagridSpec &spec,
    const std::function<std::uint32_t(float)> &material_at_angle) {
  if (spec.columns < 4 || spec.levels < 1 || spec.height <= 0 ||
      spec.base_radius <= 5)
    throw std::runtime_error("Invalid sculpted diagrid dimensions");
  float radius = spec.base_radius - spec.taper_metres * .25f,
        width = 2 * kPi * radius / spec.columns,
        rise = spec.height / spec.levels, angle = std::atan2(rise, width),
        depth = spec.shallow_control ? .56f / (front - rear) : 1.f;
  Surface surface{spec, radius};
  auto openings = opening_boxes(spec, radius);
  auto point = [&](float col, float row) {
    return Vec2{-col * width, row * rise};
  };
  auto is_node = [&](float col, float row) {
    return row > 0 && row < spec.levels &&
           node_clear(point(col, row), openings);
  };
  auto connection = [&](float c0, float r0, float c1, float r1) {
    auto a = point(c0, r0), b = point(c1, r1);
    bool na = is_node(c0, r0), nb = is_node(c1, r1);
    auto d = normalize(b - a);
    if (na)
      a = a + d * reach;
    if (nb)
      b = b - d * reach;
    for (auto [p, q] : clip_member(a, b, openings)) {
      Mesh local;
      member(local, p, q, na && length(p - a) < .001f,
             nb && length(q - b) < .001f,
             material_at_angle(-(p.x + q.x) * .5f / radius), depth);
      surface.append(scene.opaque, local);
    }
  };
  auto node = [&](float col, float row) {
    if (!is_node(col, row))
      return;
    Mesh local;
    joined_node(local, point(col, row), angle,
                material_at_angle(2 * kPi * col / spec.columns), depth);
    surface.append(scene.opaque, local);
  };
  int last_col = spec.first_column + spec.column_count,
      last_row = spec.first_level + spec.level_count;
  for (int row = spec.first_level; row < last_row; ++row)
    for (int col = spec.first_column; col < last_col; ++col) {
      connection(col, row, col + .5f, row + .5f);
      connection(col + .5f, row + .5f, col + 1, row + 1);
      connection(col + 1, row, col + .5f, row + .5f);
      connection(col + .5f, row + .5f, col, row + 1);
      node(col + .5f, row + .5f);
      node(col, row);
    }
  if (spec.column_count < spec.columns)
    for (int row = spec.first_level; row < last_row; ++row)
      node(float(last_col), float(row));
  if (last_row < spec.levels)
    for (int col = spec.first_column; col <= last_col; ++col)
      node(float(col), float(last_row));
}
Scene make_sculpted_diagrid_sample(std::string_view view,
                                   bool shallow_control) {
  Scene scene;
  scene.materials = make_materials();
  auto ceramic = sculpted_diagrid_ceramic(scene);
  SculptedDiagridSpec spec;
  spec.first_column = 2;
  spec.column_count = 4;
  spec.first_level = 12;
  spec.level_count = 3;
  spec.shallow_control = shallow_control;
  build_sculpted_diagrid(scene, spec, [&](float) { return ceramic; });
  auto pane = scene.materials[M_GLASS_CLEAR];
  pane.name = "foreground occupied glazing";
  pane.flags = 128u;
  pane.base_color = {.94f, .97f, .98f};
  pane.roughness = .055f;
  pane.room_w = 1.5f;
  pane.room_h = .98f;
  pane.room_d = .96f;
  pane.lit_probability = .012f;
  pane.metallic = 0;
  pane.albedo_set = "";
  auto glass = static_cast<std::uint32_t>(scene.materials.size());
  scene.materials.push_back(pane);
  float low = spec.base_y + spec.first_level * spec.height / spec.levels,
        high = spec.base_y + (spec.first_level + spec.level_count) *
                                 spec.height / spec.levels,
        begin = spec.first_column * 2 * kPi / spec.columns,
        end = (spec.first_column + spec.column_count) * 2 * kPi / spec.columns;
  auto p = [&](float a, float y, float offset = 0.f) {
    float t = (y - spec.base_y) / spec.height,
          r = 42.3f * (1 - .05f * t * t) + offset;
    return Vec3{spec.centre.x + r * std::cos(a), y,
                spec.centre.y + r * std::sin(a)};
  };
  int floor0 = int(std::ceil((low - spec.base_y) / 4)),
      floor1 = int(std::floor((high - spec.base_y) / 4));
  for (int floor = floor0; floor <= floor1; ++floor) {
    float y = spec.base_y + floor * 4;
    for (int bay = 0; bay < 16; ++bay) {
      float a = begin + (end - begin) * bay / 16,
            b = begin + (end - begin) * (bay + 1) / 16;
      std::vector<Vec2> floor_plan;
      for (auto point : {p(a, y), p(b, y), p(b, y, -8), p(a, y, -8)})
        floor_plan.push_back({point.x, point.z});
      slab(scene.opaque, floor_plan, y, .22f, M_CONCRETE_WHITE);
      Emit(&scene.opaque, M_BRONZE)
          .quad_metric(p(b, y - .22f), p(a, y - .22f), p(a, y), p(b, y));
      if (floor == floor1)
        continue;
      Emit(&scene.opaque, glass)
          .quad_metric(p(b, y + .22f), p(a, y + .22f), p(a, y + 3.82f),
                       p(b, y + 3.82f));
      Emit(&scene.opaque, M_BRONZE)
          .beam(p(a, y + .22f), p(a, y + 3.82f), .065f, .075f);
      Emit(&scene.opaque, M_BRONZE)
          .beam(p(a, y + 3.82f), p(b, y + 3.82f), .055f, .065f);
      Emit(&scene.opaque, M_CONCRETE_WHITE)
          .quad_metric(p(b, y, -8), p(a, y, -8), p(a, y + 3.78f, -8),
                       p(b, y + 3.78f, -8));
      if ((floor + bay) % 3 == 0) {
        float a0 = (a + b) * .5f;
        Vec3 radial{std::cos(a0), 0, std::sin(a0)},
            tangent{-radial.z, 0, radial.x};
        Emit(&scene.opaque, M_BRONZE)
            .box(p(a0, y + .70f, -2.6f), {.95f, .09f, .5f}, tangent, {0, 1, 0},
                 radial);
        for (float x : {-1.f, 1.f})
          for (float z : {-1.f, 1.f})
            Emit(&scene.opaque, M_BRONZE)
                .box(p(a0, y + .305f, -2.6f) + tangent * (x * .8f) +
                         radial * (z * .38f),
                     {.035f, .305f, .035f}, tangent, {0, 1, 0}, radial);
        for (float side : {-1.f, 1.f}) {
          auto seat = p(a0, y + .43f, -2.6f) + tangent * (side * 1.42f);
          Emit(&scene.opaque, M_BRONZE)
              .box(seat - Vec3{0, .24f, 0}, {.045f, .19f, .20f}, tangent,
                   {0, 1, 0}, radial);
          Emit(&scene.opaque, M_BRONZE)
              .box(seat, {.30f, .05f, .33f}, tangent, {0, 1, 0}, radial);
        }
        Emit(&scene.opaque, M_LOBBY_LIGHT)
            .box(p(a0, y + 3.72f, -3.3f), {.9f, .025f, .16f}, tangent,
                 {0, 1, 0}, radial);
      }
    }
  }
  float a = (begin + end) * .5f;
  Vec3 outward{std::cos(a), 0, std::sin(a)}, tangent{-outward.z, 0, outward.x};
  auto focus = p(a, (low + high) * .5f, 2);
  if (view == "frontal")
    scene.camera_position = focus + outward * 78 + Vec3{0, 2, 0};
  else if (view == "oblique")
    scene.camera_position = focus + outward * 55 + tangent * 48 + Vec3{0, 2, 0};
  else if (view == "arrival") {
    if (!shot_camera("aerial", scene.camera_position, scene.camera_target))
      throw std::runtime_error("Arrival camera unavailable");
  } else
    throw std::runtime_error("Unknown diagrid sample view");
  if (view != "arrival")
    scene.camera_target = focus;
  scene.camera_fov_degrees =
      view == "arrival" ? shot_fov_degrees("aerial") : 48.f;
  scene.city_size = "sculpted-diagrid-sample";
  scene.city_radius = 130;
  scene.finalize_draws();
  return scene;
}
} // namespace cb
