#include "transit_concourse.hpp"

#include "city_transit.hpp"
#include "street_forecourt.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
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
      glass, shell, ribbon_light;
  explicit Palette(Scene &s)
      : stone(material(s,"station warm honed limestone platform",
                       {.61f,.50f,.35f},.55f)),
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
        glass(material(s, "station continuous clerestory glazing",
                       {.95f, .90f, .78f}, .095f)),
        shell(material(s, "station silver ceramic standing seam shell",
                       {.69f, .71f, .70f}, .43f, .38f)),
        ribbon_light(material(s,"station warm ceiling ribbon diffuser",
                       {1,.79f,.49f},.45f)) {
    s.materials[light].flags = kMatEmissive;
    s.materials[light].emissive = 2.1f;
    s.materials[light].tint2 = {1, .74f, .42f};
    s.materials[ribbon_light].flags=kMatEmissive;
    s.materials[ribbon_light].emissive=4.2f;
    s.materials[ribbon_light].tint2={1,.79f,.49f};
    auto &g = s.materials[glass];
    g.flags = 128u;
    g.room_w = 1.52f;
    g.room_h = .94f;
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
  const float end = kTransitRailEnd;
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
    for (float u : {1.6f, end-1.4f}) {
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

// Smooth surface normals belong to the actual curved roof; the fine standing
// seams are separate connected geometry rather than a coarse faceted canopy.
void surface_triangle(Scene &s, Material material,
                      const std::array<Vec3, 3> &points,
                      const std::array<Vec3, 3> &normals,
                      const std::array<Vec2, 3> &uv) {
  std::array<std::uint32_t, 3> indices{};
  for (int i = 0; i < 3; ++i) {
    const Vec3 n = normalize(normals[i]);
    const Vec3 tangent = normalize(along - n * dot(along, n));
    const float handedness = dot(cross(n, tangent), left) >= 0 ? 1.f : -1.f;
    indices[i] = s.opaque.add_vertex(
        {points[i], n, {tangent.x, tangent.y, tangent.z, handedness}, uv[i], material,
         {uv[i].x, uv[i].y, 0, 1}});
  }
  if (dot(cross(points[1] - points[0], points[2] - points[0]),
          normals[0] + normals[1] + normals[2]) < 0)
    std::swap(indices[1], indices[2]);
  s.opaque.add_triangle(indices[0], indices[1], indices[2]);
}

float hall_roof_y(float v) {
  const float t = v / kTransitRoofHalfWidth;
  return kTransitRoofEaveY +
         (kTransitRoofCrownY - kTransitRoofEaveY) * (1 - t * t);
}
Vec3 hall_normal(float v) {
  const float slope = -2 * (kTransitRoofCrownY - kTransitRoofEaveY) * v /
                      (kTransitRoofHalfWidth * kTransitRoofHalfWidth);
  return normalize(up - left * slope);
}

void hall_shell(Scene &s, const Palette &p) {
  const float end = kTransitHallEnd;
  const int length_bays = std::max(1, int(std::ceil(end / 2.4f)));
  constexpr int cross_bays = 40;
  constexpr float thickness = .18f;
  for (int i = 0; i < length_bays; ++i)
    for (int j = 0; j < cross_bays; ++j) {
      const float u0 = end * i / length_bays, u1 = end * (i + 1) / length_bays;
      const float v0 = kTransitRoofHalfWidth * (-1 + 2.f * j / cross_bays),
                  v1 = kTransitRoofHalfWidth * (-1 + 2.f * (j + 1) / cross_bays);
      const std::array<Vec2, 4> uv{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
      for (bool underside : {false, true}) {
        std::array<Vec3, 4> pos, normal;
        for (int k = 0; k < 4; ++k) {
          pos[k] = transit_point(uv[k].x, uv[k].y,
                                 hall_roof_y(uv[k].y) - (underside ? thickness : 0));
          normal[k] = hall_normal(uv[k].y) * (underside ? -1.f : 1.f);
        }
        for (auto tri : {std::array<int, 3>{0, 1, 2}, {0, 2, 3}})
          surface_triangle(s, underside ? p.ceramic : p.shell,
                           {pos[tri[0]], pos[tri[1]], pos[tri[2]]},
                           {normal[tri[0]], normal[tri[1]], normal[tri[2]]},
                           {uv[tri[0]], uv[tri[1]], uv[tri[2]]});
      }
      for (float u : {0.f, end}) {
        if ((u == 0 && i != 0) || (u == end && i != length_bays - 1)) continue;
        Emit edge(&s.opaque, p.shell);
        const auto a = transit_point(u, v0, hall_roof_y(v0));
        const auto b = transit_point(u, v1, hall_roof_y(v1));
        if (u == 0) edge.quad_metric(b, a, a-up*thickness, b-up*thickness);
        else edge.quad_metric(a, b, b-up*thickness, a-up*thickness);
      }
    }
  // Fine longitudinal folds, plus restrained transverse panel joints.
  for (int seam = -5; seam <= 5; ++seam) {
    const float v = seam * 1.72f;
    box(s, p.shell, end*.5f, v, hall_roof_y(v)+.016f,
        {end*.5f, .022f, .016f});
  }
  for (float u = 7.5f; u < end; u += 7.5f)
    for (int j = 0; j < cross_bays; ++j) {
      const float a = kTransitRoofHalfWidth*(-1+2.f*j/cross_bays);
      const float b = kTransitRoofHalfWidth*(-1+2.f*(j+1)/cross_bays);
      Emit(&s.opaque, p.shell).beam(transit_point(u,a,hall_roof_y(a)+.008f),
          transit_point(u,b,hall_roof_y(b)+.008f), .016f, .012f);
    }
  Emit metal(&s.opaque, p.bronze);
  for (float side : {-1.f, 1.f}) {
    box(s,p.dark,end*.5f,side*10.01f,22.24f,{end*.5f,.065f,.085f});
    box(s,p.shell,end*.5f,side*10.04f,22.38f,{end*.5f,.08f,.045f});
    box(s,p.ceramic,end*.5f,side*9.40f,20.18f,{end*.5f,.08f,.09f});
    box(s,p.bronze,end*.5f,side*9.42f,22.12f,{end*.5f,.04f,.055f});
    const int panes = std::max(1,int(std::ceil(end/2.5f)));
    const float door0=kTransitAccessOffset+10.65f,door1=kTransitAccessOffset+13.75f;
    auto lower_pane=[&](float a,float b) {
      if(b-a<.07f)return;
      box(s,p.glass,(a+b)*.5f,side*9.42f,19.125f,
          {(b-a)*.5f-.025f,1.025f,.009f});
      box(s,p.bronze,(a+b)*.5f,side*9.42f,18.055f,
          {(b-a)*.5f,.055f,.065f});
    };
    for (int pane = 0; pane < panes; ++pane) {
      const float u0=end*pane/panes,u1=end*(pane+1)/panes;
      box(s,p.glass,(u0+u1)*.5f,side*9.42f,21.185f,
          {(u1-u0)*.5f-.025f,.925f,.009f});
      box(s,p.bronze,u0,side*9.42f,21.20f,{.023f,.95f,.045f});
      if(side<0 || u1<=door0 || u0>=door1)lower_pane(u0,u1);
      else {
        lower_pane(u0,std::min(u1,door0));
        lower_pane(std::max(u0,door1),u1);
      }
      if(side<0 || u0<door0-.06f || u0>door1+.06f)
        box(s,p.bronze,u0,side*9.42f,19.05f,{.023f,1.05f,.045f});
    }
    if(side>0)for(float u:{door0,door1})
      box(s,p.bronze,u,9.42f,19.07f,{.055f,1.07f,.07f});
    // A real opening in the complete curtain wall meets the access connector.
    // Columns stand outside the existing v=+/-6 m public platform aisles.
    const int supports=std::max(1,int(std::ceil(end/8.f)));
    for(int i=0;i<=supports;++i) {
      const float u=end*i/supports;
      const float top=hall_roof_y(8.85f)-.20f;
      box(s,p.bronze,u,side*8.85f,18.04f,{.23f,.04f,.23f});
      box(s,p.ceramic,u,side*8.85f,(18.08f+top)*.5f,
          {.095f,(top-18.08f)*.5f,.13f});
      metal.beam(transit_point(u,side*8.85f,21.15f),
                 transit_point(u,side*6.95f,hall_roof_y(6.95f)-.23f),.10f,.15f);
      if(side>0)for(int rib=0;rib<36;++rib) {
        const float a=-9.0f+rib*.5f,b=a+.5f;
        Emit(&s.opaque,p.ceramic).beam(
            transit_point(u,a,hall_roof_y(a)-.25f),
            transit_point(u,b,hall_roof_y(b)-.25f),.14f,.18f);
      }
    }
    // Continuous warm clerestory illumination has real recessed luminaires
    // and discrete sources below their opaque backing, at the same output.
    box(s,p.dark,end*.5f,side*9.09f,21.50f,{end*.495f,.24f,.05f});
    box(s,p.ribbon_light,end*.5f,side*9.16f,21.50f,{end*.493f,.18f,.025f});
    for(float u=2;u<end-1;u+=4) {
      const float half=std::min(1.65f,(end-u)*.5f);
      box(s,p.dark,u,side*8.65f,22.10f,{half,.045f,.10f});
      box(s,p.ribbon_light,u,side*8.65f,22.05f,{half,.009f,.072f});
      s.lights.push_back({transit_point(u,side*8.58f,21.30f),8,
                          {1,.76f,.47f},12.0f});
    }
  }
  // Side-platform entrance portals flank the permanent guideway opening.
  // Each lower end wall stands on its platform, with no glass across a route.
  for(float u:{0.f,end})for(float side:{-1.f,1.f})
    for(auto span:{Vec2{3.5f,4.4f},Vec2{7.6f,9.42f}}) {
      box(s,p.glass,u,side*(span.x+span.y)*.5f,19.125f,
          {.009f,1.025f,(span.y-span.x)*.5f-.025f});
      box(s,p.bronze,u,side*(span.x+span.y)*.5f,18.055f,
          {.065f,.055f,(span.y-span.x)*.5f});
      for(float v:{span.x,span.y})
        box(s,p.bronze,u,side*v,19.07f,{.065f,1.07f,.045f});
    }
  // The guideway continues through an open central portal into the terminal.
  for(float u:{0.f,end}) for(int j=0;j<cross_bays;++j) {
    float a=9.42f*(-1+2.f*j/cross_bays),b=9.42f*(-1+2.f*(j+1)/cross_bays);
    if(std::abs((a+b)*.5f)<3.5f)continue;
    Emit glass(&s.opaque,p.glass);
    Vec3 a0=transit_point(u,a,20.2f),b0=transit_point(u,b,20.2f);
    Vec3 a1=transit_point(u,a,hall_roof_y(a)-.23f),b1=transit_point(u,b,hall_roof_y(b)-.23f);
    glass.quad_metric(a0,b0,b1,a1);
    if(j%5==0)metal.beam(a0,a1,.055f,.065f);
  }
}

void terminal_shell(Scene &s,const Palette &p) {
  const float centre=(kTransitTerminalStart+kTransitLength)*.5f;
  const float axis=(kTransitLength-kTransitTerminalStart)*.5f;
  constexpr float width=8.85f,rise=2.30f,thickness=.18f;
  constexpr int sectors=64,rings=12;
  auto point=[&](float radius,float angle,float offset=0.f) {
    return transit_point(centre+axis*radius*std::cos(angle),
                         width*radius*std::sin(angle),
                         kTransitRoofEaveY+rise*(1-radius*radius)+offset);
  };
  auto normal=[&](float radius,float angle) {
    return normalize(up+along*(2*rise*radius*std::cos(angle)/axis)+
                         left*(2*rise*radius*std::sin(angle)/width));
  };
  for(bool underside:{false,true}) for(int ring=0;ring<rings;++ring)
    for(int i=0;i<sectors;++i) {
      float a=2*kPi*i/sectors,b=2*kPi*(i+1)/sectors;
      float r0=float(ring)/rings,r1=float(ring+1)/rings;
      const std::array<float,4> rr{r0,r1,r1,r0},aa{a,a,b,b};
      std::array<Vec3,4> pos,n;
      std::array<Vec2,4> uv;
      for(int k=0;k<4;++k) {
        pos[k]=point(rr[k],aa[k],underside?-thickness:0);
        n[k]=normal(rr[k],aa[k])*(underside?-1.f:1.f);
        uv[k]={axis*rr[k]*std::cos(aa[k]),width*rr[k]*std::sin(aa[k])};
      }
      surface_triangle(s,underside?p.ceramic:p.shell,{pos[0],pos[1],pos[2]},
                        {n[0],n[1],n[2]},{uv[0],uv[1],uv[2]});
      if(ring)surface_triangle(s,underside?p.ceramic:p.shell,{pos[0],pos[2],pos[3]},
                        {n[0],n[2],n[3]},{uv[0],uv[2],uv[3]});
    }
  for(int i=0;i<sectors;++i) {
    const float a=2*kPi*i/sectors,b=2*kPi*(i+1)/sectors;
    Vec3 a1=point(1,a),b1=point(1,b),a0=a1-up*thickness,b0=b1-up*thickness;
    Emit(&s.opaque,p.shell).quad_metric(a1,b1,b0,a0);
    Emit(&s.opaque,p.dark).beam(a0-up*.055f,b0-up*.055f,.07f,.08f);
    // The rounded hall is enclosed down to its founded perimeter plinth.
    // Two actual rear doorways meet the platform aisles; the central rear
    // portal admits the fixed guideway under the supported roof ring.
    const bool doorway=(i>=22&&i<26)||(i>=38&&i<42);
    const bool guideway_portal=i>=28&&i<36;
    Vec3 ag=point(.965f,a),bg=point(.965f,b);
    ag.y=20.2f;bg.y=20.2f;
    if(!guideway_portal) {
      Emit(&s.opaque,p.glass).quad_metric(bg,ag,ag+up*2.0f,bg+up*2.0f);
      Emit(&s.opaque,p.bronze).beam(ag,bg,.065f,.09f);
      if(!doorway) {
        Vec3 low_a=ag,low_b=bg;low_a.y=18.1f;low_b.y=18.1f;
        Emit(&s.opaque,p.glass).quad_metric(low_b,low_a,ag,bg);
        Vec3 base_a=ag,base_b=bg;base_a.y=17.65f;base_b.y=17.65f;
        Emit(&s.opaque,p.concrete).beam(base_a,base_b,.14f,.90f);
        if(i%4==0)Emit(&s.opaque,p.bronze).beam(low_a,ag,.05f,.06f);
      }
      Vec3 la=point(.94f,a),lb=point(.94f,b);la.y=21.5f;lb.y=21.5f;
      Vec3 back_a=point(.93f,a),back_b=point(.93f,b);back_a.y=21.5f;back_b.y=21.5f;
      Emit(&s.opaque,p.dark).beam(back_a,back_b,.07f,.44f);
      Emit(&s.opaque,p.ribbon_light).beam(la,lb,.05f,.32f);
    }
    Emit(&s.opaque,p.dark).beam(ag+up*2.08f,bg+up*2.08f,.08f,.15f);
    Emit(&s.opaque,p.ceramic).beam(ag+up*2.23f,bg+up*2.23f,.14f,.16f);
    if(i%4==0&&!guideway_portal)
      Emit(&s.opaque,p.bronze).beam(ag,ag+up*2.02f,.05f,.06f);
  }
  for(int jamb:{22,26,28,36,38,42}) {
    Vec3 base=point(.965f,2*kPi*jamb/sectors);base.y=18.0f;
    Vec3 top=base;top.y=22.23f;
    Emit(&s.opaque,p.bronze).beam(base,top,.07f,.09f);
  }
  // Six bearings meet the continuous ring from the outer platform zone.
  // The diagonal eighth-circle positions lie on the v=+/-6 m public aisles.
  for(float angle:{kPi/3,kPi/2,2*kPi/3,4*kPi/3,3*kPi/2,5*kPi/3}) {
    Vec3 top=point(.965f,angle);top.y=22.48f;
    Vec3 base=top;base.y=kTransitWalkY;
    Emit(&s.opaque,p.ceramic).beam(base,top,.14f,.17f);
  }
  // Sparse radial panel seams retain a calm broad skin at the city scale.
  for(int i=0;i<12;++i) for(int r=0;r<rings;++r) {
    const float angle=2*kPi*i/12;
    Emit(&s.opaque,p.shell).beam(point(float(r)/rings,angle,.012f),
        point(float(r+1)/rings,angle,.012f),.014f,.012f);
  }
  for(float u:{centre-6,centre,centre+6})for(float side:{-1.f,1.f}) {
    box(s,p.dark,u,side*6.7f,22.43f,{1.1f,.04f,.10f});
    box(s,p.ribbon_light,u,side*6.7f,22.38f,{1.05f,.008f,.07f});
    s.lights.push_back({transit_point(u,side*6.7f,21.35f),8,{1,.76f,.47f},12.0f});
  }
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
  solid(s, p.concrete, kTransitEndPlatformStart, kTransitLength, -3.5f, 3.5f, 17.2f,
        17.945f);
  solid(s, p.stone, kTransitEndPlatformStart, kTransitLength, -3.5f, 3.5f, 17.945f,
        18);
  for (float u : {kTransitEndPlatformStart, kTransitLength - .12f}) {
    Emit metal(&s.opaque, p.bronze);
    for (float v : {-3.5f, 0.f, 3.5f})
      metal.tube(transit_point(u, v, 18), transit_point(u, v, 19.12f), .028f,
                 10, true);
    for (float y : {18.2f, 18.65f, 19.12f})
      metal.tube(transit_point(u, -3.5f, y), transit_point(u, 3.5f, y), .028f,
                 10, true);
  }
  guard(s, p, .15f, kTransitAccessOffset+8, 9.52f);
  guard(s, p, kTransitAccessOffset+15, kTransitLength-.15f, 9.52f);
  guard(s, p, .15f, kTransitLength-.15f, -9.52f);
  hall_shell(s,p);
  terminal_shell(s,p);
  // Keep the existing planter count, but seat each retained bed in a clear
  // bay rather than cutting a new curtain wall or one of its actual bearings.
  auto bed_position=[&](float proposed,float sign) {
    auto clear=[&](float u) {
      if(u<1 || u>kTransitLength-2)return false;
      if(sign>0 && u>kTransitAccessOffset+8 && u<kTransitAccessOffset+16)return false;
      const int supports=std::max(1,int(std::ceil(kTransitHallEnd/8.f)));
      for(int i=0;i<=supports;++i)
        if(std::abs(u-kTransitHallEnd*i/supports)<1.1f)return false;
      for(int bay=0;bay<7;++bay) {
        const float a=18.f+bay*13.f+(sign<0?3.f:0.f);
        if(a+11.75f>kTransitLength-5)continue;
        if(sign>0 && a+11.75f>=kTransitAccessOffset+7 &&
                      a+3.3f<=kTransitAccessOffset+16)continue;
        if(std::abs(u-(a+4.8f))<2.5f)return false;
      }
      const float centre=(kTransitTerminalStart+kTransitLength)*.5f;
      const float axis=kTransitTerminalLength*.5f*.965f,width=8.85f*.965f;
      const float near_u=std::max(std::abs(u-centre)-.96f,0.f)/axis;
      const float far_u=(std::abs(u-centre)+.96f)/axis;
      const float near_v=(8.45f-.59f)/width,far_v=(8.45f+.59f)/width;
      if(near_u*near_u+near_v*near_v<=1 && far_u*far_u+far_v*far_v>=1)return false;
      for(float angle:{kPi/3,kPi/2,2*kPi/3,4*kPi/3,3*kPi/2,5*kPi/3})
        if(std::abs(u-(centre+axis*std::cos(angle)))<1.1f &&
           std::abs(sign*8.45f-width*std::sin(angle))<.70f)return false;
      return true;
    };
    for(int step=0;step<=48;++step)for(float direction:{1.f,-1.f}) {
      const float u=proposed+direction*step*.25f;
      if(clear(u))return u;
    }
    throw std::runtime_error("Station planter has no clear supported bay");
  };
  for (float sign : {-1.f, 1.f}) {
    // The public furniture stays against the outer platform edge, leaving a
    // continuous walking aisle and the supported access connector clear.
    for (int bay = 0; bay < 7; ++bay) {
      const float a = 18.f + bay * 13.f + (sign < 0 ? 3.f : 0.f);
      if(a+11.75f>kTransitLength-5)continue;
      if(sign>0 && a+11.75f>=kTransitAccessOffset+7 &&
                    a+3.3f<=kTransitAccessOffset+16)continue;
      bench(s, p, a + 4.8f, sign * 8.1f);
      bed(s, p, rng.child(bay + (sign > 0 ? 10 : 20)), bed_position(a+11.75f,sign),
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
