#include "landing_lounge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
using Material = std::uint32_t;
const Vec2 kCentre{224, 144};

Material named(const Scene &s, std::string_view name) {
  for (Material i = 0; i < s.materials.size(); ++i)
    if (s.materials[i].name == name)
      return i;
  throw std::runtime_error("Landing lounge requires material: " +
                           std::string(name));
}
Material surface(Scene &s, const char *name, Vec3 colour, float roughness) {
  for (Material i = 0; i < s.materials.size(); ++i)
    if (s.materials[i].name == name)
      return i;
  MaterialDesc m;
  m.name = name;
  m.base_color = colour;
  m.roughness = roughness;
  s.materials.push_back(m);
  return static_cast<Material>(s.materials.size() - 1);
}
struct Palette {
  Material wood, bronze, ceramic, stone, cloth, green, seam, paper, book, light,
      dark;
  explicit Palette(Scene &s)
      : wood(named(s, "wood_oiled")), bronze(named(s, "bronze_satin")),
        ceramic(named(s, "ceramic_ivory")), stone(named(s, "stone_warm")),
        cloth(surface(s, "lounge woven oatmeal", {.42f, .35f, .26f}, .93f)),
        green(
            surface(s, "lounge olive upholstery", {.105f, .14f, .063f}, .95f)),
        seam(surface(s, "lounge upholstery piping", {.26f, .215f, .16f}, .88f)),
        paper(surface(s, "lounge paper edges", {.59f, .53f, .405f}, .88f)),
        book(surface(s, "lounge terracotta bookcloth", {.26f, .088f, .038f},
                     .88f)),
        light(surface(s, "lounge opal luminaires", {1, .79f, .53f}, .52f)),
        dark(named(s, "gasket_charcoal")) {
    s.materials[light].flags = kMatEmissive;
    s.materials[light].emissive = 1.6f;
    s.materials[light].tint2 = {1, .79f, .53f};
  }
};

// The rounded edge is actual geometry. Surface normals follow the radius,
// while the central upholstery/wood panels retain their flat faces.
void rounded_box(Mesh &mesh, Material mat, Vec3 centre, Vec3 half,
                 float radius) {
  radius = std::min({radius, half.x * .85f, half.y * .85f, half.z * .85f});
  const Vec3 core = half - Vec3{radius, radius, radius};
  auto component = [](Vec3 v, int axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
  };
  auto assign = [](Vec3 &v, int axis, float value) {
    if (axis == 0)
      v.x = value;
    else if (axis == 1)
      v.y = value;
    else
      v.z = value;
  };
  auto samples = [&](float h) {
    float c = h - radius;
    return std::array<float, 7>{-h, -c - radius * .3f, -c, 0,
                                c,  c + radius * .3f,  h};
  };
  for (int axis = 0; axis < 3; ++axis)
    for (float side : {-1.f, 1.f}) {
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const auto us = samples(component(half, u)),
                 vs = samples(component(half, v));
      const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
      for (float y : vs)
        for (float x : us) {
          Vec3 p{};
          assign(p, axis, side * component(half, axis));
          assign(p, u, x);
          assign(p, v, y);
          Vec3 c{std::clamp(p.x, -core.x, core.x),
                 std::clamp(p.y, -core.y, core.y),
                 std::clamp(p.z, -core.z, core.z)};
          Vec3 n = normalize(p - c), t{};
          assign(t, u, 1);
          t = normalize(t - n * dot(t, n));
          Vertex out;
          out.position = centre + c + n * radius;
          out.normal = n;
          out.tangent = {t, 1};
          out.uv = {x, y};
          out.aux = {x, y, .5f, 1};
          out.material = mat;
          mesh.add_vertex(out);
        }
      for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 6; ++x) {
          const auto a = start + y * 7 + x, b = a + 1, c = a + 7, d = c + 1;
          if (side > 0) {
            mesh.add_triangle(a, b, d);
            mesh.add_triangle(a, d, c);
          } else {
            mesh.add_triangle(a, d, b);
            mesh.add_triangle(a, c, d);
          }
        }
    }
}

void resource(Scene &s, const char *name,
              const std::function<void(Mesh &)> &make) {
  for (const auto &r : s.asset_library.resources)
    if (r.name == name)
      return;
  MeshResource r;
  r.name = name;
  make(r.mesh);
  std::vector<std::uint32_t> clean;
  clean.reserve(r.mesh.indices.size());
  for (std::size_t i = 0; i < r.mesh.indices.size(); i += 3) {
    auto a = r.mesh.indices[i], b = r.mesh.indices[i + 1],
         c = r.mesh.indices[i + 2];
    if (length(cross(r.mesh.vertices[b].position - r.mesh.vertices[a].position,
                     r.mesh.vertices[c].position -
                         r.mesh.vertices[a].position)) > 1e-10f)
      clean.insert(clean.end(), {a, b, c});
  }
  r.mesh.indices = std::move(clean);
  r.mesh.bounds_min = {1e30f, 1e30f, 1e30f};
  r.mesh.bounds_max = {-1e30f, -1e30f, -1e30f};
  for (const auto &v : r.mesh.vertices) {
    r.mesh.bounds_min = vmin(r.mesh.bounds_min, v.position);
    r.mesh.bounds_max = vmax(r.mesh.bounds_max, v.position);
  }
  s.asset_library.resources.push_back(std::move(r));
}
void resources(Scene &s, const Palette &p) {
  for (int type = 0; type < 2; ++type)
    resource(
        s, type ? "landing_lounge_armchair" : "landing_lounge_sofa",
        [&, type](Mesh &m) {
          const float width = type ? .93f : 2.8f;
          const int cushions = type ? 1 : 3;
          Emit wood(&m, p.wood), metal(&m, p.bronze), seam(&m, p.seam);
          for (float x : {-width * .42f, width * .42f})
            for (float z : {-.37f, .37f}) {
              wood.frustum({x, 0, z}, {x, .30f, z}, .035f, .047f, 12, true);
              metal.tube({x, 0, z}, {x, .047f, z}, .036f, 12, true);
            }
          rounded_box(m, p.wood, {0, .30f, 0}, {width * .5f, .065f, .48f},
                      .035f);
          for (int i = 0; i < cushions; ++i) {
            const float w = (width - .25f) / cushions,
                        x = (i - (cushions - 1) * .5f) * w;
            const Material fabric = type ? p.green : p.cloth;
            rounded_box(m, fabric, {x, .455f, .025f}, {w * .48f, .12f, .43f},
                        .079f);
            rounded_box(m, fabric, {x, .76f, -.355f}, {w * .48f, .31f, .14f},
                        .085f);
            auto piping = plan_rounded_rect(w * .48f + .001f, .431f, .079f, 6,
                                            {x, .025f});
            for (std::size_t j = 0; j < piping.size(); ++j) {
              auto a = piping[j], b = piping[(j + 1) % piping.size()];
              seam.tube({a.x, .455f, a.y}, {b.x, .455f, b.y}, .005f, 6, true);
            }
          }
          rounded_box(m, p.wood, {0, .70f, -.52f}, {width * .48f, .29f, .03f},
                      .025f);
          for (float side : {-1.f, 1.f}) {
            rounded_box(m, p.wood, {side * (width * .5f - .03f), .67f, 0},
                        {.054f, .075f, .48f}, .04f);
            for (float z : {-.34f, .34f})
              rounded_box(m, p.wood, {side * (width * .5f - .03f), .49f, z},
                          {.032f, .19f, .032f}, .016f);
          }
        });
  resource(s, "landing_lounge_table", [&](Mesh &m) {
    rounded_box(m, p.stone, {0, .43f, 0}, {.84f, .045f, .43f}, .041f);
    for (float x : {-.61f, .61f})
      for (float z : {-.27f, .27f})
        Emit(&m, p.bronze).tube({x, 0, z}, {x, .39f, z}, .022f, 12, true);
    rounded_box(m, p.paper, {.20f, .505f, .03f}, {.15f, .026f, .20f}, .01f);
    for (float y : {.477f, .535f})
      rounded_box(m, p.book, {.20f, y, .03f}, {.156f, .005f, .208f}, .004f);
    Emit cup(&m, p.ceramic);
    cup.frustum({-.26f, .476f, .045f}, {-.26f, .571f, .045f}, .038f, .048f, 24,
                false);
    cup.wall(plan_circle(.040f, 24, {-.26f, .045f}), .49f, .571f, true, false);
    cup.torus({-.208f, .527f, .045f}, {0, 0, 1}, .026f, .006f, 20, 8);
    cup.torus({-.26f, .571f, .045f}, {0, 1, 0}, .045f, .004f, 24, 8);
    Emit(&m, p.dark)
        .polygon(plan_circle(.039f, 24, {-.26f, .045f}), .560f, true);
  });
  resource(s, "landing_lounge_bookcase", [&](Mesh &m) {
    Emit wood(&m, p.wood), metal(&m, p.bronze), paper(&m, p.paper);
    rounded_box(m, p.wood, {0, 1.65f, -.21f}, {1.7f, 1.65f, .036f}, .018f);
    for (float x : {-1.72f, 1.72f})
      metal.box({x, 1.69f, 0}, {.024f, 1.69f, .27f});
    Rng r = root_rng("landing bookcase");
    for (int row = 0; row < 5; ++row) {
      const float y = .20f + row * .62f;
      wood.box({0, y, 0}, {1.73f, .026f, .27f});
      for (int book = 0; book < 25; ++book) {
        if ((book + row * 3) % 11 == 4)
          continue;
        float x = -1.59f + book * .131f, h = r.range(.23f, .44f),
              w = r.range(.035f, .055f);
        Material cover = book % 4 == 0   ? p.green
                         : book % 3 == 0 ? p.book
                                         : p.cloth;
        rounded_box(m, cover, {x, y + .027f + h * .5f, .065f},
                    {w, h * .5f, .13f}, .007f);
        paper.box({x, y + .027f + h * .5f, .198f}, {w * .75f, .0017f, .001f});
      }
      Emit(&m, p.light).box({0, y - .031f, .17f}, {1.63f, .005f, .026f});
    }
  });
  resource(s, "landing_lounge_pendant", [&](Mesh &m) {
    Emit metal(&m, p.bronze), opal(&m, p.light);
    metal.tube({0, 0, 0}, {0, .65f, 0}, .008f, 8, true);
    metal.frustum({0, -.11f, 0}, {0, .015f, 0}, .44f, .10f, 48, true);
    opal.tube({0, -.122f, 0}, {0, -.115f, 0}, .415f, 48, true);
    metal.torus({0, -.115f, 0}, {0, 1, 0}, .433f, .012f, 48, 8);
  });
}

Vec3 point(float angle, float radius, float y) {
  return {kCentre.x + std::cos(angle) * radius, y,
          kCentre.y + std::sin(angle) * radius};
}
} // namespace

void stage_landing_lounge(Scene &s, Rng rng) {
  if (s.asset_library.resources.empty())
    return;
  const Palette p(s);
  resources(s, p);
  const auto first = static_cast<std::uint32_t>(s.opaque.indices.size());
  for (int floor = 0; floor < 3; ++floor) {
    const float y = 34 + floor * 7.2f;
    for (int bay = 0; bay < 5; ++bay) {
      const float degrees = std::array<float, 5>{105, 122, 155, 177, 197}[bay];
      const float angle = degrees * kPi / 180, yaw = kPi * .5f - angle;
      const Vec3 outward{std::cos(angle), 0, std::sin(angle)},
          across{std::sin(angle), 0, -std::cos(angle)};
      const Vec3 seating = point(angle, 20.25f, y);
      add_asset_instance(s, "landing_lounge_sofa", seating - outward * 1.5f,
                         yaw, 1.f);
      add_asset_instance(s, "landing_lounge_table", seating, yaw, 1.f);
      for (float side : {-1.f, 1.f})
        add_asset_instance(s, "landing_lounge_armchair",
                           seating + outward * .65f + across * (side * 1.75f),
                           yaw + kPi + side * .38f, 1.f);
      add_asset_instance(s, "landing_lounge_bookcase", point(angle, 16.7f, y),
                         yaw, 1.f);
      add_asset_instance(s, "landing_lounge_pendant",
                         seating + Vec3{0, 5.82f, 0}, yaw, 1.f);
      s.lights.push_back(
          {seating + Vec3{0, 5.48f, 0}, 7.5f, {1, .78f, .51f}, 3.4f});
      s.lights.push_back(
          {point(angle, 17.3f, y + 2.5f), 4.1f, {1, .74f, .43f}, 1.65f});
      // Shallow timber ceiling coffers and strips give the occupied volume
      // construction depth while keeping all furnishings clear of the glass.
      Emit wood(&s.opaque, p.wood), light(&s.opaque, p.light);
      for (int strip = 0; strip < 11; ++strip) {
        Vec3 c = point(angle, 20.2f, y + 6.49f) + across * ((strip - 5) * .36f);
        wood.box(c, {.07f, .055f, 3.05f}, across, {0, 1, 0}, outward);
      }
      for (float side : {-1.f, 1.f})
        light.box(point(angle, 20.2f, y + 6.42f) + across * (side * 2.10f),
                  {.018f, .007f, 2.88f}, across, {0, 1, 0}, outward);
      // Large indoor plants sit in real, drained ceramic pots behind the pane.
      Vec3 pot = point(angle, bay % 2 ? 21.9f : 22.45f, y) +
                 across * (bay % 2 ? 2.65f : -2.15f);
      Emit ceramic(&s.opaque, p.ceramic),
          soil(&s.opaque, named(s, "soil_mulch"));
      ceramic.frustum(pot, pot + Vec3{0, .68f, 0}, .33f, .49f, 36, false);
      ceramic.polygon(plan_circle(.33f, 36, {pot.x, pot.z}), pot.y, false);
      ceramic.wall(plan_circle(.435f, 36, {pot.x, pot.z}), pot.y + .57f,
                   pot.y + .68f, true, false);
      ceramic.ring_cap(plan_circle(.49f, 36, {pot.x, pot.z}),
                       plan_circle(.435f, 36, {pot.x, pot.z}), pot.y + .68f,
                       true);
      ceramic.torus(pot + Vec3{0, .69f, 0}, {0, 1, 0}, .466f, .035f, 36, 10);
      soil.polygon(plan_circle(.435f, 36, {pot.x, pot.z}), pot.y + .67f, true);
      add_asset_instance(s, bay % 2 ? "palm_fan" : "phormium",
                         pot + Vec3{0, .67f, 0}, rng.range(-kPi, kPi),
                         bay % 2 ? .40f : 1.2f);
    }
  }
  s.register_range(first, static_cast<std::uint32_t>(s.opaque.indices.size()),
                   {224, 45, 144}, 30);
}
} // namespace cb
