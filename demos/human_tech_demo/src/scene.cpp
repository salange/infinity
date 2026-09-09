#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "catalog.hpp"
#include "materials.hpp"
#include "city/flora.hpp"
#include "city/props.hpp"
#include "city/standards.hpp"
#include "towers.hpp"

namespace cb {
namespace {
// Authored visual set, in the shared city library's metre / y-up frame.
// The fixed block addresses are composition, not sites/v1 or game-world data.
constexpr float kDeck = 1.2f;
constexpr float kPitch = 104.0f;

void palette(Scene& sc) {
  sc.materials = make_materials();
  auto& m = sc.materials;
  for (Mat id : {M_WHITE_METAL, M_CONCRETE_WHITE, M_WALL_LIGHT, M_PANEL_WARM}) {
    m[id].base_color = {0.98f, 0.99f, 0.98f};
    m[id].albedo_set = "concrete_white";
    m[id].roughness = 0.36f;
    m[id].normal_strength = 0.24f;
    m[id].uv_scale = 2.4f;
    m[id].flags = kMatTriplanar;
  }
  m[M_BRONZE].base_color = {0.55f, 0.32f, 0.14f};
  m[M_BRONZE].roughness = 0.3f;
  m[M_BRONZE].albedo_set = "metal_silver";
  m[M_BRONZE].normal_strength = 0.25f;
  m[M_BRONZE].flags = kMatTriplanar;
  for (Mat id : {M_PLAZA, M_SIDEWALK, M_TERRAZZO, M_MARBLE_WHITE}) {
    m[id].base_color = {0.68f, 0.72f, 0.73f};
    m[id].roughness = 0.32f;
    m[id].normal_strength = 0.22f;
    m[id].uv_scale = 4.0f;
  }
  for (auto& mat : m) if ((mat.flags & kMatGlass) != 0) {
    mat.tint2 = {0.70f, 0.77f, 0.79f};
    mat.roughness = 0.09f;
    mat.lit_probability = 0.76f;
  }
  m[M_GLASS_CLEAR].tint2 = {1.0f, 0.84f, 0.62f};
  m[M_GLASS_STD].tint2 = {0.96f, 0.82f, 0.64f};
  m[M_GLASS_CLEAR].lit_probability = 0.96f;
  m[M_HEDGE].base_color = {0.39f, 0.59f, 0.36f};
  m[M_WATER].base_color = {0.025f, 0.14f, 0.16f};
  m[M_WATER].roughness = 0.13f;
  m[M_WATER].metallic = 0.4f;
  m[M_SIGN].base_color = {0.8f, 0.43f, 0.12f};
  m[M_SIGN].tint2 = {1.0f, 0.61f, 0.24f};
  m[M_SIGN].flags = kMatEmissive;
  m[M_SIGN].emissive = 0.65f;
  m[M_LOBBY_LIGHT].emissive = 0.8f;
}

void box(Scene& sc, Mat mat, Vec3 c, Vec3 half) { Emit(&sc.opaque, mat).box(c, half); }

// Thin bronze handrails retain the view and have explicit posts into the deck.
void rail(Scene& sc, Vec3 a, Vec3 b) {
  Emit e(&sc.opaque, M_BRONZE);
  e.tube(a + Vec3{0, 1.08f, 0}, b + Vec3{0, 1.08f, 0}, 0.045f, 6);
  e.tube(a + Vec3{0, 0.45f, 0}, b + Vec3{0, 0.45f, 0}, 0.028f, 5);
  const int n = std::max(1, static_cast<int>(length(b-a) / 2.4f));
  for (int i = 0; i <= n; ++i) {
    Vec3 p = lerp(a, b, static_cast<float>(i) / n);
    e.tube(p, p + Vec3{0, 1.08f, 0}, 0.04f, 6);
  }
}

void plant(Scene& sc, Vec3 base, float size, float phase) {
  Emit leaf(&sc.opaque, M_HEDGE);
  for (int i=0; i<11; ++i) {
    const float angle=phase+i*2.399963f;
    Vec3 dir{std::cos(angle),0,std::sin(angle)}, side{-dir.z,0,dir.x};
    const float h=size*(0.65f+0.035f*i);
    for(int j=0;j<6;++j) {
      float t=j/6.0f, u=(j+1)/6.0f;
      auto centre=[&](float q) {return base+dir*(h*q*0.7f)+Vec3{0,h*(1.5f*q-q*q),0};};
      float wa=std::sin(kPi*t)*h*0.14f, wb=std::sin(kPi*u)*h*0.14f;
      Vec3 a=centre(t),b=centre(u);
      leaf.quad_metric(a-side*wa,b-side*wb,b+side*wb,a+side*wa);
      leaf.quad_metric(a+side*wa,b+side*wb,b-side*wb,a-side*wa);
    }
  }
}

void garden(Scene& sc, Rng rng, Vec2 c, float hx, float hz, float y, bool tree) {
  box(sc, M_CONCRETE_WHITE, P3(c, y+0.38f), {hx, 0.38f, hz});
  box(sc, M_SOIL, P3(c, y+0.77f), {hx-0.2f, 0.015f, hz-0.2f});
  if (hx>8 || hz>8) {
    build_hedge(sc, c-Vec2{hx-0.4f, 0}, c+Vec2{hx-0.4f, 0}, hz*1.65f, 0.4f, y+0.8f);
  }
  const int count=std::min(22,std::max(3,static_cast<int>(std::max(hx,hz)*1.4f)));
  for(int i=0;i<count;++i) {
    Rng r=rng.child(40+i);
    plant(sc,P3(c+Vec2{r.range(-hx+0.3f,hx-0.3f),r.range(-hz+0.3f,hz-0.3f)},y+0.78f),r.range(0.7f,1.4f),r.range(0,6.28f));
  }
  if (tree) gen_tree(sc, rng.child(1), P3(c, y+0.8f), 6.5f);
}

// Open arcade bays show physical columns, seating, back walls and ceiling
// lanterns; the upper floors use the inherited parallax interior material.
void arcade(Scene& sc, Vec2 c, float hx, float hz, float y, int floors, Rng rng, bool detail) {
  auto footprint = plan_rounded_rect(hx, hz, 3.0f, 5, c);
  StandardSpec s;
  s.type = StdType::Mixed; s.storeys = floors; s.retail_ground = true;
  s.wall = M_CONCRETE_WHITE; s.glass = M_GLASS_STD;
  s.roof = RoofKind::Green; s.entrance = EntranceKind::Vestibule;
  build_standard(sc, s, footprint, y+4.4f, rng.child(1), detail ? 2 : 0);
  slab(sc.opaque, plan_offset(footprint, 1.4f), y+4.4f, 0.45f, M_WHITE_METAL);
  box(sc, M_PANEL_WARM, P3(c, y+2), {hx-1, 2, hz-3.2f});
  for (float zsign : {-1.0f, 1.0f}) {
    const float z = c.y + zsign*(hz-0.5f);
    for (float x = c.x-hx+2; x < c.x+hx; x += 5.0f) {
      box(sc, M_BRONZE, {x,y+2.1f,z}, {0.1f,2.1f,0.1f});
      if (detail) {
        box(sc, M_LOBBY_LIGHT, {x+1.5f,y+4.13f,z-zsign}, {0.8f,0.06f,0.22f});
        gen_bench(sc, {x+1.5f,y,z-zsign*1.2f}, 0);
        box(sc, M_GLASS_CLEAR, {x+1.5f,y+2.0f,z-zsign*2.0f}, {1.7f,1.7f,0.05f});
      }
    }
  }
}

void tower(Scene& sc, TowerSpec s, Vec2 p, float y, Rng rng, bool hero) {
  s.base = BaseKind::Lobby; s.base_scale = 1.1f;
  const int group = sc.lod_groups++;
  const float height = s.floors*s.floor_h;
  for (int lod = 0; lod < 4; ++lod) {
    auto first = static_cast<std::uint32_t>(sc.opaque.indices.size());
    build_tower(sc, s, p, y, rng, hero ? 2-lod : std::min(1,2-lod));
    sc.register_range(first, static_cast<std::uint32_t>(sc.opaque.indices.size()),
                      P3(p,y+height*0.5f), std::sqrt(height*height*0.25f+s.a*s.a+s.b*s.b)+12,
                      group,lod,lod==0 ? 290.0f : lod==1 ? 700.0f : lod==2 ? 1600.0f : 1e30f);
  }
  ++sc.stats_towers;
}

TowerSpec family(int i, int floors, float r) {
  switch (i % 6) {
    case 0: return spec_diagrid(r, floors);
    case 1: return spec_hex(r, floors);
    case 2: return spec_lens(r*1.2f,r*0.72f,floors,0.25f);
    case 3: return spec_finweave(r,floors);
    case 4: return spec_lens(r*1.2f,r*0.8f,floors,-0.6f);
    default: return spec_xframe(r,r*0.8f,floors);
  }
}

void district(Scene& sc, int ix, int iz, Rng rng) {
  Vec2 c{ix*kPitch, iz*kPitch};
  if (ix <= -2) c.x -= 40;
  const float dist = length(c);
  const bool near = dist < 460;
  auto first = static_cast<std::uint32_t>(sc.opaque.indices.size());
  const auto plot = plan_rounded_rect(46,46,5,6,c);
  slab(sc.opaque, plot, kDeck, 1.4f, M_PLAZA);
  // Three occupied terraces make the block a continuous urban base.
  const int terraces = near ? 3 : 2;
  for (int f = 0; f < terraces; ++f) {
    float half = 43.0f - f*3.0f;
    auto level = plan_rounded_rect(half,half,6,8,c);
    float y = kDeck+4.2f*(f+1);
    Emit glazing(&sc.opaque, M_GLASS_STD);
    glazing.element_random = rng.child(10+f).next();
    glazing.wall(plan_offset(level,-1.8f), y-4.0f,y-0.3f,true);
    slab(sc.opaque,level,y,0.45f,M_WHITE_METAL);
    if (near) {
      for (float sign : {-1.0f,1.0f}) {
        garden(sc,rng.child(20+f),c+Vec2{sign*(half-3),0},1.3f,half-7,y,false);
        garden(sc,rng.child(30+f),c+Vec2{0,sign*(half-3)},half-7,1.3f,y,false);
      }
    }
  }
  const float roof = kDeck+4.2f*terraces;
  if (dist < 850) for (int i=0;i<5;++i) {
    const float sign=(i%2==0)?1.0f:-1.0f;
    Vec2 q=c+Vec2{-28+i*14.0f,sign*31};
    garden(sc,rng.child(210+i),q,2.0f,2.6f,roof,true);
  }
  const int address = (ix+16)*37+(iz+16)*13;
  bool tall = (address%3!=0 && dist < 630) || (address%7==0);
  // Foreground lattice garden and its attached ribbon neighbor.
  if (ix == 1 && iz == 1) tall = true;
  if (!tall) {
    for (int j = 0; j < 2; ++j) {
      arcade(sc,c+Vec2{j==0 ? -20.0f : 20.0f,0},16,29,roof,
             3+address%5,rng.child(70+j),near);
      ++sc.stats_standards;
    }
  }
  if (near) {
    for (float sign : {-1.0f,1.0f}) {
      garden(sc,rng.child(90),c+Vec2{sign*31,28},3,4,roof,true);
      gen_bench(sc,P3(c+Vec2{sign*22,33},roof),0);
      gen_lamp(sc,P3(c+Vec2{sign*44,44},kDeck),0);
    }
    for (float sign : {-1.0f,1.0f}) rail(sc,P3(c+Vec2{-35,sign*36},roof),P3(c+Vec2{35,sign*36},roof));
  }
  sc.register_range(first,static_cast<std::uint32_t>(sc.opaque.indices.size()),P3(c,25),90);
  if (tall) {
    int floors = 25+address%35;
    if (dist > 650) floors = 14+address%28;
    TowerSpec spec = family(address, floors, 19+address%6);
    if (ix==1 && iz==1) spec = spec_hex(25,48);
    tower(sc,spec,c,roof,rng.child(120),near);
  }
  ++sc.stats_blocks;
}

void bridge(Scene& sc, Vec3 a, Vec3 b, float width) {
  Emit deck(&sc.opaque,M_WHITE_METAL);
  deck.beam(a,b,width,0.65f);
  Vec3 side = normalize(cross(b-a,Vec3{0,1,0}))*(width*0.5f-0.2f);
  rail(sc,a+side+Vec3{0,0.33f,0},b+side+Vec3{0,0.33f,0});
  rail(sc,a-side+Vec3{0,0.33f,0},b-side+Vec3{0,0.33f,0});
  Emit(&sc.opaque,M_BRONZE).beam(a-Vec3{0,0.5f,0},b-Vec3{0,0.5f,0},width*0.6f,0.4f);
}

void civic(Scene& sc, Rng rng) {
  // A canal promenade meets the forecourt through shallow stairs and bridges.
  slab(sc.opaque,plan_rounded_rect(90,149,12,12,{0,0}),kDeck,1.4f,M_PLAZA);
  for (int step = 0; step < 6; ++step)
    box(sc,M_MARBLE_WHITE,{24,kDeck+step*0.15f,98-step*0.9f},{48,0.075f,0.5f});
  build_basin(sc,{0,58},23,34,false,kDeck,0.35f);
  build_unification_ring(sc,{-20,8},kDeck,30,radians(78),2);
  build_government(sc,{24,-70},0,46,kDeck,rng,2,5);
  for (float sign : {-1.0f,1.0f}) {
    arcade(sc,{sign*71,29},12,36,kDeck,2,rng.child(sign<0?1:2),true);
    for (int i=0;i<5;++i) {
      garden(sc,rng.child(30+i),{sign*41,110-i*26.0f},3,5,kDeck,true);
      gen_bench(sc,{sign*34,kDeck,110-i*26.0f},sign*kPi*0.5f);
    }
  }
  // Human-scale lanterns, drainage and bronze inlays along the basin axis.
  for (int z=24;z<135;z+=8) for (float sign : {-1.0f,1.0f}) {
    box(sc,M_BRONZE,{sign*28,kDeck+0.5f,static_cast<float>(z)},{0.12f,0.5f,0.12f});
    box(sc,M_LOBBY_LIGHT,{sign*28,kDeck+0.85f,static_cast<float>(z)},{0.13f,0.16f,0.13f});
  }
  box(sc,M_BRONZE,{31,kDeck+0.015f,61},{0.04f,0.015f,76});
  box(sc,M_DARK_METAL,{-31,kDeck+0.015f,61},{0.12f,0.015f,76});
  sc.stats_plazas = 1;
}

void terrace(Scene& sc, Rng rng) {
  // At the south face of the foreground hex tower, floor-aligned to its
  // podium. Open garden terrace and a skywalk landing into the next block.
  constexpr float y = kDeck+12.6f;
  auto p = plan_rounded_rect(35,15,5,10,{104,143});
  slab(sc.opaque,p,y,0.6f,M_TERRAZZO);
  rail(sc,{70,y,156},{137,y,156});
  rail(sc,{138,y,131},{138,y,155});
  for (int i=0;i<5;++i) {
    garden(sc,rng.child(i),{77.0f+i*12,150},3,2,y,i%2==0);
    gen_bench(sc,{78.0f+i*12,y,146},0);
  }
  // Structural arch at the garden edge: cladding joints and bronze node caps.
  Emit ceramic(&sc.opaque,M_WHITE_METAL), bronze(&sc.opaque,M_BRONZE);
  for (float x : {73.0f,135.0f}) {
    Vec3 foot{x,y,132}, knee{x+(x<100?7.0f:-7.0f),y+10,130}, top{x,y+22,128};
    ceramic.beam(foot,knee,0.9f,1.0f); ceramic.beam(knee,top,0.9f,1.0f);
    bronze.sphere(knee,0.65f,8,12);
    for(int j=1;j<5;++j) bronze.beam(lerp(foot,knee,j/5.0f)-Vec3{0.45f,0,0},lerp(foot,knee,j/5.0f)+Vec3{0.45f,0,0},0.025f,0.025f);
  }
  bridge(sc,{144,y-0.33f,135},{169,y-0.33f,135},6);
  slab(sc.opaque,plan_rect(6,9,{170,135}),y,0.6f,M_TERRAZZO);
  // Stair connects the landing to the neighboring roof (same building levels).
  for(int i=0;i<28;++i) box(sc,M_WHITE_METAL,{175+i*0.35f,y-i*0.15f,135},{0.18f,0.15f,3});
  rail(sc,{175,y,138},{185,y-4.2f,138});
}
}  // namespace

Scene generate_scene(const SceneParams& params) {
  Scene sc;
  palette(sc);
  Rng root = root_rng(params.seed);
  set_far_patterns(params.far_patterns);
  if (!params.asset.empty()) {
    std::string error;
    if (!generate_asset(sc,params.asset,root.child(300),params.detail,&error)) {
      std::fprintf(stderr,"asset: %s\n",error.c_str()); std::exit(2);
    }
    return sc;
  }
  // Water continues beyond the city, with a deliberately finite urban set.
  box(sc,M_WATER,{0,-1,0},{9000,0.2f,9000});
  box(sc,M_ASPHALT,{475,-0.35f,0},{1430,0.3f,1070});
  // West-side canal: no ground plate underneath the water surface.
  box(sc,M_WATER,{-176,0.1f,0},{22,0.1f,1080});
  for(float x : {-204.0f,-148.0f}) {
    box(sc,M_PLAZA,{x,0.7f,0},{6,0.5f,1060});
    for(int z=-950;z<=950;z+=26) {
      if (std::abs(z)<260) garden(sc,root.child(410+z+950),{x,static_cast<float>(z)},2,3,kDeck,true);
    }
  }
  for(int iz=-10;iz<=10;++iz) for(int ix=-9;ix<=18;++ix) {
    if (ix==0 && iz>=-1 && iz<=1) continue;
    // Preserve a civic square spanning the three central blocks.
    district(sc,ix,iz,root.child(1000+(ix+9)*32+iz+10));
  }
  for(int z=-8;z<=8;z+=2) bridge(sc,{-220,kDeck-0.33f,z*kPitch+52},{-132,kDeck-0.33f,z*kPitch+52},9);
  civic(sc,root.child(500));
  terrace(sc,root.child(600));
  // Sheltered storefront facing the pedestrian route. Bays remain open so
  // the camera sees furniture, shelving and warm ceiling light in real depth.
  slab(sc.opaque,plan_rounded_rect(9,23,3,8,{64,117}),6.4f,0.5f,M_WHITE_METAL);
  box(sc,M_PANEL_WARM,{70,3.8f,117},{0.3f,2.6f,23});
  for (int bay=0;bay<6;++bay) {
    const float z=97+bay*8.0f;
    box(sc,M_BRONZE,{55.5f,3.7f,z},{0.12f,2.5f,0.12f});
    box(sc,M_LOBBY_LIGHT,{62,6.05f,z+2},{3.5f,0.05f,0.35f});
    for(int shelf=0;shelf<4;++shelf) {
      box(sc,M_BRONZE,{69,2.0f+shelf*0.8f,z+2},{0.7f,0.04f,2.8f});
      for(int item=0;item<5;++item) box(sc,M_PANEL_WARM,{68.9f,2.22f+shelf*0.8f,z+item-0.1f},{0.18f,0.18f,0.28f});
    }
    gen_bench(sc,{60,kDeck,z+3},kPi*0.5f);
    garden(sc,root.child(650+bay),{54,z},0.8f,1.1f,kDeck,false);
  }
  for(int slat=0;slat<52;++slat) box(sc,M_BRONZE,{62,5.92f,94+slat*0.9f},{7,0.06f,0.045f});
  // Elevated promenade between the two eastern podiums, at a real floor.
  bridge(sc,{140,13.47f,80},{171,13.47f,80},7);
  sc.finalize_draws();
  sc.city_size="human-tech visual set"; sc.city_radius=2000;
  if(sc.lights.size()>64) sc.lights.resize(64);
  shot_camera(params.shot,sc.camera_position,sc.camera_target);
  return sc;
}
bool shot_camera(const std::string& shot, Vec3& position, Vec3& target) {
  if(shot=="civic") { position={-26,3.0f,111}; target={4,34,-45}; }
  else if(shot=="street") { position={49,3.0f,140}; target={25,15,28}; }
  else if(shot=="terrace") { position={84,16.0f,145}; target={-35,34,-55}; }
  else if(shot=="aerial") { position={-175,300,310}; target={75,32,-80}; }
  else return false;
  return true;
}
}  // namespace cb
