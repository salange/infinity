#include "civic_forecourt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
constexpr float kFloor=1.2f;
using Material=std::uint32_t;
Material named(const Scene& scene,std::string_view name) {
  for(std::size_t i=scene.materials.size();i>0;--i)
    if(scene.materials[i-1].name==name)return static_cast<Material>(i-1);
  throw std::runtime_error("Civic forecourt requires authored material: "+std::string(name));
}
Material material(Scene& scene,MaterialDesc value) {
  for(Material i=0;i<scene.materials.size();++i)
    if(scene.materials[i].name==value.name)return i;
  scene.materials.push_back(std::move(value));
  return static_cast<Material>(scene.materials.size()-1);
}
struct Palette {
  Material stone,wood,bronze,ceramic,soil,bark,dark,glass,water,light;
  explicit Palette(Scene& scene)
      : wood(named(scene,"wood_oiled")),bronze(named(scene,"bronze_satin")),
        ceramic(named(scene,"ceramic_ivory")),soil(named(scene,"soil_mulch")),
        bark(named(scene,"bark_ridged")),dark(named(scene,"gasket_charcoal")),
        glass(named(scene,"glass_clear")) {
    MaterialDesc coping=scene.materials[M_MARBLE_WHITE];
    coping.name="civic forecourt warm honed stone";
    coping.base_color=scene.reviewed_material_maps ? Vec3{.7612f,.6816f,.4857f} : Vec3{.4776f,.4328f,.3433f};
    coping.roughness=.72f;coping.flags=kMatTriplanar;coping.normal_strength=.30f;
    coping.uv_scale=1.4f;coping.metallic=0;stone=material(scene,coping);
    MaterialDesc pond;pond.name="civic shallow reflecting water";
    pond.base_color={.93f,.98f,.98f};pond.roughness=.045f;pond.flags=128u|1024u;
    pond.room_w=1.333f;pond.room_h=.96f;pond.room_d=1.f;pond.lit_probability=.26f;
    water=material(scene,pond);
    MaterialDesc diffuser;diffuser.name="civic forecourt warm diffuser";
    diffuser.base_color={1,.78f,.48f};diffuser.tint2={1,.70f,.35f};
    diffuser.flags=kMatEmissive;diffuser.emissive=2.2f;diffuser.roughness=.44f;
    light=material(scene,diffuser);
    if(!(scene.materials[glass].flags&128u))
      throw std::runtime_error("Civic forecourt requires physical authored glazing");
  }
};

std::uint32_t resource(Scene& scene,const char* name,const std::function<void(Mesh&)>& make) {
  for(std::uint32_t i=0;i<scene.asset_library.resources.size();++i)
    if(scene.asset_library.resources[i].name==name)return i;
  MeshResource r;r.name=name;make(r.mesh);scene.asset_library.resources.push_back(std::move(r));
  return static_cast<std::uint32_t>(scene.asset_library.resources.size()-1);
}
void add(Scene& scene,std::uint32_t id,Vec3 at,float yaw=0) {
  scene.asset_instances.push_back({id,at,yaw,{1,1,1},{1,1,1}});
}

void make_details(Scene& scene,const Palette& p) {
  resource(scene,"forecourt_bronze_lantern",[&](Mesh& mesh) {
    Emit bronze(&mesh,p.bronze),dark(&mesh,p.dark),light(&mesh,p.light);
    dark.box({0,.035f,0},{.185f,.035f,.185f});
    bronze.box({0,.45f,0},{.145f,.415f,.145f});
    for(float x:{-.148f,.148f})light.box({x,.47f,0},{.006f,.25f,.103f});
    for(float z:{-.148f,.148f})light.box({0,.47f,z},{.103f,.25f,.006f});
    for(int i=0;i<14;++i) {
      const float y=.235f+i*.036f;
      for(float x:{-.157f,.157f})bronze.box({x,y,0},{.013f,.005f,.12f});
      for(float z:{-.157f,.157f})bronze.box({0,y,z},{.12f,.005f,.013f});
    }
    bronze.box({0,.90f,0},{.18f,.035f,.18f});
    for(float x:{-.12f,.12f})for(float z:{-.12f,.12f})
      bronze.tube({x,.075f,z},{x,.093f,z},.018f,8,true);
  });
  resource(scene,"forecourt_glazed_shelter",[&](Mesh& mesh) {
    Emit stone(&mesh,p.stone),metal(&mesh,p.bronze),glass(&mesh,p.glass),wood(&mesh,p.wood);
    stone.box({0,.06f,0},{5.7f,.06f,2.75f});
    for(float z:{-2.55f,2.55f}) {
      stone.box({0,.30f,z},{5.55f,.18f,.15f});
      glass.box({0,1.42f,z},{5.40f,.94f,.014f});
      for(int i=0;i<9;++i)metal.box({-5.40f+i*1.35f,1.44f,z},{.035f,.99f,.045f});
    }
    // The barrel has separate inner/outer faces and closed material edges.
    for(int segment=0;segment<36;++segment) {
      float a=kPi*segment/36,b=kPi*(segment+1)/36;
      Vec3 u{0,2.4f+1.20f*std::sin(a),2.55f*std::cos(a)};
      Vec3 v{0,2.4f+1.20f*std::sin(b),2.55f*std::cos(b)};
      glass.quad_metric(u-Vec3{5.45f,0,0},u+Vec3{5.45f,0,0},v+Vec3{5.45f,0,0},v-Vec3{5.45f,0,0});
      glass.quad_metric(u+Vec3{5.45f,-.025f,0},u-Vec3{5.45f,.025f,0},v-Vec3{5.45f,.025f,0},v+Vec3{5.45f,-.025f,0});
      for(float x:{-5.45f,5.45f})
        glass.quad_metric(u+Vec3{x,0,0},u+Vec3{x,-.025f,0},v+Vec3{x,-.025f,0},v+Vec3{x,0,0});
      for(int rib=0;rib<7;++rib) {
        const float x=-5.45f+rib*(10.9f/6);
        metal.beam(u+Vec3{x,.03f,0},v+Vec3{x,.03f,0},.065f,.085f);
      }
    }
    for(float x:{-5.47f,5.47f}) {
      for(float z:{-1.87f,1.87f})glass.box({x,1.42f,z},{.014f,1.30f,.67f});
      for(float z:{-1.17f,1.17f})metal.box({x,1.44f,z},{.055f,1.32f,.055f});
      metal.box({x,2.74f,0},{.055f,.045f,1.22f});
    }
    // Clear entrances at both ends, physical seating on the protected floor.
    for(float x:{-3.2f,3.2f}) {
      for(int i=0;i<7;++i)wood.box({x,.58f,1.25f+i*.08f},{1.42f,.035f,.032f});
      for(float dx:{-1.1f,1.1f})metal.box({x+dx,.32f,1.5f},{.045f,.20f,.30f});
      for(int i=0;i<4;++i)wood.box({x,.80f+i*.085f,1.8f},{1.42f,.032f,.032f});
    }
    metal.box({0,3.26f,0},{3.8f,.03f,.065f});
    Emit(&mesh,p.light).box({0,3.22f,0},{3.7f,.008f,.045f});
  });
}

float bed(Scene& scene,const Palette& p,Vec2 centre,Vec2 half,float base,float height) {
  const auto outer=plan_superellipse(half.x,half.y,2.6f,64,centre);
  const auto inner=plan_superellipse(half.x-.22f,half.y-.22f,2.6f,64,centre);
  Emit stone(&scene.opaque,p.stone);stone.wall(outer,base,base+height,true);
  stone.ring_cap(outer,inner,base+height);stone.wall(inner,base+height-.15f,base+height,true,false);
  const float soil=base+height-.12f;Emit(&scene.opaque,p.soil).polygon(inner,soil,true);
  return soil;
}
void plants(Scene& scene,const Palette& p,Rng rng,Vec2 centre,Vec2 half,float soil) {
  const auto inner=plan_superellipse(half.x-.40f,half.y-.40f,2.6f,64,centre);
  Emit mulch(&scene.opaque,p.bark),hose(&scene.opaque,p.dark);
  const int count=std::max(12,int(half.x*half.y*2.8f));
  for(int i=0;i<count;++i) {
    Vec2 at{centre.x+rng.range(-half.x+.55f,half.x-.55f),centre.y+rng.range(-half.y+.55f,half.y-.55f)};
    if(!point_in_polygon(inner,at))continue;
    const char* name=i%7==0?"shrub_flowering":i%3==0?"fern_arching":i%4==0?"phormium":"groundcover";
    const float scale=i%7==0?rng.range(.43f,.62f):i%3==0?rng.range(.63f,.88f):rng.range(.58f,.92f);
    add_asset_instance(scene,name,{at.x,soil,at.y},rng.range(-kPi,kPi),scale);
  }
  for(int i=0;i<count*7;++i) {
    Vec2 at{centre.x+rng.range(-half.x+.3f,half.x-.3f),centre.y+rng.range(-half.y+.3f,half.y-.3f)};
    if(!point_in_polygon(inner,at))continue;
    const float angle=rng.range(-kPi,kPi);const Vec3 x{std::cos(angle),0,std::sin(angle)},z{-x.z,0,x.x};
    mulch.box({at.x,soil+.014f,at.y},{rng.range(.025f,.09f),.012f,rng.range(.012f,.026f)},x,{0,1,0},z);
  }
  auto loop=plan_superellipse(half.x-.32f,half.y-.32f,2.6f,48,centre);
  for(std::size_t i=0;i<loop.size();++i) {
    Vec2 a=loop[i],b=loop[(i+1)%loop.size()];
    hose.tube({a.x,soil+.015f,a.y},{b.x,soil+.015f,b.y},.012f,5,true);
  }
}
void arc_seat(Scene& scene,const Palette& p,Vec2 centre,Vec2 radius,float y,float start,float end) {
  Emit wood(&scene.opaque,p.wood),metal(&scene.opaque,p.bronze);
  std::vector<Vec3> curve;std::vector<float> distances{0};
  for(int i=0;i<=240;++i) {
    float angle=radians(start+(end-start)*i/240);
    Vec3 at{centre.x+radius.x*std::cos(angle),y,centre.y+radius.y*std::sin(angle)};
    if(!curve.empty())distances.push_back(distances.back()+length(at-curve.back()));
    curve.push_back(at);
  }
  std::size_t segment=1;int count=0;Vec3 previous{};
  for(float d=.05f;d<distances.back()-.05f;d+=.094f,++count) {
    while(segment+1<distances.size()&&distances[segment]<d)++segment;
    float t=(d-distances[segment-1])/(distances[segment]-distances[segment-1]);
    Vec3 at=lerp(curve[segment-1],curve[segment],t),along=normalize(curve[segment]-curve[segment-1]);
    Vec3 side{along.z,0,-along.x};wood.box(at,{.040f,.035f,.29f},along,{0,1,0},side);
    if(count)metal.beam(previous-Vec3{0,.09f,0},at-Vec3{0,.09f,0},.06f,.07f);
    if(count%12==0)metal.box(at-Vec3{0,.26f,0},{.055f,.225f,.22f},along,{0,1,0},side);
    previous=at;
  }
}
void lantern(Scene& scene,const Palette& p,Vec2 at,float base=kFloor) {
  add(scene,asset_resource(scene.asset_library,"forecourt_bronze_lantern"),{at.x,base,at.y});
  scene.lights.push_back({{at.x,base+.48f,at.y},7.5f,{1,.68f,.32f},2.1f});
  (void)p;
}

void reflecting_basin(Scene& scene,const Palette& p) {
  const auto outer=west_civic_floor_openings().front();
  const auto wall=plan_offset(outer,-.18f),wet=plan_offset(outer,-.34f);
  Emit backing(&scene.opaque,M_CONCRETE_DARK),lining(&scene.opaque,p.stone);
  // Structural bottom is .73 m, safely above .65 m terrain. The surrounding
  // city slab must omit outer; its remaining faces meet this flush coping.
  backing.polygon(outer,.73f,false);backing.wall(outer,.73f,kFloor,true);
  lining.polygon(wall,.78f,true);lining.wall(wall,.78f,1.19f,true,false);
  lining.ring_cap(outer,wall,kFloor);lining.ring_cap(wall,wet,1.18f);
  Emit(&scene.opaque,p.water).polygon(wet,1.04f,true);
  Emit metal(&scene.opaque,p.bronze);
  const auto drains=plan_sample(plan_offset(outer,-.245f),.35f);
  for(std::size_t i=0;i<drains.points.size();++i) {
    const Vec2 at=drains.points[i],n=drains.normals[i];
    metal.beam({at.x-n.x*.03f,1.187f,at.y-n.y*.03f},{at.x+n.x*.03f,1.187f,at.y+n.y*.03f},.013f,.008f);
  }
}
}  // namespace

std::vector<std::vector<Vec2>> west_civic_floor_openings() {
  return {plan_superellipse(18.f,5.4f,3.f,64,{-155.f,-88.5f})};
}

void stage_civic_forecourt(Scene& scene,Rng rng) {
  const Palette p(scene);make_details(scene,p);
  reflecting_basin(scene,p);

  // Near-left retained bed and sweeping timber seat enter the actual frame.
  const Vec2 left{-176.5f,-105.1f};
  const float left_soil=bed(scene,p,left,{6.4f,2.5f},kFloor,.57f);
  plants(scene,p,rng.child(1),left,{6.4f,2.5f},left_soil);
  arc_seat(scene,p,left,{6.72f,2.88f},kFloor+.47f,24,156);
  add_asset_instance(scene,"canopy_broadleaf",{-179.5f,left_soil,-106.0f},-.50f,1.18f);

  // Three low, physically filled curved steps lift the opposite planted seat.
  const Vec2 right{-179.f,-91.7f};
  for(int step=0;step<3;++step) {
    auto shape=plan_superellipse(4.1f-step*.35f,3.35f-step*.35f,2.6f,64,right);
    const float top=kFloor+(step+1)*.15f;
    Emit stone(&scene.opaque,p.stone);stone.wall(shape,kFloor,top,true);stone.polygon(shape,top,true);
  }
  const Vec2 right_bed{-179.f,-90.8f};
  const float right_soil=bed(scene,p,right_bed,{2.9f,1.7f},1.65f,.52f);
  plants(scene,p,rng.child(2),right_bed,{2.9f,1.7f},right_soil);
  arc_seat(scene,p,right_bed,{3.20f,2.05f},2.08f,188,352);
  add_asset_instance(scene,"canopy_broadleaf",{-178.f,right_soil,-91.2f},2.7f,1.05f);

  // Farther planted edges retain a varied public court instead of isolated
  // identical garden boxes. The clear eastward route remains at the base level.
  struct EdgeBed {Vec2 centre,half;float height;};
  const std::array<EdgeBed,4> beds{{
      {{-192,-124},{10.0f,3.1f},.58f},{{-163,-130},{12.0f,2.45f},.54f},
      {{-195,-78.5f},{8.5f,3.5f},.61f},{{-148,-71.8f},{11.0f,2.6f},.52f}
  }};
  for(std::size_t i=0;i<beds.size();++i) {
    const auto& b=beds[i];float soil=bed(scene,p,b.centre,b.half,kFloor,b.height);
    plants(scene,p,rng.child(10+i),b.centre,b.half,soil);
    if(i<3) {
      add_asset_instance(scene,i==2?"tree_multistem":"canopy_broadleaf",
                         {b.centre.x,soil,b.centre.y},float(i)*1.25f,i==2?1.10f:.86f);
    }
  }

  // A small occupied glazed civic shelter sits beyond the foreground beds.
  const Vec3 shelter{-139.f,kFloor,-120.f};
  constexpr float shelter_yaw=-kPi*.5f;
  const AssetInstance frame{0,shelter,shelter_yaw,{1,1,1},{1,1,1}};
  add(scene,asset_resource(scene.asset_library,"forecourt_glazed_shelter"),shelter,shelter_yaw);
  for(float x:{-2.6f,2.6f}) {
    Vec3 at=asset_transform_point(frame,{x,.12f,-.55f});
    add_asset_instance(scene,"street_table",at,0,.85f);
    for(float z:{-1.65f,.65f}) {
      at=asset_transform_point(frame,{x,.12f,z});
      add_asset_instance(scene,"street_seat",at,shelter_yaw+(z>0?kPi:0),.85f);
    }
  }
  scene.lights.push_back({shelter+Vec3{0,2.8f,0},8.5f,{1,.76f,.45f},3.f});

  for(Vec2 at:std::array<Vec2,9>{{{-177,-101.1f},{-180.9f,-95.3f},{-168,-96.3f},
      {-153,-96.3f},{-137,-96.3f},{-128,-113},{-146,-113.5f},{-182,-118.5f},{-158,-76.5f}}})
    lantern(scene,p,at);

  // Large inset stone courses interrupt the uniform base paving around the
  // near approach; flush details do not create barriers across the walking line.
  Emit paving(&scene.opaque,p.stone),metal(&scene.opaque,p.bronze);
  for(int row=0;row<3;++row)for(int i=0;i<16;++i) {
    Vec3 at{-183.5f+i*2.9f+(row%2)*1.45f,1.207f,-101.f+row*1.65f};
    paving.box(at,{1.435f,.007f,.807f});
  }
  for(int i=0;i<16;++i) {
    const float x=-184.f+i*2.7f;
    metal.box({x,1.218f,-102.0f},{.008f,.005f,.27f});
  }
}
}  // namespace cb
