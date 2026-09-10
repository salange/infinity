#include "roof_staging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
using Material = std::uint32_t;
constexpr float kBorder = 1.5f;
constexpr float kGap = 1.5f;

Material named(const Scene& scene, std::string_view name) {
  // The authored palette follows the shared city palette. Both contain a
  // glass_clear name, but only the authored pane has the physical-glass ABI.
  for (std::size_t i = scene.materials.size(); i > 0; --i)
    if (scene.materials[i-1].name == name) return static_cast<Material>(i-1);
  throw std::runtime_error("Roof staging requires material: " + std::string(name));
}
Material surface(Scene& scene, const char* name, Vec3 colour, float roughness) {
  for (Material i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name) return i;
  MaterialDesc m; m.name = name; m.base_color = colour; m.roughness = roughness;
  scene.materials.push_back(m);
  return static_cast<Material>(scene.materials.size() - 1);
}
struct Palette {
  Material ceramic, bronze, wood, soil, dark, glass, stone, leaf, linen, light;
  explicit Palette(Scene& scene)
      : ceramic(named(scene, "ceramic_ivory")), bronze(named(scene, "bronze_satin")),
        wood(named(scene, "wood_oiled")), soil(named(scene, "soil_mulch")),
        dark(named(scene, "gasket_charcoal")), glass(named(scene, "glass_clear")),
        stone(named(scene, "stone_warm")), leaf(named(scene, "leaf_middle")),
        linen(surface(scene, "roof warm woven upholstery", {.36f,.31f,.24f}, .92f)),
        light(surface(scene, "roof warm task diffuser", {1,.74f,.42f}, .5f)) {
    if (!(scene.materials[glass].flags & 128u))
      throw std::runtime_error("Roof lantern requires authored transmissive glazing");
    auto& lamp = scene.materials[light]; lamp.flags = kMatEmissive;
    lamp.emissive = .85f; lamp.tint2 = {1,.74f,.42f};
  }
};

std::uint32_t mesh_resource(Scene& scene, const char* name,
                            const std::function<void(Mesh&)>& build) {
  for (std::uint32_t i = 0; i < scene.asset_library.resources.size(); ++i)
    if (scene.asset_library.resources[i].name == name) return i;
  MeshResource resource; resource.name = name; build(resource.mesh);
  scene.asset_library.resources.push_back(std::move(resource));
  return static_cast<std::uint32_t>(scene.asset_library.resources.size() - 1);
}

struct Module {
  std::vector<AssetInstance> parts;
  Vec2 lo{1e30f,1e30f}, hi{-1e30f,-1e30f};
  void add(const Scene& scene, std::uint32_t resource, Vec3 p = {}, float yaw = 0,
           float scale = 1) {
    AssetInstance part{resource,p,yaw,{scale,scale,scale},{1,1,1}};
    const auto& mesh = scene.asset_library.resources.at(resource).mesh;
    for (int i = 0; i < 8; ++i) {
      Vec3 corner{i & 1 ? mesh.bounds_max.x : mesh.bounds_min.x,
                  i & 2 ? mesh.bounds_max.y : mesh.bounds_min.y,
                  i & 4 ? mesh.bounds_max.z : mesh.bounds_min.z};
      const Vec3 q = asset_transform_point(part, corner);
      lo.x = std::min(lo.x,q.x); lo.y = std::min(lo.y,q.z);
      hi.x = std::max(hi.x,q.x); hi.y = std::max(hi.y,q.z);
    }
    parts.push_back(part);
  }
  void add(const Scene& scene, std::string_view resource, Vec3 p = {},
           float yaw = 0, float scale = 1) {
    add(scene, asset_resource(scene.asset_library,resource), p, yaw, scale);
  }
};

void bench(Mesh& mesh, const Palette& p, Vec3 centre, float width) {
  Emit wood(&mesh,p.wood), metal(&mesh,p.bronze);
  for (int i = 0; i < 7; ++i)
    wood.box(centre + Vec3{0,.45f,-.255f+i*.085f}, {width*.5f,.035f,.034f});
  for (float x : {-width*.38f,width*.38f}) {
    metal.box(centre + Vec3{x,.215f,0}, {.045f,.215f,.26f});
    metal.beam(centre+Vec3{x,.30f,.27f},centre+Vec3{x,.91f,.39f},.055f,.055f);
  }
  for (int i = 0; i < 4; ++i)
    wood.box(centre + Vec3{0,.62f+i*.085f,.33f+i*.017f}, {width*.5f,.033f,.03f});
}

void planter(Mesh& mesh, const Palette& p, Vec2 centre, Vec2 half, float height) {
  auto outer = plan_rounded_rect(half.x,half.y,.22f,5,centre);
  auto inner = plan_rounded_rect(half.x-.10f,half.y-.10f,.14f,5,centre);
  Emit e(&mesh,p.ceramic); e.wall(outer,0,height,true);
  e.ring_cap(outer,inner,height); e.wall(inner,height-.14f,height,true,false);
  Emit(&mesh,p.soil).polygon(inner,height-.10f,true);
  Emit(&mesh,p.bronze).wall(outer,.08f,.105f,true);
}

void shade(Mesh& mesh, const Palette& p, Vec2 half, float height, bool vaulted) {
  Emit ceramic(&mesh,p.ceramic), bronze(&mesh,p.bronze), wood(&mesh,p.wood);
  for (float x : {-half.x,half.x}) for (float z : {-half.y,half.y}) {
    bronze.box({x,.055f,z},{.19f,.055f,.19f});
    ceramic.box({x,height*.5f,z},{.09f,height*.5f,.11f});
    bronze.box({x,height-.12f,z},{.14f,.045f,.16f});
    for (float dx : {-.13f,.13f}) for (float dz : {-.13f,.13f})
      bronze.tube({x+dx,.108f,z+dz},{x+dx,.13f,z+dz},.019f,8,true);
  }
  auto roof_y = [&](float x) {
    return height + (vaulted ? .53f*(1-x*x/(half.x*half.x)) : 0.f);
  };
  for (float z : {-half.y,0.f,half.y}) {
    for (int i = 0; i < 24; ++i) {
      float a = -half.x+2*half.x*i/24, b = -half.x+2*half.x*(i+1)/24;
      ceramic.beam({a,roof_y(a),z},{b,roof_y(b),z},.15f,.17f);
    }
  }
  for (int i = 0; i <= int(half.x*2/.16f); ++i) {
    const float x = -half.x+i*.16f;
    wood.box({x,roof_y(x)+.13f,0},{.046f,.09f,half.y+.16f});
  }
  for (float x : {-half.x,half.x})
    bronze.box({x,height+.045f,0},{.06f,.05f,half.y+.10f});
}

void floor_inlay(Mesh& mesh, const Palette& p, Vec2 half, bool timber) {
  Emit e(&mesh,timber ? p.wood : p.stone);
  if (timber) {
    for (float x = -half.x+.06f; x < half.x; x += .13f)
      e.box({x,.025f,0},{.057f,.025f,half.y});
  } else {
    for (float x = -half.x+.4f; x < half.x; x += .8f)
      for (float z = -half.y+.4f; z < half.y; z += .8f)
        e.box({x,.015f,z},{.392f,.015f,.392f});
  }
}

Module residential(Scene& scene, const Palette& p) {
  Module m;
  m.add(scene,mesh_resource(scene,"roof_residential_shade",[&](Mesh& mesh) {
    floor_inlay(mesh,p,{3.25f,2.45f},true);
    shade(mesh,p,{3.0f,2.2f},2.9f,false);
    bench(mesh,p,{-1.8f,.05f,.95f},2.0f);
    bench(mesh,p,{1.8f,.05f,.95f},2.0f);
    planter(mesh,p,{0,-1.50f},{2.6f,.50f},.60f);
    Emit(&mesh,p.bronze).box({0,2.72f,.1f},{1.2f,.035f,.045f});
    Emit(&mesh,p.light).box({0,2.68f,.1f},{1.14f,.008f,.034f});
  }));
  for (int i = 0; i < 5; ++i) {
    m.add(scene,"groundcover",{-2.0f+i, .50f,-1.5f},i*.8f,.55f);
    if (i % 2 == 0) m.add(scene,"phormium",{-2.0f+i,.50f,-1.5f},i*.7f,.56f);
  }
  m.add(scene,"street_table",{0,.05f,.50f},0,.8f);
  return m;
}

Module workplace(Scene& scene, const Palette& p) {
  Module m;
  m.add(scene,mesh_resource(scene,"roof_work_curved_canopy",[&](Mesh& mesh) {
    floor_inlay(mesh,p,{5.15f,3.25f},false);
    shade(mesh,p,{4.8f,2.9f},3.45f,true);
    for (float x : {-2.15f,2.15f}) {
      Emit wood(&mesh,p.wood), metal(&mesh,p.bronze);
      for (int i = 0; i < 12; ++i)
        wood.box({x,.78f,-.65f+i*.115f},{1.36f,.035f,.049f});
      for (float dx : {-1.08f,1.08f}) metal.box({x+dx,.38f,0},{.06f,.38f,.48f});
      for (float z : {-1.05f,1.05f}) {
        wood.box({x,.47f,z},{1.38f,.036f,.21f});
        for (float dx : {-1.05f,1.05f}) metal.box({x+dx,.23f,z},{.045f,.23f,.19f});
      }
      Emit(&mesh,p.linen).box({x+.28f,.832f,.13f},{.17f,.016f,.24f});
      Emit(&mesh,p.ceramic).tube({x-.35f,.817f,-.21f},{x-.35f,.915f,-.21f},.048f,18,true);
    }
    for (float x : {-4.20f,4.20f}) planter(mesh,p,{x,0},{.43f,2.2f},.65f);
  }));
  for (float x : {-4.20f,4.20f}) for (int i = 0; i < 4; ++i)
    m.add(scene,"phormium",{x,.55f,-1.6f+i*1.05f},i,.48f);
  return m;
}

Module civic(Scene& scene, const Palette& p) {
  Module m;
  m.add(scene,mesh_resource(scene,"roof_civic_glazed_lantern",[&](Mesh& mesh) {
    floor_inlay(mesh,p,{4.15f,2.35f},false);
    Emit ceramic(&mesh,p.ceramic), metal(&mesh,p.bronze), glass(&mesh,p.glass);
    for (float z : {-2.35f,2.35f}) {
      ceramic.box({0,.25f,z},{4.2f,.25f,.13f});
      metal.box({0,2.05f,z},{4.2f,.055f,.055f});
      glass.box({0,1.25f,z},{4.05f,.75f,.012f});
      for (int i = 0; i < 8; ++i) metal.box({-4.05f+i*1.16f,1.27f,z},{.036f,.78f,.04f});
    }
    // This is the roof of an occupied rooftop room, not a fake opening through
    // the intact supporting slab. Both barrel faces and every edge are present.
    constexpr int arc_steps = 32;
    for (int j = 0; j < arc_steps; ++j) {
      const float a = kPi*j/arc_steps, b = kPi*(j+1)/arc_steps;
      Vec3 za{0,2.05f+1.25f*std::sin(a),2.35f*std::cos(a)};
      Vec3 zb{0,2.05f+1.25f*std::sin(b),2.35f*std::cos(b)};
      glass.quad_metric(za-Vec3{4.1f,0,0},za+Vec3{4.1f,0,0},zb+Vec3{4.1f,0,0},zb-Vec3{4.1f,0,0});
      glass.quad_metric(za+Vec3{4.1f,-.025f,0},za-Vec3{4.1f,.025f,0},zb-Vec3{4.1f,.025f,0},zb+Vec3{4.1f,-.025f,0});
      for (float x : {-4.1f,4.1f})
        glass.quad_metric(za+Vec3{x,0,0},za+Vec3{x,-.025f,0},zb+Vec3{x,-.025f,0},zb+Vec3{x,0,0});
      for (float x : {-4.1f,-2.05f,0.f,2.05f,4.1f})
        metal.beam(za+Vec3{x,.018f,0},zb+Vec3{x,.018f,0},.055f,.07f);
    }
    for (float x : {-4.13f,4.13f}) {
      for (float z : {-1.72f,1.72f}) glass.box({x,1.28f,z},{.013f,1.25f,.58f});
      for (float z : {-1.08f,1.08f}) metal.box({x,1.30f,z},{.055f,1.3f,.055f});
      metal.box({x,2.55f,0},{.07f,.05f,1.12f});
      // A real 2.1 m clear entrance sits between the side panes at either end.
    }
    bench(mesh,p,{0,.03f,1.35f},4.8f);
    Emit(&mesh,p.light).box({0,2.85f,0},{2.4f,.025f,.045f});
  }));
  for (float x : {-2.1f,2.1f}) m.add(scene,"street_table",{x,.03f,-.10f},0,.8f);
  return m;
}

void heat_pump(Mesh& mesh, const Palette& p, Vec3 centre) {
  Emit metal(&mesh,p.bronze), ceramic(&mesh,p.ceramic), dark(&mesh,p.dark);
  ceramic.box(centre+Vec3{0,.73f,0},{.90f,.65f,1.25f});
  for (float z : {-.64f,.64f}) {
    const Vec3 c=centre+Vec3{0,1.397f,z};
    dark.tube(c,c+Vec3{0,.012f,0},.52f,32,true);
    metal.torus(c+Vec3{0,.018f,0},{0,1,0},.52f,.024f,32,8);
    metal.tube(c,c+Vec3{0,.08f,0},.10f,16,true);
    for (int blade=0;blade<7;++blade) {
      const float a=blade*2*kPi/7;Vec3 axis{std::cos(a),0,std::sin(a)};
      metal.beam(c+axis*.13f+Vec3{0,.04f,0},c+axis*.46f+Vec3{0,.04f,0},.17f,.017f);
    }
    for (int bar=-5;bar<=5;++bar) {
      float x=bar*.083f,h=std::sqrt(.49f*.49f-x*x);
      metal.tube(c+Vec3{x,.09f,-h},c+Vec3{x,.09f,h},.008f,5,true);
    }
  }
  for (float x : {-.915f,.915f}) for (int slat=0;slat<13;++slat)
    metal.box(centre+Vec3{x,.24f+slat*.079f,0},{.018f,.019f,1.15f});
  for (float x : {-.65f,.65f}) for (float z : {-1.f,1.f})
    dark.box(centre+Vec3{x,.05f,z},{.13f,.05f,.13f});
}

Module service(Scene& scene, const Palette& p) {
  Module m;
  m.add(scene,mesh_resource(scene,"roof_screened_mechanical_court",[&](Mesh& mesh) {
    Emit dark(&mesh,p.dark), metal(&mesh,p.bronze), wood(&mesh,p.wood);
    dark.box({0,.035f,0},{3.25f,.035f,3.05f});
    heat_pump(mesh,p,{-1.70f,.07f,-.75f});
    heat_pump(mesh,p,{1.70f,.07f,-.75f});
    for (float x : {-3.15f,3.15f}) {
      for (float z : {-2.9f,0.f,2.9f}) metal.box({x,1.18f,z},{.055f,1.18f,.055f});
      for (int i=0;i<18;++i) wood.box({x,.20f+i*.12f,0},{.055f,.034f,2.95f});
    }
    for (int i=0;i<18;++i) wood.box({0,.20f+i*.12f,-2.95f},{3.15f,.034f,.055f});
    // Front remains open for a 1.8 m maintenance aisle; services join machines.
    for (float x : {-1.7f,1.7f}) {
      metal.tube({x,.20f,-2.1f},{x,.20f,-2.62f},.065f,12,true);
      metal.tube({x,.20f,-2.62f},{0,.20f,-2.62f},.065f,12,true);
    }
    metal.box({0,.085f,-2.66f},{.28f,.035f,.23f});
    for (int i=0;i<10;++i) metal.box({-.24f+i*.052f,.122f,-2.66f},{.014f,.006f,.21f});
  }));
  return m;
}

Module market(Scene& scene, const Palette& p) {
  Module m;
  m.add(scene,mesh_resource(scene,"roof_market_kitchen_garden",[&](Mesh& mesh) {
    for (float x : {-2.2f,2.2f}) {
      planter(mesh,p,{x,0},{.75f,2.7f},.72f);
      for (int i=0;i<3;++i) Emit(&mesh,p.wood).box({x,.725f,-1.8f+i*1.8f},{.70f,.018f,.025f});
    }
    Emit wood(&mesh,p.wood), bronze(&mesh,p.bronze);
    wood.box({0,.92f,-2.35f},{.72f,.045f,.40f});
    wood.box({0,.30f,-2.35f},{.64f,.025f,.33f});
    for (float x : {-.58f,.58f}) for (float z : {-2.65f,-2.05f})
      bronze.box({x,.45f,z},{.035f,.45f,.035f});
    for (int i=0;i<4;++i) {
      const Vec3 at{-.48f+i*.31f,.97f,-2.35f};
      Emit(&mesh,p.stone).frustum(at,at+Vec3{0,.19f,0},.08f,.11f,14,false);
      Emit(&mesh,p.soil).tube(at+Vec3{0,.16f,0},at+Vec3{0,.17f,0},.10f,14,true);
    }
    bronze.box({0,.045f,0},{.15f,.03f,1.4f});
    for (int i=0;i<24;++i) bronze.box({0,.079f,-1.32f+i*.115f},{.13f,.005f,.016f});
  }));
  for (float x : {-2.2f,2.2f}) for (int i=0;i<6;++i) {
    m.add(scene,i%3==0 ? "shrub_flowering" : "groundcover",
          {x,.62f,-2.12f+i*.84f},i*1.13f,i%3==0?.42f:.54f);
  }
  return m;
}

Module secondary(Scene& scene, const Palette& p, int type) {
  Module m;
  if (type==0) {
    m.add(scene,mesh_resource(scene,"roof_daybed_pair",[&](Mesh& mesh) {
      floor_inlay(mesh,p,{2.45f,1.95f},true);
      for (float x : {-1.25f,1.25f}) {
        Emit(&mesh,p.bronze).box({x,.19f,0},{.48f,.19f,1.12f});
        Emit(&mesh,p.linen).box({x,.43f,0},{.54f,.065f,1.13f});
        Emit(&mesh,p.linen).box({x,.56f,.77f},{.49f,.055f,.29f});
      }
    }));
    m.add(scene,"street_table",{0,.05f,0},0,.52f);
  } else if (type==1) {
    m.add(scene,mesh_resource(scene,"roof_stone_planter_seat",[&](Mesh& mesh) {
      planter(mesh,p,{0,-.50f},{2.05f,.58f},.68f);
      bench(mesh,p,{0,0,.43f},3.8f);
    }));
    for (int i=0;i<4;++i) {
      m.add(scene,"groundcover",{-1.4f+i*.92f,.58f,-.5f},i,.50f);
      m.add(scene,"phormium",{-1.4f+i*.92f,.58f,-.5f},i+.7f,.53f);
    }
  } else if (type==2) {
    m.add(scene,mesh_resource(scene,"roof_access_headhouse",[&](Mesh& mesh) {
      Emit ceramic(&mesh,p.ceramic), bronze(&mesh,p.bronze), glass(&mesh,p.glass);
      ceramic.box({-1.30f,1.4f,0},{.10f,1.4f,1.75f});
      ceramic.box({1.30f,1.4f,0},{.10f,1.4f,1.75f});
      ceramic.box({0,1.4f,-1.65f},{1.30f,1.4f,.10f});
      ceramic.box({0,2.85f,0},{1.48f,.12f,1.93f});
      for (float x : {-1.075f,1.075f}) ceramic.box({x,1.4f,1.65f},{.125f,1.4f,.10f});
      ceramic.box({0,2.52f,1.65f},{.95f,.28f,.10f});
      glass.box({0,1.18f,1.66f},{.93f,1.15f,.015f});
      for (float x : {-.96f,0.f,.96f}) bronze.box({x,1.2f,1.69f},{.035f,1.18f,.025f});
      for (float x : {-.12f,.12f}) bronze.tube({x,1.0f,1.74f},{x,1.38f,1.74f},.018f,10,true);
      for (int i=0;i<12;++i) bronze.box({0,3.005f,-1.65f+i*.30f},{1.4f,.025f,.012f});
      bronze.box({0,.04f,2.08f},{1.18f,.025f,.13f});
    }));
  } else if (type==3) {
    const auto rack=mesh_resource(scene,"roof_solar_rack",[&](Mesh& mesh) {
      const auto& source=scene.asset_library.resources[asset_resource(scene.asset_library,"roof_solar_panel")].mesh;
      const float angle=radians(12.f),c=std::cos(angle),s=std::sin(angle);
      auto rotate=[&](Vec3 v) {return Vec3{v.x,c*v.y-s*v.z,s*v.y+c*v.z};};
      for (auto vertex:source.vertices) {
        vertex.position=rotate(vertex.position-Vec3{0,.385f,0})+Vec3{0,.385f+s*.8f,0};
        vertex.normal=rotate(vertex.normal);
        vertex.tangent={rotate(vertex.tangent.xyz()),vertex.tangent.w};
        mesh.add_vertex(vertex);
      }
      mesh.indices=source.indices;
      Emit metal(&mesh,p.bronze),dark(&mesh,p.dark);
      for(float x:{-1.15f,1.15f})for(float z:{-.60f,.60f}) {
        const float top=.385f+s*.8f-s*z;
        dark.box({x,.035f,z},{.13f,.035f,.16f});
        metal.box({x,(top+.07f)*.5f,z},{.035f,(top-.07f)*.5f,.035f});
      }
      for(float x:{-1.15f,1.15f})
        metal.beam({x,.385f+s*1.4f,-.6f},{x,.385f+s*.2f,.6f},.05f,.045f);
      dark.tube({-1.3f,.15f,0},{1.3f,.15f,0},.018f,6,true);
      metal.box({0,.14f,0},{.13f,.08f,.09f});
    });
    for (int i=0;i<4;++i)
      m.add(scene,rack,{-2.7f+(i%2)*3.3f,0,-1.7f+(i/2)*3.0f},0,1.f);
  } else if (type==4) {
    m.add(scene,mesh_resource(scene,"roof_water_recovery_station",[&](Mesh& mesh) {
      Emit metal(&mesh,p.bronze), ceramic(&mesh,p.ceramic), dark(&mesh,p.dark);
      for (float x : {-.9f,.9f}) {
        dark.box({x,.08f,0},{.67f,.08f,.67f});
        ceramic.tube({x,.16f,0},{x,2.30f,0},.62f,36,true);
        for (float y : {.25f,1.22f,2.20f}) metal.torus({x,y,0},{0,1,0},.625f,.035f,36,8);
        metal.tube({x,2.30f,0},{x,2.36f,0},.18f,24,true);
        metal.tube({x,.34f,.60f},{x,.34f,.95f},.045f,12,true);
        metal.torus({x,.46f,.84f},{0,1,0},.12f,.016f,16,6);
      }
      metal.tube({-.9f,.34f,.95f},{.9f,.34f,.95f},.045f,12,true);
    }));
    m.add(scene,"roof_irrigation",{2.1f,0,0},kPi*.5f,.80f);
  } else {
    m.add(scene,"street_table",{0,0,0},0,1.f);
    for (int i=0;i<4;++i) {
      const float a=i*kPi*.5f;
      m.add(scene,"street_seat",{std::sin(a)*1.38f,0,std::cos(a)*1.38f},a+kPi,.76f);
    }
  }
  return m;
}

float cross2(Vec2 a, Vec2 b) { return a.x*b.y-a.y*b.x; }
float point_segment_distance(Vec2 p, Vec2 a, Vec2 b) {
  const Vec2 d=b-a;
  const float t=std::clamp(dot(p-a,d)/std::max(dot(d,d),1e-12f),0.f,1.f);
  return length(p-(a+d*t));
}
bool segments_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  const float ab_c=cross2(b-a,c-a),ab_d=cross2(b-a,d-a);
  const float cd_a=cross2(d-c,a-c),cd_b=cross2(d-c,b-c);
  if (((ab_c>0 && ab_d<0)||(ab_c<0 && ab_d>0)) &&
      ((cd_a>0 && cd_b<0)||(cd_a<0 && cd_b>0))) return true;
  return point_segment_distance(a,c,d)<1e-5f || point_segment_distance(b,c,d)<1e-5f ||
         point_segment_distance(c,a,b)<1e-5f || point_segment_distance(d,a,b)<1e-5f;
}
bool boundaries_intersect(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
  for (std::size_t i=0;i<a.size();++i) for (std::size_t j=0;j<b.size();++j)
    if (segments_intersect(a[i],a[(i+1)%a.size()],b[j],b[(j+1)%b.size()])) return true;
  return false;
}
bool overlap(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
  if (a.size()<3 || b.size()<3) return false;
  return boundaries_intersect(a,b) || point_in_polygon(a,b.front()) || point_in_polygon(b,a.front());
}
std::vector<Vec2> footprint(const Module& module,Vec2 centre,float yaw,float margin) {
  const float c=std::cos(yaw),s=std::sin(yaw);
  std::vector<Vec2> corners{{module.lo.x-margin,module.lo.y-margin},
                             {module.hi.x+margin,module.lo.y-margin},
                             {module.hi.x+margin,module.hi.y+margin},
                             {module.lo.x-margin,module.hi.y+margin}};
  for (auto& q:corners) q=centre+Vec2{c*q.x+s*q.y,-s*q.x+c*q.y};
  return corners;
}
bool contained(const std::vector<Vec2>& outer,const std::vector<Vec2>& inner) {
  for (Vec2 p:inner) if (!point_in_polygon(outer,p)) return false;
  return !boundaries_intersect(outer,inner);
}

}  // namespace

void stage_occupied_roof(Scene& scene,const std::vector<Vec2>& roof,float floor_y,
                         const std::vector<std::vector<Vec2>>& exclusions,
                         RoofUse use,Rng rng) {
  if (roof.size()<3 || std::abs(plan_area(roof))<24.f) return;
  if (!std::isfinite(floor_y)) throw std::runtime_error("Roof staging needs a finite floor height");
  for (Vec2 p:roof) if (!std::isfinite(p.x+p.y))
    throw std::runtime_error("Roof staging needs finite polygon coordinates");
  for(const auto& exclusion:exclusions)for(Vec2 p:exclusion)
    if(!std::isfinite(p.x+p.y))throw std::runtime_error("Roof staging needs finite exclusion coordinates");
  const Palette p(scene);
  Module primary = use==RoofUse::ResidentialGarden ? residential(scene,p) :
                   use==RoofUse::WorkTerrace ? workplace(scene,p) :
                   use==RoofUse::CivicRoof ? civic(scene,p) :
                   use==RoofUse::ServiceRoof ? service(scene,p) : market(scene,p);
  std::array<Module,6> details;
  for (int i=0;i<6;++i) details[i]=secondary(scene,p,i);
  const std::array<std::array<int,5>,5> uses{{
      {{0,1,5,1,2}}, {{5,1,2,5,3}}, {{1,5,2,1,3}}, {{3,4,2,3,4}}, {{1,4,5,2,1}}
  }};
  const auto& order=uses.at(static_cast<std::size_t>(use));
  Vec2 lo,hi; plan_bounds(roof,&lo,&hi);
  Vec2 axis=plan_long_axis(roof);
  float base_yaw=std::atan2(-axis.y,axis.x);
  float usable_area=std::abs(plan_area(roof));
  for (const auto& exclusion:exclusions) usable_area-=std::abs(plan_area(exclusion));
  const int count=std::clamp(int(std::max(0.f,usable_area)/110.f),2,20);
  std::vector<std::vector<Vec2>> occupied;
  for (int item=0;item<count;++item) {
    Rng placement=rng.child(100+item);
    const Module& module=(item==0 || (item%6==0 && use!=RoofUse::CivicRoof)) ? primary :
                         details[order[(item-1)%order.size()]];
    for (int attempt=0;attempt<180;++attempt) {
      const Vec2 centre{placement.range(lo.x,hi.x),placement.range(lo.y,hi.y)};
      const float yaw=base_yaw+placement.irange(0,3)*kPi*.5f;
      const auto border=footprint(module,centre,yaw,kBorder);
      if (!contained(roof,border)) continue;
      bool clear=true;
      for (const auto& exclusion:exclusions) if (overlap(border,exclusion)) { clear=false;break; }
      if (!clear) continue;
      const auto separation=footprint(module,centre,yaw,kGap*.5f);
      for (const auto& previous:occupied) if (overlap(separation,previous)) { clear=false;break; }
      if (!clear) continue;
      AssetInstance frame{0,{centre.x,floor_y,centre.y},yaw,{1,1,1},{1,1,1}};
      for (auto part:module.parts) {
        part.translation=asset_transform_point(frame,part.translation);part.yaw+=yaw;
        scene.asset_instances.push_back(part);
      }
      occupied.push_back(separation);
      break;
    }
  }
}
}  // namespace cb
