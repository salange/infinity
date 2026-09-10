#include "transit_concourse.hpp"

#include "city_transit.hpp"
#include "street_forecourt.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace cb {
namespace {
using Material = std::uint32_t;
const Vec3 up{0, 1, 0};
const Vec3 along{kTransitAlong.x, 0, kTransitAlong.y};
const Vec3 left{kTransitLeft.x, 0, kTransitLeft.y};

Material material(Scene &s, const char *name, Vec3 colour, float rough,
                  float metal = 0) {
  MaterialDesc m;
  m.name = name;
  m.base_color = colour;
  m.roughness = rough;
  m.metallic = metal;
  s.materials.push_back(m);
  return static_cast<Material>(s.materials.size() - 1);
}
Material named_or(const Scene &s, std::string_view name, Material fallback) {
  for (std::size_t i = s.materials.size(); i > 0; --i)
    if (s.materials[i - 1].name == name)
      return static_cast<Material>(i - 1);
  return fallback;
}
struct Palette {
  Material stone, concrete, rail, bronze, wood, ceramic, dark, soil, light,
      glass;
  explicit Palette(Scene &s)
      : stone(market_paving_material(s)),
        concrete(
            material(s, "guideway precast sleeper", {.28f, .29f, .27f}, .86f)),
        rail(material(s, "guideway steel running face", {.44f, .47f, .49f},
                      .21f, .96f)),
        bronze(named_or(s, "bronze_satin", M_BRONZE)),
        wood(named_or(s, "wood_oiled", M_PANEL_WARM)),
        ceramic(named_or(s, "ceramic_ivory", M_CONCRETE_WHITE)),
        dark(named_or(s, "gasket_charcoal", M_DARK_METAL)),
        soil(named_or(s, "soil_mulch", M_SOIL)),
        light(
            material(s, "platform warm opal lighting", {1, .74f, .42f}, .45f)),
        glass(material(s, "platform laminated canopy glazing",
                       {.66f, .80f, .81f}, .11f)) {
    s.materials[light].flags = kMatEmissive;
    s.materials[light].emissive = 2.1f;
    s.materials[light].tint2 = {1, .74f, .42f};
    auto &g = s.materials[glass];
    g.flags = 128u;
    g.room_w = 1.52f;
    g.room_h = .85f;
    g.room_d = 1.f;
    g.lit_probability = .018f;
  }
};
void box(Scene &s, Material m, float u, float v, float y, Vec3 half) {
  Emit(&s.opaque, m).box(transit_point(u, v, y), half, along, up, left);
}
void solid(Scene &s, Material m, float u0, float u1, float v0, float v1,
           float y0, float y1) {
  Emit e(&s.opaque, m);
  const auto plan = transit_plan(u0, u1, v0, v1);
  e.polygon(plan, y1, true);
  e.polygon(plan, y0, false);
  e.wall(plan, y0, y1, true);
}

void platforms(Scene &s, const Palette &p) {
  for (float sign : {-1.f, 1.f}) {
    float v0 = sign > 0 ? 3.5f : -9.6f;
    float v1 = sign > 0 ? 9.6f : -3.5f;
    solid(s, p.concrete, 0, kTransitLength, v0, v1, kTransitDeckY,
          kTransitWalkY - .055f);
    Emit(&s.opaque, p.dark)
        .polygon(transit_plan(0, kTransitLength, v0, v1), 17.997f, true);
    // Individual large stones have real joints over a continuous bearing slab.
    for (float u = 0; u < kTransitLength; u += 2.4f)
      for (int row = 0; row < 2; ++row) {
        const float a = v0 + row * 3.05f;
        solid(s, p.stone, u + .008f, std::min(u + 2.392f, kTransitLength),
              a + .008f, a + 3.042f, kTransitWalkY - .055f, kTransitWalkY);
      }
    // Tactile edge courses and recessed drainage sit on their own ledges.
    for (float u = .3f; u < kTransitLength - .3f; u += .6f) {
      box(s, p.ceramic, u, sign * 3.77f, 18.007f, {.27f, .007f, .20f});
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 4; ++b)
          Emit(&s.opaque, p.bronze)
              .tube(transit_point(u - .18f + b * .12f,
                                  sign * 3.77f - .12f + a * .12f, 18.014f),
                    transit_point(u - .18f + b * .12f,
                                  sign * 3.77f - .12f + a * .12f, 18.019f),
                    .018f, 8, true);
      box(s, p.dark, u, sign * 9.28f, 18.003f, {.27f, .003f, .06f});
      for (int grate = 0; grate < 8; ++grate)
        box(s, p.bronze, u - .24f + grate * .068f, sign * 9.28f, 18.009f,
            {.013f, .006f, .065f});
    }
    // Track-facing fascia has a drip return, with lamps attached to the wall.
    box(s, p.ceramic, kTransitLength * .5f, sign * 3.51f, 17.90f,
        {kTransitLength * .5f, .07f, .045f});
    for (float u = 4; u < kTransitLength; u += 8) {
      box(s, p.dark, u, sign * 3.475f, 17.72f, {.32f, .065f, .03f});
      box(s, p.light, u, sign * 3.44f, 17.72f, {.27f, .025f, .008f});
      s.lights.push_back({transit_point(u, sign * 3.37f, 17.73f),
                          3.5f,
                          {1, .72f, .40f},
                          1.1f});
    }
  }
}

void tracks(Scene &s, const Palette &p) {
  const float end = kTransitLength - 3.2f;
  solid(s, p.dark, .2f, kTransitLength - .2f, -3.35f, 3.35f, kTransitDeckY,
        kTransitDeckY + .02f);
  for (float centre : {-1.55f, 1.55f}) {
    for (float u = .5f; u < end - .2f; u += .6f) {
      box(s, p.concrete, u, centre, 17.32f, {.13f, .10f, 1.20f});
      for (float side : {-1.f, 1.f}) {
        const float v = centre + side * .7175f;
        box(s, p.dark, u, v, 17.435f, {.16f, .018f, .12f});
        for (float bolt : {-.11f, .11f})
          Emit(&s.opaque, p.bronze)
              .tube(transit_point(u, v + bolt, 17.45f),
                    transit_point(u, v + bolt, 17.477f), .025f, 6, true);
      }
    }
    for (float side : {-1.f, 1.f}) {
      const float v = centre + side * .7175f;
      // Three connected sections form a supported rail, including real web.
      box(s, p.rail, (end + .2f) * .5f, v, 17.465f,
          {(end - .2f) * .5f, .013f, .068f});
      box(s, p.rail, (end + .2f) * .5f, v, 17.515f,
          {(end - .2f) * .5f, .038f, .011f});
      box(s, p.rail, (end + .2f) * .5f, v, 17.568f,
          {(end - .2f) * .5f, .020f, .036f});
    }
    for (float u : {1.6f, kTransitLength - 4.6f}) {
      const float direction = u < 5 ? 1.f : -1.f;
      box(s, p.bronze, u, centre, 18.04f, {.13f, .12f, 1.13f});
      for (float side : {-1.f, 1.f}) {
        const float v = centre + side * .72f;
        Emit metal(&s.opaque, p.bronze);
        metal.beam(transit_point(u, v, 18.03f),
                   transit_point(u - direction * .95f, v, 17.59f), .14f, .14f);
        box(s, p.dark, u + direction * .19f, v, 18.04f, {.06f, .18f, .20f});
        box(s, p.rail, u - direction * .95f, v, 17.62f, {.26f, .04f, .11f});
      }
    }
  }
}

void guard(Scene &s, const Palette &p, float u0, float u1, float v) {
  const int bays = std::max(1, int(std::ceil((u1 - u0) / 2.3f)));
  Emit metal(&s.opaque, p.bronze);
  for (int i = 0; i <= bays; ++i) {
    float u = u0 + (u1 - u0) * i / bays;
    box(s, p.bronze, u, v, 18.018f, {.105f, .018f, .105f});
    metal.tube(transit_point(u, v, 18.036f), transit_point(u, v, 19.12f), .028f,
               10, true);
  }
  for (float y : {18.20f, 18.65f, 19.12f})
    metal.tube(transit_point(u0, v, y), transit_point(u1, v, y),
               y > 19 ? .035f : .014f, 10, true);
}

void shelter(Scene &s, const Palette &p, float u0, float u1, float sign) {
  const float vcentre = sign * 6.55f;
  Emit metal(&s.opaque, p.bronze);
  for (float u : {u0 + .55f, u1 - .55f}) {
    const float v = sign * 8.70f;
    box(s, p.bronze, u, v, 18.035f, {.20f, .035f, .20f});
    box(s, p.bronze, u, v, 20.27f, {.072f, 2.20f, .072f});
    metal.beam(transit_point(u, v, 21.1f),
               transit_point(u, sign * 5.35f, 22.55f), .085f, .12f);
    for (float a : {-.13f, .13f})
      for (float b : {-.13f, .13f})
        Emit(&s.opaque, p.rail)
            .tube(transit_point(u + a, v + b, 18.07f),
                  transit_point(u + a, v + b, 18.105f), .025f, 6, true);
  }
  auto roof_y = [](float v) {
    return 22.5f + .24f * (1 - v * v / (2.75f * 2.75f));
  };
  for (int j = 0; j < 6; ++j) {
    float a = -2.75f + j * (5.5f / 6), b = -2.75f + (j + 1) * (5.5f / 6);
    auto a0 = transit_point(u0, vcentre + a, roof_y(a));
    auto a1 = transit_point(u1, vcentre + a, roof_y(a));
    auto b0 = transit_point(u0, vcentre + b, roof_y(b));
    auto b1 = transit_point(u1, vcentre + b, roof_y(b));
    const auto mat = (j == 2 || j == 3) ? p.glass : p.ceramic;
    Emit sheet(&s.opaque, mat);
    sheet.quad_metric(a0, b0, b1, a1);
    sheet.quad_metric(a1 - up * .08f, b1 - up * .08f, b0 - up * .08f,
                      a0 - up * .08f);
    if (j == 0)
      sheet.quad_metric(a0, a1, a1 - up * .08f, a0 - up * .08f);
    if (j == 5)
      sheet.quad_metric(b1, b0, b0 - up * .08f, b1 - up * .08f);
    sheet.quad_metric(b0, a0, a0 - up * .08f, b0 - up * .08f);
    sheet.quad_metric(a1, b1, b1 - up * .08f, a1 - up * .08f);
    for (float u : {u0 + .55f, u1 - .55f})
      metal.beam(transit_point(u, vcentre + a, roof_y(a) - .13f),
                 transit_point(u, vcentre + b, roof_y(b) - .13f), .10f, .11f);
    if (j % 2 == 0) {
      box(s, p.bronze, (u0 + u1) * .5f, vcentre + a, roof_y(a) - .12f,
          {(u1 - u0) * .5f, .04f, .035f});
      box(s, p.light, (u0 + u1) * .5f, vcentre + a, roof_y(a) - .168f,
          {(u1 - u0) * .43f, .008f, .027f});
    }
  }
  for (float u = u0 + 2; u < u1; u += 4)
    s.lights.push_back(
        {transit_point(u, vcentre, 22.15f), 7, {1, .76f, .47f}, 4.8f});
}

void bench(Scene &s, const Palette &p, float u, float v) {
  for (float offset : {-1.12f, 1.12f}) {
    box(s, p.bronze, u + offset, v, 18.22f, {.045f, .22f, .30f});
    box(s, p.bronze, u + offset, v, 18.015f, {.13f, .015f, .34f});
  }
  for (int slat = 0; slat < 7; ++slat)
    box(s, p.wood, u, v - .28f + slat * .093f, 18.475f, {1.48f, .035f, .039f});
  const float sign = v > 0 ? 1.f : -1.f;
  for (float offset : {-1.12f, 1.12f})
    box(s, p.bronze, u + offset, v + sign * .32f, 18.67f, {.035f, .35f, .035f});
  for (int slat = 0; slat < 4; ++slat)
    box(s, p.wood, u, v + sign * .355f, 18.69f + slat * .09f,
        {1.48f, .032f, .028f});
}

void bed(Scene &s, const Palette &p, Rng r, float u, float v) {
  solid(s, p.concrete, u - .9f, u + .9f, v - .53f, v + .53f, 18, 18.08f);
  for (float sign : {-1.f, 1.f})
    box(s, p.ceramic, u + sign * .85f, v, 18.29f, {.05f, .21f, .53f});
  for (float sign : {-1.f, 1.f})
    box(s, p.ceramic, u, v + sign * .48f, 18.29f, {.8f, .21f, .05f});
  solid(s, p.soil, u - .8f, u + .8f, v - .43f, v + .43f, 18.08f, 18.44f);
  for (int i = 0; i < 48; ++i)
    box(s, p.wood, u + r.range(-.72f, .72f), v + r.range(-.40f, .40f), 18.449f,
        {r.range(.015f, .065f), .009f, r.range(.008f, .02f)});
  if (s.asset_library.resources.empty())
    return;
  for (int i = 0; i < 4; ++i) {
    const auto name = i % 3 == 0 ? "phormium" : "groundcover";
    float scale = i % 3 == 0 ? .38f : .58f;
    AssetInstance plant{asset_resource(s.asset_library, name),
                        transit_point(u - .6f + i * .4f, v, 18.44f),
                        r.range(-kPi, kPi),
                        {scale, scale, scale},
                        {1, 1, 1}};
    bool within = true;
    for (const auto &vertex :
         s.asset_library.resources[plant.resource].mesh.vertices) {
      Vec3 world = asset_transform_point(plant, vertex.position);
      Vec2 delta{world.x - kTransitStart.x, world.z - kTransitStart.y};
      float pu = dot(delta, kTransitAlong), pv = dot(delta, kTransitLeft);
      if (pu < u - 1.1f || pu > u + 1.1f || std::abs(pv) > 9.20f) {
        within = false;
        break;
      }
    }
    if (within)
      s.asset_instances.push_back(plant);
  }
}
} // namespace

void stage_transit_concourse(Scene &s, Rng rng) {
  const auto first = static_cast<std::uint32_t>(s.opaque.indices.size());
  const Palette p(s);
  platforms(s, p);
  tracks(s, p);
  solid(s, p.concrete, kTransitLength - 3, kTransitLength, -3.5f, 3.5f, 17.2f,
        17.945f);
  solid(s, p.stone, kTransitLength - 3, kTransitLength, -3.5f, 3.5f, 17.945f,
        18);
  for (float u : {kTransitLength - 3, kTransitLength - .12f}) {
    Emit metal(&s.opaque, p.bronze);
    for (float v : {-3.5f, 0.f, 3.5f})
      metal.tube(transit_point(u, v, 18), transit_point(u, v, 19.12f), .028f,
                 10, true);
    for (float y : {18.2f, 18.65f, 19.12f})
      metal.tube(transit_point(u, -3.5f, y), transit_point(u, 3.5f, y), .028f,
                 10, true);
  }
  guard(s, p, .15f, 8, 9.52f);
  guard(s, p, 15, kTransitLength-.15f, 9.52f);
  guard(s, p, .15f, kTransitLength-.15f, -9.52f);
  for (float sign : {-1.f, 1.f}) {
    // Separate shelter modules retain open sky and explicit supported ends.
    for (int bay = 0; bay < 6; ++bay) {
      const float a = 20.f + bay * 13.f + (sign < 0 ? 3.f : 0.f);
      shelter(s, p, a, a + 10.5f, sign);
      bench(s, p, a + 4.8f, sign * 8.1f);
      bed(s, p, rng.child(bay + (sign > 0 ? 10 : 20)), a + 11.75f,
          sign * 8.45f);
    }
    // End guards terminate at the track-side tactile strip; no fake guideway
    // extension or vehicle silhouette closes the reserved terminus.
    for (float u : {.12f, kTransitLength - .12f}) {
      Emit metal(&s.opaque, p.bronze);
      for (float v : {3.6f, 6.5f, 9.5f})
        metal.tube(transit_point(u, sign * v, 18),
                   transit_point(u, sign * v, 19.12f), .028f, 10, true);
      for (float y : {18.2f, 18.65f, 19.12f})
        metal.tube(transit_point(u, sign * 3.6f, y),
                   transit_point(u, sign * 9.5f, y), .028f, 10, true);
    }
  }
  s.register_range(first, static_cast<std::uint32_t>(s.opaque.indices.size()),
                   transit_point(kTransitLength * .5f, 0, 20),
                   kTransitLength * .6f);
}
} // namespace cb
