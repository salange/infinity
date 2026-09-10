#include "canal_tower.hpp"
#include "city/towers.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

namespace cb {
namespace {
using Material = Mat;
Material surface(Scene& scene, MaterialDesc recipe, const char* name,
                 Vec3 color, float roughness, float metallic) {
  recipe.name=name;recipe.base_color=color;recipe.roughness=roughness;recipe.metallic=metallic;
  scene.materials.push_back(std::move(recipe));
  return static_cast<Material>(scene.materials.size()-1);
}
struct Palette {
  Material frame,bronze,glass,neck,ring,deck,roof;
  explicit Palette(Scene& s) {
    frame=surface(s,s.materials[M_WHITE_METAL],"canal tower formed ivory cells",{.86f,.82f,.72f},.27f,0);
    s.materials[frame].normal_strength=.055f;
    bronze=surface(s,s.materials[M_BRONZE],"canal tower fine bronze framing",{.24f,.145f,.072f},.29f,.65f);
    glass=surface(s,s.materials[M_GLASS_BRONZE],"canal tower graduated occupied glass",{.48f,.33f,.19f},.085f,.44f);
    auto& pane=s.materials[glass];pane.room_h=4;pane.room_w=2.35f;
    pane.tint2={.95f,.76f,.46f};pane.lit_probability=.36f;
    neck=surface(s,pane,"canal tower upper bronze clerestory",{.37f,.255f,.145f},.08f,.49f);
    s.materials[neck].lit_probability=.12f;
    ring=surface(s,s.materials[M_WHITE_METAL],"canal tower satin champagne crown rings",{.88f,.83f,.71f},.24f,.32f);
    deck=surface(s,s.materials[M_DARK_METAL],"canal tower recessed crown walk",{.12f,.09f,.055f},.49f,.35f);
    roof=surface(s,s.materials[M_WHITE_METAL],"canal tower pale formed pavilion roof",{.97f,.90f,.76f},.38f,0);
    s.materials[roof].normal_strength=.025f;
  }
};
Vec3 radial(float a) {return {std::cos(a),0,std::sin(a)};}
Vec3 at(const CanalTowerSpec& s,float a,float y,float r) {return P3(s.centre,y)+radial(a)*r;}

// A closed radial section creates the real underside, bevels and upper lip.
// Section coordinates are (radius, height); no camera-facing substitute is used.
void formed_ring(Scene& scene,const CanalTowerSpec& s,const std::vector<Vec2>& section,
                 Material material,int segments=144) {
  Emit emit(&scene.opaque,material);
  for(int i=0;i<segments;++i) {
    const float a=i*2*kPi/segments,b=(i+1)*2*kPi/segments;
    for(std::size_t edge=0;edge<section.size();++edge) {
      const auto p=section[edge],q=section[(edge+1)%section.size()];
      emit.quad_metric(at(s,a,p.y,p.x),at(s,a,q.y,q.x),at(s,b,q.y,q.x),at(s,b,p.y,p.x));
    }
  }
}
void member(Scene& scene,Vec3 a,Vec3 b,Vec3 outward,Material face,Material bronze) {
  const Vec3 along=normalize(b-a),u=normalize(cross(outward,along)),v=normalize(cross(along,u));
  constexpr float w=.205f,d=.125f,bevel=.040f;
  const std::array<Vec2,8> section{{{-w+bevel,-d},{w-bevel,-d},{w,-d+bevel},{w,d-bevel},
    {w-bevel,d},{-w+bevel,d},{-w,d-bevel},{-w,-d+bevel}}};
  Emit emit(&scene.opaque,face);
  Emit(&scene.opaque,bronze).beam(a-v*.07f,b-v*.07f,.38f,.17f,outward);
  for(std::size_t i=0;i<section.size();++i) {
    const auto p=section[i],q=section[(i+1)%section.size()];
    const Vec3 one=u*p.x+v*p.y,two=u*q.x+v*q.y;
    emit.quad_metric(a+one,a+two,b+two,b+one);
    emit.triangle(a,a+two,a+one);emit.triangle(b,b+one,b+two);
  }
}
void skin(Scene& scene,const CanalTowerSpec& s,float lo,float hi,Material material,bool doorway=false) {
  Emit pane(&scene.opaque,material);
  for(int i=0;i<144;++i) {
    const float a=i*2*kPi/144,b=(i+1)*2*kPi/144;
    if(doorway&&(std::min(a,2*kPi-a)<.24f||std::min(b,2*kPi-b)<.24f))continue;
    // Keep one metre-scale room grid around the cylinder. Restarting U on
    // every narrow panel repeats one room and erases the occupied variation.
    const float u0=a*(s.neck_radius-.32f),u1=b*(s.neck_radius-.32f);
    pane.quad(at(s,b,lo,canal_tower_radius(s,lo)-.32f),at(s,a,lo,canal_tower_radius(s,lo)-.32f),
      at(s,a,hi,canal_tower_radius(s,hi)-.32f),at(s,b,hi,canal_tower_radius(s,hi)-.32f),
      QuadUV{{u1,lo-s.base_y},{u0,lo-s.base_y},{u0,hi-s.base_y},{u1,hi-s.base_y}});
  }
}
} // namespace

float canal_tower_radius(const CanalTowerSpec& s,float y) {
  const float t=std::clamp((y-s.base_y)/(s.crown_floor-s.base_y),0.f,1.f);
  return s.base_radius+(s.neck_radius-s.base_radius)*t+.18f*std::sin(t*kPi);
}

void build_canal_tower(Scene& scene,const CanalTowerSpec& s) {
  const Palette p(scene);
  // A continuous core carries the tower through the occupied podium floors.
  Emit(&scene.opaque,M_CONCRETE_WHITE).box(P3(s.centre,(1.2f+s.crown_floor)*.5f),
      {6.2f,(s.crown_floor-1.2f)*.5f,6.2f});
  slab(scene.opaque,plan_circle(s.base_radius,144,s.centre),s.base_y,.24f,p.roof);
  skin(scene,s,s.base_y+.15f,s.base_y+7.82f,M_GLASS_CLEAR,true);
  for(int column=0;column<12;++column) {
    const float a=(column+.5f)*2*kPi/12;
    Emit(&scene.opaque,p.bronze).beam(at(s,a,s.base_y,s.base_radius-.75f),
      at(s,a,s.base_y+8,canal_tower_radius(s,s.base_y+8)-.75f),.25f,.32f,radial(a));
  }
  for(float floor=s.base_y+8;floor<s.crown_floor-.01f;floor+=4) {
    const float top=std::min(floor+4,s.crown_floor);
    slab(scene.opaque,plan_circle(canal_tower_radius(s,floor)-.30f,144,s.centre),floor,.18f,p.bronze);
    skin(scene,s,floor,top,top>82?p.neck:p.glass);
    for(int column=0;column<72;++column) {
      const float a=column*2*kPi/72;
      Emit(&scene.opaque,p.bronze).beam(at(s,a,floor+.02f,canal_tower_radius(s,floor)-.29f),
        at(s,a,top-.02f,canal_tower_radius(s,top)-.29f),.048f,.065f,radial(a));
    }
  }
  // Complete cells leave the characteristic toothed edge under the dark neck.
  // Adjacent cells share one physical member, including the cylindrical seam.
  const float lattice_base=s.base_y+8,reference_radius=s.neck_radius+.32f;
  const float circumference=2*kPi*reference_radius,w=circumference/s.columns,h=s.cell_height;
  auto point=[&](Vec2 q) {return at(s,q.x/reference_radius,lattice_base+q.y,
                                 canal_tower_radius(s,lattice_base+q.y)+.32f);};
  std::set<std::array<int,4>> edges;
  for(int row=0;row<s.cell_rows;++row)for(int column=0;column<s.columns;++column) {
    const float x=(column+(row%2)*.5f)*w,y=h*.5f+row*h*.75f;
    const std::array<Vec2,6> hex{{{x,y-h*.5f},{x+w*.5f,y-h*.25f},{x+w*.5f,y+h*.25f},
      {x,y+h*.5f},{x-w*.5f,y+h*.25f},{x-w*.5f,y-h*.25f}}};
    for(int edge=0;edge<6;++edge) {
      const Vec2 a=hex[edge],b=hex[(edge+1)%6];
      auto key=[&](Vec2 q) {return std::array<int,2>{int(std::lround(std::fmod(q.x+circumference*2,circumference)*1000)),int(std::lround(q.y*1000))};};
      auto ka=key(a),kb=key(b);if(kb<ka)std::swap(ka,kb);
      if(!edges.insert({ka[0],ka[1],kb[0],kb[1]}).second)continue;
      for(int part=0;part<4;++part) {
        const Vec2 u=a+(b-a)*(part/4.f),v=a+(b-a)*((part+1)/4.f);
        member(scene,point(u),point(v),radial((u.x+v.x)*.5f/reference_radius),p.frame,p.bronze);
      }
    }
  }
  const float base_ring=canal_tower_radius(s,lattice_base)+.44f;
  formed_ring(scene,s,{{base_ring-.48f,lattice_base-.30f},{base_ring-.08f,lattice_base-.30f},
    {base_ring,lattice_base-.16f},{base_ring-.08f,lattice_base+.06f},{base_ring-.48f,lattice_base+.06f}},p.ring);

  // The open ring stack surrounds a real lower annular roof and raised pavilion.
  slab(scene.opaque,plan_circle(s.neck_radius+.25f,144,s.centre),s.crown_floor,.40f,p.deck);
  for(int level=0;level<4;++level) {
    const float y=s.crown_floor+level*1.80f,outer=s.crown_radius;
    formed_ring(scene,s,{{outer-.74f,y-.13f},{outer-.10f,y-.13f},{outer+.035f,y-.035f},
      {outer+.035f,y+.055f},{outer-.08f,y+.19f},{outer-.74f,y+.19f}},p.ring);
    formed_ring(scene,s,{{outer-.57f,y-.21f},{outer-.43f,y-.21f},{outer-.43f,y-.14f},{outer-.57f,y-.14f}},p.bronze);
  }
  for(int column=0;column<36;++column) {
    const float a=column*2*kPi/36;
    const Vec3 foot=at(s,a,s.crown_floor-.09f,s.crown_radius-.43f);
    Emit(&scene.opaque,p.ring).beam(foot,{foot.x,s.crown_top,foot.z},.075f,.10f,radial(a));
    Emit(&scene.opaque,p.bronze).beam(at(s,a,s.crown_floor-.18f,s.neck_radius-.45f),
      at(s,a,s.crown_floor-.18f,s.crown_radius-.40f),.16f,.25f);
  }
  for(float y:{s.crown_top-.62f,s.crown_top-.10f})
    formed_ring(scene,s,{{s.crown_radius-.49f,y-.035f},{s.crown_radius-.39f,y-.035f},
      {s.crown_radius-.39f,y+.035f},{s.crown_radius-.49f,y+.035f}},p.ring);

  const float pavilion=s.pavilion_radius-.38f,roof=s.pavilion_roof;
  Emit(&scene.opaque,p.neck).wall(plan_circle(pavilion,96,s.centre),s.crown_floor,roof-.36f,true);
  for(int column=0;column<24;++column) {
    const float a=column*2*kPi/24;
    Emit(&scene.opaque,p.bronze).beam(at(s,a,s.crown_floor,pavilion+.015f),
      at(s,a,roof-.28f,pavilion+.015f),.11f,.15f,radial(a));
  }
  // Twelve panels retain the shallow polygonal silhouette of the small roof.
  slab(scene.opaque,plan_circle(s.pavilion_radius-.18f,12,s.centre),roof-.05f,.31f,p.roof);
  formed_ring(scene,s,{{s.pavilion_radius-.28f,roof-.36f},{s.pavilion_radius+.04f,roof-.21f},
    {s.pavilion_radius,roof},{s.pavilion_radius-.28f,roof+.055f}},p.roof,12);
  // A low folded exhaust cowl sits on the roof instead of a painted roof mark.
  const Vec3 c=P3(s.centre,roof-.05f),u{1.6f,0,0},v{0,0,1.0f};
  Emit cowl(&scene.opaque,p.bronze);
  const Vec3 a=c-u-v,b=c+u-v,d=c-u+v,e=c+u+v,peak=c+Vec3{.65f,1.10f,.25f};
  cowl.triangle(a,peak,b);cowl.triangle(b,peak,e);cowl.triangle(e,peak,d);cowl.triangle(d,peak,a);
  cowl.quad_metric(a,b,e,d);
  for(int i=0;i<12;++i) {
    const float a=(i+.5f)*2*kPi/12;
    Emit(&scene.opaque,M_LOBBY_LIGHT).box(at(s,a,s.crown_floor+.035f,s.neck_radius-.55f),{.12f,.025f,.12f});
  }
}

Scene make_canal_tower_sample(std::string_view view) {
  Scene scene;scene.materials=make_materials();const CanalTowerSpec spec;
  build_canal_tower(scene,spec);
  if(!shot_camera("aerial",scene.camera_position,scene.camera_target))throw std::runtime_error("Arrival camera unavailable");
  if(view=="arrival")scene.camera_fov_degrees=shot_fov_degrees("aerial");
  else if(view=="crown"||view=="full") {
    scene.camera_target=P3(spec.centre,view=="crown"?94.f:55.f);
    scene.camera_fov_degrees=view=="crown"?9.f:21.f;
  } else throw std::runtime_error("Unknown canal tower sample view");
  scene.city_size="canal-tower-inspection";scene.city_radius=180;
  scene.stats_towers=1;scene.finalize_draws();return scene;
}
} // namespace cb
