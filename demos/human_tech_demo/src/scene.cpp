#include "scene.hpp"
#include "city_routes.hpp"
#include "city_transit.hpp"
#include "riverfront_layout.hpp"
#include "civic_landmarks.hpp"
#include "civic_forecourt.hpp"
#include "canal_construction.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <set>
#include <utility>
#include <vector>

#include "catalog.hpp"
#include "materials.hpp"
#include "garden_staging.hpp"
#include "garden_frame.hpp"
#include "garden_guard.hpp"
#include "garden_facade_sector.hpp"
#include "sculpted_diagrid.hpp"
#include "market_staging.hpp"
#include "market_structure.hpp"
#include "market_canopy.hpp"
#include "market_climbers.hpp"
#include "landing_lounge.hpp"
#include "landing_canopies.hpp"
#include "roof_staging.hpp"
#include "street_forecourt.hpp"
#include "transit_concourse.hpp"
#include "city/flora.hpp"
#include "city/props.hpp"
#include "city/standards.hpp"
#include "towers.hpp"

namespace cb {
namespace {
// A single authored architectural benchmark, in metres and y-up. The lot
// addresses remain stable when new detail layers are added to this visual set.
constexpr float kDeck = 1.2f;
thread_local bool arrival_blockout = false;
// Orthogonal survey axes: cross streets point east-southeast; the broad
// boulevard follows the perpendicular northeast-southwest street family.
constexpr float kGridCos=.963518f,kGridSin=.267645f;
constexpr float kGridLateralOrigin=-540.f;
constexpr float kNearestBridgeRow=72.f;
const std::array<float,17> kOffsets{28,94,218,300,485,578,730,805,980,1070,1254,1336,1442,1638,1718,1900,2040};
const Vec2 kMarketMove{-190,-20};
const Vec2 kDomeMove{40,-92};
const Vec2 kHexMove{28.37f,-14.90f};
std::vector<Vec2> relocated_market_approach();
std::vector<std::vector<Vec2>> market_approach_footprints();
std::vector<Vec2> survey_rect(float s0,float s1,float t0,float t1) {
  auto point=[](float x,float z){return Vec2{kGridCos*x+kGridSin*z,-kGridSin*x+kGridCos*z};};
  return {point(s0,t0),point(s1,t0),point(s1,t1),point(s0,t1)};
}
std::vector<Vec2> moved_plan(std::vector<Vec2> p,Vec2 d) {for(auto& v:p)v=v+d;return p;}
struct SceneTail {
  std::size_t opaque,foliage,instances,lights,draws,fine,roofs,obstructions;
  explicit SceneTail(const Scene& s):opaque(s.opaque.vertices.size()),foliage(s.foliage.vertices.size()),
    instances(s.asset_instances.size()),lights(s.lights.size()),draws(s.draws.size()),fine(s.fine.size()),
    roofs(s.authored_roofs.size()),obstructions(s.roof_obstructions.size()){}
  void move(Scene& s,Vec2 d) const {
    const Vec3 shift{d.x,0,d.y};
    auto vertices=[&](auto& vs,std::size_t start){for(std::size_t i=start;i<vs.size();++i)vs[i].position=vs[i].position+shift;};
    vertices(s.opaque.vertices,opaque);vertices(s.foliage.vertices,foliage);
    for(std::size_t i=instances;i<s.asset_instances.size();++i)s.asset_instances[i].translation=s.asset_instances[i].translation+shift;
    for(std::size_t i=lights;i<s.lights.size();++i)s.lights[i].position=s.lights[i].position+shift;
    for(std::size_t i=draws;i<s.draws.size();++i)s.draws[i].centre=s.draws[i].centre+shift;
    for(std::size_t i=fine;i<s.fine.size();++i)s.fine[i].centre=s.fine[i].centre+shift;
    for(std::size_t i=roofs;i<s.authored_roofs.size();++i)s.authored_roofs[i].polygon=moved_plan(std::move(s.authored_roofs[i].polygon),d);
    for(std::size_t i=obstructions;i<s.roof_obstructions.size();++i)s.roof_obstructions[i].polygon=moved_plan(std::move(s.roof_obstructions[i].polygon),d);
  }
};
const Vec2 kRing{171.883f, -166.568f};
constexpr float kRingRadius=51.064f;
const Vec2 kDome{-36, -28};
const Vec2 kLattice{-345,405};
constexpr float kGardenY = 277.2f;
const std::array<Vec2,6> kGardenApproach{{{44,194},riverfront::point(216,130),
    riverfront::point(216,202),riverfront::point(27,202),
    riverfront::point(27,300),riverfront::point(27,360)}};
const Vec2 kGardenBridgeGarden{-301.8f,405};
const Vec2 kGardenBridgeNeighbor{-153,481};
const Vec2 kGardenBridgeJunction{-177,405+94.f/118.f*76.f};
constexpr float kLandingEntryX=203.5f;
const std::array<Vec2,12> kWestPublicRoute{{{-187.3f,-99},{-127,-99},{-99,-90},{-99,25},{-110,48},
    {-113,60},riverfront::point(146,11.5f),riverfront::point(176.5f,11.5f),
    riverfront::point(176.5f,108),riverfront::point(176.5f,130),
    riverfront::point(216,130),{44,194}}};
const std::vector<Vec2> kWestCivicCourt{{-207,-130},{-125,-140},{-96,-92},{-124,-53},{-207,-72}};
const std::vector<Vec2> kWestMarketReservation{{-172,55},{-117,55},{-117,163},{-172,163}};
const std::vector<Vec2> kMarketPromenade{{-121,163},{-84,163},{-35,84},{-111,84}};
const Vec2 kGardenCompanion{-386,300};
const std::vector<Vec2> kGardenCompanionParcel{{-402.4f,253.1f},{-358.1f,253.1f},{-366.7f,327},{-410.6f,327}};
const Vec3 kWestGardenCamera{-400,279,388};
const Vec3 kWestGardenTarget{-133.918f,243.616f,-36.094f};
const std::array<Vec2,5> kWestGardenWalk{{{-388,405},{-400,402},{-404.3f,398},{-404.3f,393},{-400,388}}};
const Vec2 kUpperBridgeOriginal{-350,362.2f};
const Vec2 kUpperBridgeCompanion{-383,317.7f};
const Vec2 kUpperStair{-350,356.4f};
const Vec2 kCanalHex{-179.8f,248.1f};
const std::vector<Vec2> kCanalHexParcel=moved_plan(riverfront::rectangle(
    riverfront::lot_west,riverfront::lot_east,riverfront::lot_north,riverfront::lot_south),kHexMove*-1.f);
const std::vector<Vec2> kCanalHexAccess=moved_plan(riverfront::rectangle(146,178,104,112),kHexMove*-1.f);
const std::array<std::pair<int,int>,40> kRearMiddleFloors{{
    {569,14}, // coastal_ribbon/3/14
    {606,19}, // coastal_ribbon/3/15
    {791,22}, // coastal_ribbon/3/20
    {697,21}, // coastal_ribbon/4/17
    {882,13}, // coastal_ribbon/4/22
    {677,19}, // coastal_ribbon/5/16
    {714,13}, // coastal_ribbon/5/17
    {751,18}, // coastal_ribbon/5/18
    {546,13}, // coastal_ribbon/6/12
    {657,17}, // coastal_ribbon/6/15
    {694,22}, // coastal_ribbon/6/16
    {731,16}, // coastal_ribbon/6/17
    {805,15}, // coastal_ribbon/6/19
    {637,15}, // coastal_ribbon/7/14
    {711,14}, // coastal_ribbon/7/16
    {785,13}, // coastal_ribbon/7/18
    {822,18}, // coastal_ribbon/7/19
    {859,12}, // coastal_ribbon/7/20
    {896,17}, // coastal_ribbon/7/21
    {933,22}, // coastal_ribbon/7/22
    {617,13}, // coastal_ribbon/8/13
    {654,18}, // coastal_ribbon/8/14
    {802,16}, // coastal_ribbon/8/18
    {839,21}, // coastal_ribbon/8/19
    {913,20}, // coastal_ribbon/8/21
    {950,14}, // coastal_ribbon/8/22
    {987,19}, // coastal_ribbon/8/23
    {634,16}, // coastal_ribbon/9/13
    {671,21}, // coastal_ribbon/9/14
    {819,19}, // coastal_ribbon/9/18
    {893,18}, // coastal_ribbon/9/20
    {651,19}, // coastal_ribbon/10/13
    {725,18}, // coastal_ribbon/10/15
    {762,12}, // coastal_ribbon/10/16
    {799,17}, // coastal_ribbon/10/17
    {873,16}, // coastal_ribbon/10/19
    {742,21}, // coastal_ribbon/11/15
    {779,15}, // coastal_ribbon/11/16
    {853,14}, // coastal_ribbon/11/18
    {890,19}, // coastal_ribbon/11/19
}};
int rear_middle_floors(int address) {
  for(const auto& [id,floors]:kRearMiddleFloors)if(id==address)return floors;
  return -1;
}

std::vector<CanalCrossing> canal_construction_crossings(Rng root);
std::vector<std::vector<Vec2>> canal_construction_exclusions(Rng root);
float shore(float z);
float east_shore(float z);
float canal_centre(float z);
float canal_halfwidth(float z);
std::vector<float> coastal_rows(Rng root);
Mat lattice_ceramic(Scene& sc);
void showcase_lattice_tower(Scene& sc,TowerSpec spec,Vec2 centre,float base,Rng rng);
std::vector<std::vector<Vec2>> subtract_convex(const std::vector<Vec2>& source,const std::vector<Vec2>& obstacle,float minimum_area);
std::vector<Vec2> inset_convex_plan(const std::vector<Vec2>& source,float distance);

void palette(Scene& sc) {
  sc.materials = make_materials();
  auto& m = sc.materials;
  for (Mat id : {M_WHITE_METAL, M_CONCRETE_WHITE, M_WALL_LIGHT}) {
    m[id].base_color={0.82f,0.80f,0.72f}; m[id].roughness=0.34f;
    m[id].albedo_set="concrete_white"; m[id].normal_strength=0.16f;
    m[id].uv_scale=1.8f; m[id].flags=kMatTriplanar; m[id].metallic=0;
  }
  m[M_PANEL_WARM].base_color={0.32f,0.25f,0.16f};
  m[M_PANEL_WARM].roughness=.65f;
  m[M_BRONZE].base_color={.43f,.235f,.095f}; m[M_BRONZE].roughness=.27f;
  m[M_BRONZE].metallic=.86f; m[M_BRONZE].normal_strength=.2f;
  m[M_CHROME].base_color={.73f,.69f,.57f};m[M_CHROME].roughness=.24f;
  for(Mat id:{M_PLAZA,M_SIDEWALK,M_TERRAZZO,M_MARBLE_WHITE}) {
    m[id].base_color={.86f,.88f,.84f};m[id].roughness=.37f;m[id].normal_strength=.28f;m[id].uv_scale=2.4f;
  }
  for(auto& mat:m) if(mat.flags&kMatGlass) {
    // Coated-window contract: base_color is one-pass pane transmission;
    // metallic is coating coverage and tint2 colors the room independently.
    mat.base_color={.74f,.83f,.86f};mat.tint2={.94f,.90f,.81f};
    mat.metallic=.22f;mat.roughness=.10f;mat.lit_probability=.29f;
  }
  m[M_GLASS_BRONZE].base_color={.78f,.58f,.36f};
  m[M_GLASS_BRONZE].metallic=.60f;m[M_GLASS_BRONZE].roughness=.075f;
  m[M_GLASS_BLUE].base_color={.62f,.76f,.82f};
  m[M_GLASS_BLUE].metallic=.35f;m[M_GLASS_BLUE].roughness=.09f;
  m[M_GLASS_DARK].base_color={.50f,.60f,.66f};
  m[M_GLASS_DARK].metallic=.31f;m[M_GLASS_DARK].roughness=.10f;
  m[M_GLASS_GREEN].base_color={.63f,.79f,.66f};m[M_GLASS_GREEN].metallic=.22f;
  m[M_GLASS_SILVER].base_color={.87f,.90f,.92f};m[M_GLASS_SILVER].metallic=.46f;
  m[M_GLASS_CLEAR].base_color={.94f,.97f,.98f};
  m[M_GLASS_CLEAR].metallic=.02f;m[M_GLASS_CLEAR].roughness=.045f;
  m[M_GLASS_CLEAR].lit_probability=.7f;
  m[M_GLASS_STD].lit_probability=.43f;
  m[M_HEDGE].base_color={.64f,.78f,.57f};m[M_HEDGE].roughness=.86f;
  m[M_LEAF].base_color={.72f,.84f,.68f};
  m[M_GRASS].base_color={.78f,.85f,.65f};
  m[M_SOIL].base_color={.85f,.82f,.77f};m[M_SOIL].roughness=.95f;
  m[M_WATER].base_color={.028f,.065f,.073f};m[M_WATER].roughness=.12f;
  m[M_WATER].metallic=0;m[M_WATER].flags=kMatPlanarXZ|64u;
  m[M_SIGN].base_color={.8f,.43f,.12f};m[M_SIGN].tint2={1,.61f,.24f};
  m[M_SIGN].flags=kMatEmissive|kMatNightOnly;m[M_SIGN].emissive=.65f;
  m[M_LOBBY_LIGHT].emissive=1.2f;
  m[M_ASPHALT].base_color={.86f,.89f,.86f};
  // Wetness belongs to exposed ground paving. The same polished stone family
  // also supplies dry upper roof caps and sheltered terraces, so IDs alone
  // cannot infer whether a material should collect the street's rain film.
  for(Mat id:{M_ASPHALT,M_PLAZA,M_SIDEWALK})m[id].flags|=512u;
  if(sc.reviewed_material_maps) {
    // The verified ARM maps modulate roughness by (.45+.55*map/.6).
    // These factors give dry mean roughness .28 marble, .45 square stone,
    // .53 sidewalk stone and .44 terrazzo before any exposed rain film.
    for(Mat id:{M_MARBLE,M_MARBLE_WHITE}) {
      m[id].base_color={.98f,.97f,.89f};m[id].roughness=.55f;m[id].normal_strength=.22f;
    }
    for(Mat id:{M_PLAZA,M_SIDEWALK}) {
      m[id].base_color={.87f,.98f,1.f};m[id].normal_strength=.48f;
    }
    m[M_PLAZA].roughness=.50f;m[M_SIDEWALK].roughness=.58f;
    m[M_TERRAZZO].base_color={.80f,.79f,.80f};m[M_TERRAZZO].roughness=.45f;
  }
}
void box(Scene& sc, Mat m, Vec3 c, Vec3 h) { Emit(&sc.opaque,m).box(c,h); }
void rail(Scene& sc, Vec3 a, Vec3 b, bool glazing=false) {
  Emit e(&sc.opaque,M_BRONZE);
  e.tube(a+Vec3{0,1.08f,0},b+Vec3{0,1.08f,0},.034f,6);
  e.tube(a+Vec3{0,.12f,0},b+Vec3{0,.12f,0},.025f,5);
  int n=std::max(1,int(length(b-a)/2.8f));
  for(int i=0;i<=n;++i) {Vec3 p=lerp(a,b,float(i)/n);e.tube(p,p+Vec3{0,1.08f,0},.032f,5);}
  if(glazing) {
    // A balustrade is a real sheet of laminated glass. It must not use the
    // occupied-window shader, which would turn a view rail into an opaque wall.
    Mat glass_id=M_GLASS_CLEAR;
    for(std::size_t i=0;i<sc.materials.size();++i)
      if(sc.materials[i].name=="laminated balcony glazing")glass_id=static_cast<Mat>(i);
    if(glass_id==M_GLASS_CLEAR) {
      MaterialDesc material=sc.materials[M_GLASS_CLEAR];
      material.name="laminated balcony glazing";material.flags=128u;
      material.base_color={.93f,.97f,.95f};material.roughness=.07f;
      material.albedo_set="";material.emissive=0;material.metallic=0;
      material.room_w=1.5f;material.room_h=.98f;material.room_d=.96f;material.lit_probability=.012f;
      glass_id=static_cast<Mat>(sc.materials.size());sc.materials.push_back(material);
    }
    Vec3 side=normalize(cross(b-a,Vec3{0,1,0}))*.018f;
    Emit glass(&sc.opaque,glass_id);
    glass.quad_metric(a+Vec3{0,.18f,0}+side,b+Vec3{0,.18f,0}+side,b+Vec3{0,1.02f,0}+side,a+Vec3{0,1.02f,0}+side);
  }
}
void lamp(Scene& sc, Vec3 p, float height=4.1f) {
  Emit e(&sc.opaque,M_BRONZE);e.tube(p,p+Vec3{0,height,0},.055f,7);
  box(sc,M_LOBBY_LIGHT,p+Vec3{0,height-.18f,0},{.12f,.2f,.12f});
  sc.lights.push_back({p+Vec3{0,height-.2f,0},14,{1,.67f,.35f},3.8f});
}
// Each lamina has a curved centre vein and asymmetrical lobes. Foreground
// vegetation is subsequently supplied by the editable mesh resource library.
void leaf(Scene& sc,Vec3 p,Vec3 d,Vec3 side,float len,float width,Mat mat) {
  Emit e(&sc.opaque,mat);
  for(int s=0;s<4;++s) {
    float t=float(s)/4,u=float(s+1)/4;
    auto pt=[&](float v){return p+d*(len*v)+Vec3{0,.23f*len*std::sin(kPi*v),0};};
    Vec3 a=pt(t),b=pt(u);float wa=std::sin(kPi*t)*width,wb=std::sin(kPi*u)*width;
    e.quad_metric(a-side*wa,b-side*wb,b+side*wb,a+side*wa);
    e.quad_metric(a+side*wa,b+side*wb,b-side*wb,a-side*wa);
  }
}
bool close_botanical(Vec3 p) {
  // Every canonical camera sees the same botanical geometry. Full laminae
  // remain intact throughout the foreground of all six authored views.
  const std::array<Vec3,6> cameras{{{-285,245,510},{-235.2f,420.2f,-860.1f},
      {-187.3f,3,-99},{-119,3,149},kWestGardenCamera,{200,36,166}}};
  for(Vec3 camera:cameras)if(length(p-camera)<600)return true;
  return false;
}
std::string botanical_resource(const Scene& sc,const char* name,Vec3 p) {
  if(close_botanical(p))return name;
  std::string medium=std::string(name)+"_mid";
  for(const auto& r:sc.asset_library.resources)if(r.name==medium)return medium;
  return name;
}
bool polygons_overlap(const std::vector<Vec2>& a,const std::vector<Vec2>& b) {
  if(a.size()<3||b.size()<3)return false;
  auto separated=[&](const std::vector<Vec2>& edges) {
    for(std::size_t i=0;i<edges.size();++i) {
      Vec2 d=edges[(i+1)%edges.size()]-edges[i],axis{-d.y,d.x};
      float alo,ahi,blo,bhi;plan_extent(a,axis,&alo,&ahi);plan_extent(b,axis,&blo,&bhi);
      if(ahi<blo||bhi<alo)return true;
    }
    return false;
  };
  return !separated(a)&&!separated(b);
}
bool reserved_plot(const std::vector<Vec2>& plot) {
  // Reservations apply to complete occupied footprints, including balconies
  // and roof overhangs, rather than just the parcel centroid.
  const std::array<std::array<float,4>,8> reservations{{
    {{38,-48,176,165}},{{46,150,60,93}},{{212,170,93,74}},
    {{kLattice.x,kLattice.y,72,78}},{{-125,490,52,48}},
    {{190,-300,42,43}},{{410,95,54,53}},{{458.8f,-360,82,77}}
  }};
  for(const auto& r:reservations)
    if(polygons_overlap(plot,plan_rect(r[2]+5,r[3]+5,{r[0],r[1]})))return true;
  if(polygons_overlap(plot,kWestCivicCourt)||polygons_overlap(plot,moved_plan(kWestMarketReservation,kMarketMove))||polygons_overlap(plot,moved_plan(kMarketPromenade,kMarketMove))||polygons_overlap(plot,moved_plan(kCanalHexParcel,kHexMove))||polygons_overlap(plot,moved_plan(kCanalHexAccess,kHexMove))||polygons_overlap(plot,kGardenCompanionParcel))return true;
  if(polygons_overlap(plot,plan_rect(80,67,kDome+kDomeMove)))return true;
  for(const auto& reserved:transit_reserved_plans())if(polygons_overlap(plot,reserved))return true;
  for(const auto& reserved:market_approach_footprints())if(polygons_overlap(plot,reserved))return true;
  for(std::size_t i=0;i+1<kWestPublicRoute.size();++i) {
    Vec2 a=kWestPublicRoute[i],b=kWestPublicRoute[i+1],d=normalize(b-a),n{-d.y*9,d.x*9};
    if(polygons_overlap(plot,{a-n,b-n,b+n,a+n}))return true;
  }
  Vec2 lo,hi;plan_bounds(plot,&lo,&hi);
  if(hi.y>-1350&&lo.y<1250) {
    float z0=std::max(lo.y,-1350.f),z1=std::min(hi.y,1250.f);
    for(int i=0;i<4;++i) {
      float a=z0+(z1-z0)*i/4,b=z0+(z1-z0)*(i+1)/4;
      float ca=canal_centre(a),cb=canal_centre(b),wa=canal_halfwidth(a)+13,wb=canal_halfwidth(b)+13;
      if(polygons_overlap(plot,{{ca-wa,a},{cb-wb,b},{cb+wb,b},{ca+wa,a}}))return true;
    }
  }
  for(std::size_t i=0;i+1<kGardenApproach.size();++i) {
    Vec2 a=kGardenApproach[i],b=kGardenApproach[i+1],d=normalize(b-a),n{-d.y*8,d.x*8};
    if(polygons_overlap(plot,{a-n,b-n,b+n,a+n}))return true;
  }
  // The civic ribbon building stands on its own podium beside the square.
  return polygons_overlap(plot,plan_rect(47,41,{235,-12}));
}
void plant(Scene& sc,Rng rng,Vec3 p,float size,int species) {
  if(arrival_blockout)return;
  if(!sc.asset_library.resources.empty()) {
    constexpr std::array<const char*,5> kinds{"shrub_flowering","fern_arching","phormium","groundcover","fern_arching"};
    // Trailing plants hang from elevated edges; their pivot is the pot lip.
    add_asset_instance(sc,botanical_resource(sc,kinds[species%5],p),p,rng.range(0,6.283f),size);
    return;
  }
  const int stems=species==4?7:species==1?13:9;
  for(int i=0;i<stems;++i) {
    Rng r=rng.child(i);float a=i*2.399963f+r.range(-.2f,.2f);
    Vec3 d{std::cos(a),r.range(.15f,.6f),std::sin(a)},side{-d.z,0,d.x};
    float len=size*r.range(.55f,1.1f);
    if(species==1) {
      Vec3 tip=p+d*(len*.8f)+Vec3{0,len*.3f,0};
      Emit(&sc.opaque,M_HEDGE).tube(p,tip,.013f,4);
      for(int j=1;j<7;++j) for(float sign:{-1.f,1.f})
        leaf(sc,lerp(p,tip,j/7.f),side*sign+d*.3f,normalize(d),len*(1-j/8.f)*.36f,len*.07f,M_LEAF);
    } else leaf(sc,p,d,side,len,size*(species==2?.026f:.13f),i%3?M_HEDGE:M_LEAF);
  }
}
void small_tree(Scene& sc,Rng rng,Vec3 p,float height,int species) {
  if(arrival_blockout)return;
  if(!sc.asset_library.resources.empty()) {
    constexpr std::array<const char*,5> kinds{"canopy_broadleaf","canopy_columnar","palm_fan","palm_feather","tree_multistem"};
    int id=species%5;if(id==4)height=std::min(height,8.f);
    const auto& resource=sc.asset_library.resources[asset_resource(sc.asset_library,kinds[id])];
    float native_height=std::max(.1f,resource.mesh.bounds_max.y);
    add_asset_instance(sc,botanical_resource(sc,kinds[id],p),p,rng.range(0,6.283f),height/native_height);
    return;
  }
  Emit bark(&sc.opaque,M_BARK);
  Vec3 crown=p+Vec3{.3f,height*.63f,-.15f};
  bark.frustum(p,crown,height*.033f,height*.013f,7);
  int n=species==1?6:10;
  for(int j=0;j<n;++j) {
    Rng r=rng.child(j);float a=j*2.39996f;
    Vec3 d{std::cos(a),r.range(.05f,.3f),std::sin(a)};
    Vec3 b=crown+d*height*r.range(.2f,.38f)+Vec3{0,height*r.range(-.1f,.15f),0};
    bark.frustum(lerp(p,crown,.62f),b,height*.014f,.018f,5);
    for(int k=0;k<13;++k) {
      Rng q=r.child(k+70);Vec3 at=lerp(crown,b,q.range(.6f,1.15f))+Vec3{q.range(-.4f,.4f),q.range(-.3f,.5f),q.range(-.4f,.4f)};
      float angle=q.range(0,6.28f);Vec3 d2{std::cos(angle),q.range(-.4f,.3f),std::sin(angle)};
      leaf(sc,at,d2,{-d2.z,0,d2.x},height*q.range(.10f,.2f),height*.035f,k%3?M_LEAF:M_HEDGE);
    }
  }
}
void garden(Scene& sc,Rng rng,Vec2 c,float hx,float hz,float y,bool tree,bool detailed=true) {
  if(arrival_blockout)detailed=false;
  auto edge=plan_rounded_rect(hx,hz,std::min({.65f,hx*.5f,hz*.5f}),4,c);
  slab(sc.opaque,edge,y+.5f,.5f,M_CONCRETE_WHITE);
  slab(sc.opaque,plan_offset(edge,-.15f),y+.515f,.03f,M_SOIL);
  if(!detailed) { slab(sc.opaque,plan_offset(edge,-.28f),y+.59f,.04f,M_GRASS);return; }
  int n=std::clamp(int((hx+hz)*1.5f),4,28);
  for(int i=0;i<n;++i){Rng r=rng.child(i+10);Vec2 q=c+Vec2{r.range(-hx+.2f,hx-.2f),r.range(-hz+.2f,hz-.2f)};plant(sc,r.child(1),P3(q,y+.54f),r.range(.45f,1.1f),i%5);}
  if(tree) {
    int kind=c.x<shore(c.y)+95?2:int(std::abs(c.x+c.y)*.1f)%5;
    small_tree(sc,rng.child(2),P3(c,y+.54f),rng.child(3).range(4,7),kind);
  }
  if(y>8&&!sc.asset_library.resources.empty()) {
    Vec3 a=P3(c+Vec2{hx-.1f,hz-.1f},y+.5f),b=P3(c+Vec2{-hx+.1f,hz-.1f},y+.5f);
    add_asset_instance(sc,botanical_resource(sc,"climber_cascade",a),a,0,.6f);
    add_asset_instance(sc,botanical_resource(sc,"climber_cascade",b),b,.7f,.42f);
  }
  // Exposed irrigation follows the planting edge, with mulch below foliage.
  Emit(&sc.opaque,M_DARK_METAL).tube(P3(c-Vec2{hx-.22f,0},y+.55f),P3(c+Vec2{hx-.22f,0},y+.55f),.018f,4);
}

TowerSpec family(int id,int floors,float radius,float rot) {
  TowerSpec s;
  switch(id%16) {
    case 0:s=spec_diagrid(radius,floors);s.plan=PlanKind::Circle;s.member_r=.48f;break;
    case 1:s=spec_hex(radius,floors);s.member_r=.4f;break;
    case 2:s=spec_lens(radius*1.35f,radius*.67f,floors,rot);s.tip=.34f;s.taper=.12f;break;
    case 3:s=spec_finweave(radius,floors);s.plan=PlanKind::Circle;s.taper=.1f;break;
    case 4:s=spec_sail(radius*1.12f,radius*.51f,floors,rot);s.tip=.42f;break;
    case 5:s=spec_xframe(radius,radius*.7f,floors);s.taper=.12f;break;
    case 6:s=spec_diagrid(radius,floors);s.twist=.22f;s.exponent=4;s.setback_floor=floors*2/3;s.setback_scale=.8f;break;
    case 7:s=spec_finweave(radius,floors);s.facade=FacadeKind::Ribbon;s.plan=PlanKind::RoundedRect;s.b=radius*.62f;s.taper=.22f;break;
    case 8:s=spec_hex(radius,floors);s.plan=PlanKind::Polygon;s.sides=6;s.setback_floor=floors*3/4;break;
    case 9:s=spec_lens(radius*1.5f,radius*.8f,floors,rot);s.facade=FacadeKind::Louvre;s.tip=.2f;break;
    case 10:s=spec_finweave(radius*.73f,floors);s.taper=.32f;s.tip=.6f;s.crown=CrownKind::Mast;break;
    case 11:s=spec_hex(radius,floors);s.facade=FacadeKind::Curtain;s.member=M_BRONZE;s.plan=PlanKind::Polygon;s.sides=5;break;
    case 12:s=spec_xframe(radius*1.2f,radius*.5f,floors);s.setback_floor=floors/2;s.setback_scale=.73f;break;
    case 13:s=spec_diagrid(radius,floors);s.plan=PlanKind::Circle;s.tip=.24f;s.crown=CrownKind::Lantern;break;
    case 14:s=spec_finweave(radius,floors);s.plan=PlanKind::Superellipse;s.exponent=6;s.b=radius*.56f;s.setback_floor=floors*2/3;break;
    default:s=spec_lens(radius,radius*.75f,floors,rot);s.facade=FacadeKind::Ribbon;s.taper=.05f;s.crown=CrownKind::Louvres;break;
  }
  s.rot=rot;s.floor_h=4.0f;s.base=BaseKind::Lobby;s.base_scale=1.12f;s.base_floors=2;
  s.glass=id%4==0?M_GLASS_BRONZE:id%4==1?M_GLASS_DARK:id%4==2?M_GLASS_BLUE:M_GLASS_STD;
  s.member=(id%7==2||id%7==4)?M_BRONZE:M_WHITE_METAL;s.frame=M_BRONZE;
  s.crown=id%5==0?CrownKind::Lattice:s.crown;
  return s;
}
std::vector<Vec2> authored_tower_plan(const TowerSpec& spec,Vec2 centre) {
  std::vector<Vec2> plan;
  switch(spec.plan) {
    case PlanKind::Superellipse:plan=plan_superellipse(spec.a,spec.b,spec.exponent,64);break;
    case PlanKind::Circle:plan=plan_circle(spec.a,64);break;
    case PlanKind::RoundedRect:plan=plan_rounded_rect(spec.a,spec.b,std::min(spec.a,spec.b)*.35f,8);break;
    case PlanKind::Polygon:plan=plan_circle(spec.a,std::max(3,spec.sides));break;
    case PlanKind::Lens: {
      const float r=.5f*(spec.b+spec.a*spec.a/spec.b);
      plan=plan_lens(r,r-spec.b,32);break;
    }
  }
  return plan_transform(plan,centre,spec.rot);
}
void tower(Scene& sc,TowerSpec s,Vec2 p,float y,Rng rng,int detail) {
  // These are the same architectural plans used by the shared mesh builder,
  // kept in this standalone scene for later roof allocation. The upper frame
  // receives its real clearance instead of leaving plants under the shaft.
  auto footprint=plan_offset(authored_tower_plan(s,p),1.3f);
  sc.roof_obstructions.push_back({footprint,y,y+(s.floors+s.base_floors)*s.floor_h+8});
  auto first=std::uint32_t(sc.opaque.indices.size());
  build_floor_aligned_tower(sc,sc.tower_floor_materials,s,p,y,rng,arrival_blockout?-1:detail);
  const float h=s.floors*s.floor_h;
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(p,y+h*.5f),std::sqrt(h*h*.25f+s.a*s.a+s.b*s.b)+18);
  ++sc.stats_towers;
}
void bridge(Scene& sc,Vec3 a,Vec3 b,float width,int kind=0,float open_start=0,float open_end=0) {
  Emit(&sc.opaque,M_WHITE_METAL).beam(a,b,width,.6f);
  Vec3 side=normalize(cross(b-a,Vec3{0,1,0}))*(width*.5f-.12f);
  Vec3 direction=normalize(b-a),rail_a=a+direction*open_start,rail_b=b-direction*open_end;
  rail(sc,rail_a+side+Vec3{0,.3f,0},rail_b+side+Vec3{0,.3f,0},kind==1);
  rail(sc,rail_a-side+Vec3{0,.3f,0},rail_b-side+Vec3{0,.3f,0},kind==1);
  Emit bronze(&sc.opaque,M_BRONZE);
  bronze.beam(a-Vec3{0,.5f,0},b-Vec3{0,.5f,0},width*.6f,.45f);
  if(kind==2) {
    const float span=length(b-a);
    for(int i=0;i<12;++i) {
      float t0=std::max(i/12.f,open_start/span),t1=std::min((i+1)/12.f,1-open_end/span);
      if(t0>=t1)continue;
      Vec3 p=lerp(a,b,t0),q=lerp(a,b,t1);
      float h=std::sin(kPi*(i+.5f)/12)*4;
      bronze.beam(p+side+Vec3{0,.5f,0},q+side+Vec3{0,h+.5f,0},.14f,.14f);
      bronze.beam(p-side+Vec3{0,.5f,0},q-side+Vec3{0,h+.5f,0},.14f,.14f);
    }
  }
}
// The coastline is a continuous natural constraint. Arterials follow it;
// district parcels change widths, local orientation and depth independently.
float shore(float z) {
  float headland=(z-405)/290.f;
  return -560+310*std::sin((z+850)/1450.f)+90*std::sin(z/430.f)-240*std::exp(-headland*headland);
}
float east_shore(float z) {
  return 950+350*std::sin((z+300)/950.f)+.20f*std::max(0.f,-z-1600);
}
float canal_centre(float z) {
  // Surveyed straight visible reach. Beyond the Arrival frame the same
  // channel joins the existing western sea through straight return reaches.
  if(z<-550) {float t=std::clamp((z+1400)/850.f,0.f,1.f);return -820*(1-t)+(-259.6f+.155f*-550)*t;}
  if(z>650) {float t=std::clamp((z-650)/650.f,0.f,1.f);return (-259.6f+.155f*650)*(1-t)-360*t;}
  return -259.6f+.155f*z;
}
float canal_halfwidth(float) {return 17.f;}
std::vector<std::vector<Vec2>> arrival_promenade_plans() {
  std::vector<std::vector<Vec2>> plans;
  for (float sign : {-1.f, 1.f}) {
    const Vec2 a{canal_centre(504) + sign * (canal_halfwidth(504) + 5), 504};
    const Vec2 b{canal_centre(-446) + sign * (canal_halfwidth(-446) + 5), -446};
    const Vec2 side = riverfront::across * 4.f;
    plans.push_back({a - side, b - side, b + side, a + side});
  }
  return plans;
}
Vec2 street(float row,float offset) {
  float lateral=kGridLateralOrigin+offset;
  const Vec2 original{kGridCos*lateral+kGridSin*row,-kGridSin*lateral+kGridCos*row};
  const Vec2 delta=original-riverfront::origin;
  const float s=dot(delta,Vec2{kGridCos,-kGridSin}),t=dot(delta,Vec2{kGridSin,kGridCos});
  auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
  const float weight=smooth((s+260)/140)*smooth((550-s)/200)*
                     smooth((t+220)/180)*smooth((700-t)/360);
  // The riverfront and its immediately adjacent parcels share the canal's
  // real plan direction. Transition back into the retained outer-city survey.
  const Vec2 aligned=riverfront::point(s*(riverfront::avenue_lateral/211.783073f),t);
  return original*(1-weight)+aligned*weight;
}
float avenue_width(std::size_t column) {return column==4?30.f:column%4==0?20.f:12.f;}
float cross_width(float row,std::size_t index) {return std::abs(row-kNearestBridgeRow)<.01f?20.f:index%6==0?18.f:12.f;}
bool cross_spans_canal(float row,std::size_t index) {return std::abs(row-kNearestBridgeRow)<.01f||index%3==0;}
std::vector<std::vector<Vec2>> arrival_primary_corridors() {
  auto corridor=[](Vec2 a,Vec2 b,float width) {
    Vec2 n=normalize(Vec2{-(b-a).y,(b-a).x})*(width*.5f);
    return std::vector<Vec2>{a-n,b-n,b+n,a+n};
  };
  std::vector<std::vector<Vec2>> result;
  const auto rows=coastal_rows(root_rng("arrival-corridors"));
  for(std::size_t i=0;i+1<rows.size();++i)
    result.push_back(corridor(street(rows[i],485),street(rows[i+1],485),31));
  for(std::size_t i=0;i+1<kOffsets.size();++i)
    result.push_back(corridor(street(kNearestBridgeRow,kOffsets[i]),street(kNearestBridgeRow,kOffsets[i+1]),21));
  return result;
}
std::vector<std::vector<Vec2>> market_approach_footprints() {
  const auto walk=relocated_market_approach();
  std::vector<std::vector<Vec2>> plans;
  for(std::size_t i=0;i+1<walk.size();++i) {
    Vec2 a=walk[i],b=walk[i+1],d=normalize(b-a),n{-d.y*4,d.x*4};
    plans.push_back({a-n,b-n,b+n,a+n});
  }
  for(Vec2 p:walk)plans.push_back(plan_circle(4,24,p));
  return plans;
}
float road_surface_height(Vec2 p,const std::vector<Vec2>& crossings) {
  float elevation=0;
  for(Vec2 crossing:crossings) {
    float d=length(p-crossing)-canal_halfwidth(crossing.y)-9;
    elevation=std::max(elevation,4*std::clamp(1-d/55,0.f,1.f));
  }
  return kDeck-.11f+elevation;
}
std::vector<std::pair<Vec2,Vec2>> road_plan_segments(const std::vector<Vec2>& line) {
  std::vector<std::pair<Vec2,Vec2>> retained_segments;
  for(std::size_t i=0;i+1<line.size();++i) {
    const Vec2 a=line[i],b=line[i+1];
    const Vec2 u=riverfront::coordinates(a),v=riverfront::coordinates(b);
    if(std::max(u.x,v.x)<=30||std::min(u.x,v.x)>=179||
       std::max(u.y,v.y)<=13||std::min(u.y,v.y)>=193) {
      retained_segments.push_back({a,b});continue;
    }
    std::vector<float> cuts{0,1};
    auto cut=[&](float start,float finish,float boundary) {
      if(std::abs(finish-start)<1e-6f)return;
      const float t=(boundary-start)/(finish-start);
      if(t>0&&t<1)cuts.push_back(t);
    };
    // The formerly subdivided site is one complete corner block. Its internal
    // crossstreet is retired all the way to the back-street sidewalk.
    for(float edge:{30.f,179.f})cut(u.x,v.x,edge);
    for(float edge:{13.f,193.f})cut(u.y,v.y,edge);
    std::sort(cuts.begin(),cuts.end());
    for(std::size_t k=0;k+1<cuts.size();++k) {
      if(cuts[k+1]-cuts[k]<1e-6f)continue;
      const Vec2 mid=u+(v-u)*((cuts[k]+cuts[k+1])*.5f);
      if(mid.x>30&&mid.x<179&&mid.y>13&&mid.y<193)continue;
      retained_segments.push_back({a+(b-a)*cuts[k],a+(b-a)*cuts[k+1]});
    }
  }
  std::vector<std::pair<Vec2,Vec2>> result;
  for(const auto& segment:retained_segments) {
    const auto a=segment.first,b=segment.second;
    const auto u=riverfront::coordinates(a),v=riverfront::coordinates(b);
    std::vector<float> cuts{0,1};
    // Exact plateau ends give the nearest bridge symmetric deck bearings and
    // keep its generated ramp profile identical to the public route sampling.
    if(std::abs(u.y)<.01f&&std::abs(v.y)<.01f&&std::abs(v.x-u.x)>1e-6f)
      for(float edge:{-26.f,26.f}) {
        const float t=(edge-u.x)/(v.x-u.x);if(t>0&&t<1)cuts.push_back(t);
      }
    std::sort(cuts.begin(),cuts.end());
    for(std::size_t i=0;i+1<cuts.size();++i)
      if(cuts[i+1]-cuts[i]>1e-6f)result.push_back({a+(b-a)*cuts[i],a+(b-a)*cuts[i+1]});
  }
  return result;
}
void road(Scene& sc,const std::vector<Vec2>& line,float width,bool lit,bool cross_canal=true) {
  Emit asphalt(&sc.opaque,M_ASPHALT),curb(&sc.opaque,M_CONCRETE_DARK),stripe(&sc.opaque,M_LANE_WHITE);
  float travelled=0,next_lamp=14,next_edge=5;
  std::vector<Vec2> crossings;
  for(std::size_t i=0;i+1<line.size();++i) {
    Vec2 a=line[i],b=line[i+1];float da=a.x-canal_centre(a.y),db=b.x-canal_centre(b.y);
    if(da*db>=0)continue;
    float t=std::abs(da)/(std::abs(da)+std::abs(db));Vec2 p=a+(b-a)*t;
    if(cross_canal&&p.y>-1350&&p.y<1250&&p.x>shore(p.y)+canal_halfwidth(p.y)+7)crossings.push_back(p);
  }
  const auto retained_segments=road_plan_segments(line);
  for(const auto& segment:retained_segments) {
    Vec2 aa=segment.first,bb=segment.second;
    auto inside=[&](Vec2 p){return p.x>=shore(p.y)+width*.6f&&p.x<=east_shore(p.y)-width*.6f;};
    bool ia=inside(aa),ib=inside(bb);
    if(!ia&&!ib)continue;
    if(ia!=ib) {
      float lo=0,hi=1;
      for(int step=0;step<14;++step){float t=(lo+hi)*.5f;bool im=inside(aa*(1-t)+bb*t);if(im==ia)lo=t;else hi=t;}
      Vec2 edge=aa*(1-(lo+hi)*.5f)+bb*((lo+hi)*.5f);
      if(ia)bb=edge;else aa=edge;
    }
    int pieces=std::max(1,int(length(bb-aa)/14));
    for(int piece=0;piece<pieces;++piece) {
      Vec2 pa=aa+(bb-aa)*(float(piece)/pieces),pb=aa+(bb-aa)*(float(piece+1)/pieces);
      Vec2 mid=(pa+pb)*.5f;
      if(!cross_canal&&mid.y>-1350&&mid.y<1250&&std::abs(mid.x-canal_centre(mid.y))<canal_halfwidth(mid.y)+12) {
        travelled+=length(pb-pa);next_lamp=std::max(next_lamp,travelled);next_edge=std::max(next_edge,travelled);continue;
      }
      Vec3 a=P3(pa,road_surface_height(pa,crossings)),b=P3(pb,road_surface_height(pb,crossings));
      Vec3 side=normalize(cross(b-a,Vec3{0,1,0}))*width*.5f;
      asphalt.quad_metric(a+side,b+side,b-side,a-side);
      curb.beam(a-side,b-side,.22f,.19f);curb.beam(a+side,b+side,.22f,.19f);
      if(a.y>4.7f&&b.y>4.7f) {
        if(mid.y>=-450&&mid.y<=280)build_canal_bridge_edge(sc,a,b,width,width>12,
            std::abs(riverfront::coordinates(mid).y)<1.f);
        else {
          Emit(&sc.opaque,M_CONCRETE_WHITE).beam(a-Vec3{0,.36f,0},b-Vec3{0,.36f,0},width,.65f);
          rail(sc,a+side,b+side);rail(sc,a-side,b-side);
        }
      }
      if(piece%4==0&&a.y>kDeck+.6f) {
        Vec3 foot=a+side*.7f;
        box(sc,M_CONCRETE_DARK,{foot.x,(a.y-1.5f)*.5f,foot.z},{.5f,(a.y+1.5f)*.5f,.5f});
        foot=a-side*.7f;
        box(sc,M_CONCRETE_DARK,{foot.x,(a.y-1.5f)*.5f,foot.z},{.5f,(a.y+1.5f)*.5f,.5f});
      }
      if(width>12) {
        int marks=std::max(1,int(length(b-a)/9));
        for(int j=0;j<marks;++j) {
          Vec3 p=lerp(a,b,float(j)/marks)+Vec3{0,.012f,0};
          stripe.beam(p,p+normalize(b-a)*3,.07f,.015f);
          if(std::abs(riverfront::coordinates(mid).y)<1.f&&std::abs(riverfront::coordinates(mid).x)<75)
            for(float sign:{-1.f,1.f}) {
              Vec3 lane=p+side*(sign*.39f);
              stripe.beam(lane,lane+normalize(b-a)*3,.075f,.015f);
            }
        }
      }
      float run=length(b-a);
      if(lit)while(next_lamp<=travelled+run) {
        Vec3 p=lerp(a,b,std::clamp((next_lamp-travelled)/run,0.f,1.f))+side+Vec3{0,.11f,0};
        lamp(sc,p,4.8f);sc.lights.back().radius=23;sc.lights.back().intensity=6;
        next_lamp+=28;
      }
      if(width>12)while(next_edge<=travelled+run) {
        Vec3 p=lerp(a,b,std::clamp((next_edge-travelled)/run,0.f,1.f));
        for(float sign:{-1.f,1.f})
          Emit(&sc.opaque,M_SIGN).beam(p+side*sign+Vec3{0,.025f,0},p+side*sign+normalize(b-a)*1.8f+Vec3{0,.025f,0},.07f,.025f);
        next_edge+=8;
      }
      travelled+=run;
    }
  }
}
void terrain(Scene& sc,Rng district_rng) {
  const auto canal_crossings=canal_construction_crossings(district_rng);
  const auto canal_exclusions=canal_construction_exclusions(district_rng);
  box(sc,M_WATER,{0,-.2f,0},{150000,.2f,150000});
  // Quay wall and planted shoreline are real geometry down to the water.
  std::vector<Vec2> coast;
  for(int i=0;i<=204;++i) {float z=3430-i*38.f;coast.push_back({shore(z),z});}
  for(std::size_t i=0;i+1<coast.size();++i) {
    Vec2 a=coast[i],b=coast[i+1];
    Emit ground(&sc.opaque,M_GRASS);
    Vec2 east_a{east_shore(a.y),a.y},east_b{east_shore(b.y),b.y};
    auto strip=[&](float left_a,float right_a,float left_b,float right_b) {
      float wa=right_a-left_a,wb=right_b-left_b;
      if(wa>.01f&&wb>.01f)ground.quad_metric({left_a,.65f,a.y},{right_a,.65f,a.y},{right_b,.65f,b.y},{left_b,.65f,b.y});
      else if(wa>.01f)ground.triangle({left_a,.65f,a.y},{right_a,.65f,a.y},{left_b,.65f,b.y});
      else if(wb>.01f)ground.triangle({left_a,.65f,a.y},{right_b,.65f,b.y},{left_b,.65f,b.y});
    };
    bool water_cut=a.y>-1350&&b.y<1250;
    if(water_cut) {
      float ca=canal_centre(a.y),cb=canal_centre(b.y),wa=canal_halfwidth(a.y),wb=canal_halfwidth(b.y);
      strip(a.x,std::clamp(ca-wa,a.x,east_a.x),b.x,std::clamp(cb-wb,b.x,east_b.x));
      strip(std::clamp(ca+wa,a.x,east_a.x),east_a.x,std::clamp(cb+wb,b.x,east_b.x),east_b.x);
      for(float sign:{-1.f,1.f}) {
        Vec2 ba{ca+sign*wa,a.y},bb{cb+sign*wb,b.y};
        if(ba.x>a.x+4&&bb.x>b.x+4) {
          if(ba.y<=504&&bb.y>=-450) {
            build_canal_bank(sc,ba,bb,sign,kDeck,canal_crossings,canal_exclusions);
            continue;
          }
          Emit(&sc.opaque,M_CONCRETE_DARK).beam(P3(ba,0),P3(bb,0),1.4f,2.4f);
          Vec3 qa=P3(ba+Vec2{sign*5,0},kDeck-.2f),qb=P3(bb+Vec2{sign*5,0},kDeck-.2f);
          Emit(&sc.opaque,M_PLAZA).beam(qa,qb,8,.4f);
          lamp(sc,lerp(qa,qb,.5f)+Vec3{sign*2,.2f,0},4.2f);
          sc.lights.back().radius=21;sc.lights.back().intensity=5;
          if(i%3==0) {
            float z=ba.y-13;Vec2 at{canal_centre(z)+sign*(canal_halfwidth(z)+9),z};
            garden(sc,root_rng("canal").child(i,sign>0?1:0),at,2.6f,5.5f,kDeck,true,true);
          }
        }
      }
    } else strip(a.x,east_a.x,b.x,east_b.x);
    bool west_open=water_cut&&(std::abs(a.x-canal_centre(a.y))<canal_halfwidth(a.y)+1||std::abs(b.x-canal_centre(b.y))<canal_halfwidth(b.y)+1);
    if(!west_open) {
      Emit(&sc.opaque,M_CONCRETE_DARK).beam(P3(a,0),P3(b,0),3.5f,2.3f);
      Emit(&sc.opaque,M_PLAZA).beam(P3(a+Vec2{7,0},kDeck-.2f),P3(b+Vec2{7,0},kDeck-.2f),12,.4f);
      if(std::abs(a.y)<2100) {
        lamp(sc,P3((a+b)*.5f+Vec2{5,0},kDeck),5);sc.lights.back().radius=23;sc.lights.back().intensity=6;
      }
    }
    Emit(&sc.opaque,M_CONCRETE_DARK).beam(P3(east_a,0),P3(east_b,0),3.5f,2.3f);
    Emit(&sc.opaque,M_PLAZA).beam(P3(east_a-Vec2{7,0},kDeck-.2f),P3(east_b-Vec2{7,0},kDeck-.2f),12,.4f);
    if(std::abs(a.y)<2400) {
      lamp(sc,P3((east_a+east_b)*.5f-Vec2{5,0},kDeck),5);sc.lights.back().radius=23;sc.lights.back().intensity=6;
    }
  }
  // Northern headland closes the bay, with a lower-density far-shore skyline.
  box(sc,M_ASPHALT,{-650,.3f,-4700},{2700,.35f,950});
  for(int band=0;band<3;++band) for(int i=0;i<28;++i) {
    float x=-5000+i*380.f,z=-6000-band*850.f;
    float h=80+120*(.5f+.5f*std::sin(i*1.76f+band));
    Emit e(&sc.opaque,M_CONCRETE_DARK);
    e.triangle({x-260,0,z},{x,h,z-70},{x+280,0,z});
    e.triangle({x-260,0,z},{x+50,0,z-560},{x,h,z-70});
  }
}
Mat roof_finish(Scene& sc,int style) {
  const int family=style%6<3?0:style%6<5?1:2;
  const std::array<const char*,3> names{{"dry charcoal mineral roof","dry warm stone roof","dry pale stone roof"}};
  for(std::size_t i=0;i<sc.materials.size();++i)
    if(sc.materials[i].name==names[family])return static_cast<Mat>(i);
  MaterialDesc material=sc.materials[M_MARBLE_WHITE];material.name=names[family];
  material.flags&=~512u;material.roughness=.82f;material.normal_strength=.30f;
  material.base_color=family==0?Vec3{.25f,.30f,.31f}:family==1?Vec3{.49f,.42f,.29f}:Vec3{.83f,.82f,.76f};
  material.uv_scale=3.2f;
  sc.materials.push_back(material);return static_cast<Mat>(sc.materials.size()-1);
}
std::vector<std::vector<Vec2>> roof_garden(Scene& sc,Rng source,const std::vector<Vec2>& roof,Vec2 c,float y,int style,
    const std::vector<std::vector<Vec2>>& exclusions={}) {
  const float radius=plan_inradius(roof);if(radius<4)return {};
  Vec2 axis=normalize(plan_long_axis(roof));if(style%2)axis={-axis.y,axis.x};
  std::vector<std::vector<Vec2>> free{roof};
  for(const auto& obstacle:exclusions) {
    std::vector<std::vector<Vec2>> next;
    for(const auto& part:free)for(auto p:subtract_convex(part,obstacle,.1f))next.push_back(std::move(p));
    free=std::move(next);
  }
  float free_area=0;for(const auto& part:free)free_area+=std::abs(plan_area(part));
  const float planted_target=free_area*(.32f+.035f*(style%4));
  std::vector<std::vector<Vec2>> beds;float best_error=1e30f;
  // A roof garden follows the remaining terrace, not the original footprint's
  // arbitrary north side. This also gives setback balconies planted edges even
  // when their entire original planting half is occupied by an upper storey.
  for(int direction=0;direction<4;++direction)for(float offset:{-.25f,0.f,.25f,.48f}) {
    const Vec2 normal=direction==0?axis:direction==1?axis*-1.f:
                      direction==2?Vec2{-axis.y,axis.x}:Vec2{axis.y,-axis.x};
    auto candidate=clip_halfplane(plan_scale(roof,.94f,c),c+normal*(radius*offset),normal);
    if(candidate.size()<3)continue;
    std::vector<std::vector<Vec2>> patches{candidate};
    for(const auto& obstacle:exclusions) {
      std::vector<std::vector<Vec2>> next;
      for(const auto& patch:patches)for(auto part:subtract_convex(patch,plan_offset(obstacle,1.2f),4.f))
        if(plan_inradius(part)>.9f)next.push_back(std::move(part));
      patches=std::move(next);if(patches.empty())break;
    }
    float area=0;for(const auto& part:patches)area+=std::abs(plan_area(part));
    if(area<25)continue;
    const float error=std::abs(area-planted_target);
    if(error<best_error){best_error=error;beds=std::move(patches);}
  }
  if(beds.empty())return {};
  for(std::size_t piece=0;piece<beds.size();++piece) {
    const auto& bed=beds[piece];Rng rng=source.child(static_cast<std::uint32_t>(piece));
  slab(sc.opaque,bed,y+.34f,.34f,M_CONCRETE_WHITE);
  auto soil=plan_offset(bed,-.16f);if(soil.size()<3)continue;slab(sc.opaque,soil,y+.37f,.025f,M_SOIL);
  // Connected ground-cover masses carry the planted silhouette at distance;
  // exposed mulch and layered leaves provide its structure nearby.
  auto ground=plan_offset(soil,-.4f);if(ground.size()<3)continue;slab(sc.opaque,ground,y+.43f,.045f,M_GRASS);
  Vec2 lo,hi;plan_bounds(ground,&lo,&hi);
  Vec2 green_c=plan_centroid(ground);
  float area=std::abs(plan_area(ground));int canopy_count=std::clamp(int(area/90),2,32);
  for(int i=0;i<canopy_count;++i) {
    Rng r=rng.child(10+i);Vec2 q=green_c+Vec2{r.range(-.40f,.40f)*(hi.x-lo.x),r.range(-.40f,.40f)*(hi.y-lo.y)};
    if(!point_in_polygon(ground,q))q=green_c;
    bool tree_clear=true;
    for(const auto& obstacle:exclusions)if(point_in_polygon(plan_offset(obstacle,5.5f),q)){tree_clear=false;break;}
    if(!tree_clear)continue;
    small_tree(sc,r.child(2),P3(q,y+.44f),r.range(8.0f,13.0f),i%3==0?(style+i)%5:0);
    for(int j=0;j<6;++j) {
      Rng under=r.child(30+j);Vec2 s=q+Vec2{under.range(-3,3),under.range(-3,3)};
      if(point_in_polygon(ground,s))plant(sc,under.child(1),P3(s,y+.44f),under.range(.8f,1.4f),j%2);
    }
  }
  int patches=std::clamp(int(area/9),4,36);
  for(int i=0;i<patches;++i) {
    Rng r=rng.child(100+i);Vec2 q{r.range(lo.x,hi.x),r.range(lo.y,hi.y)};
    if(!point_in_polygon(ground,q))continue;
    if(!sc.asset_library.resources.empty())
      add_asset_instance(sc,botanical_resource(sc,"groundcover",P3(q,y+.45f)),P3(q,y+.45f),r.range(0,6.28f),r.range(1.1f,2.1f));
    else plant(sc,r.child(1),P3(q,y+.45f),1.4f,3);
  }
  if(!sc.asset_library.resources.empty()) {
    Sampled edge=plan_sample(bed,11.f);
    for(std::size_t i=0;i<edge.points.size();i+=2) {
      Vec3 p=P3(edge.points[i],y+.4f);
      add_asset_instance(sc,botanical_resource(sc,"climber_cascade",p),p,std::atan2(edge.normals[i].x,edge.normals[i].y),.65f);
    }
    add_asset_instance(sc,"roof_irrigation",P3(green_c,y+.4f),0,1.f);
  }
  }
  return beds;
}
void low_building(Scene& sc,Rng rng,const std::vector<Vec2>& outline,Vec2 c,float base,int floors,int style,bool detail) {
  if(arrival_blockout)detail=false;
  // Low frontage remains two to four occupied storeys in most parcels. Each
  // real setback is allocated only after every neighbouring shaft exists.
  const float floor_h=4;auto p=outline;
  const Mat roof_surface=roof_finish(sc,style);
  const std::array<Mat,6> glass_family{{M_GLASS_BLUE,M_GLASS_STD,M_GLASS_BRONZE,M_GLASS_DARK,M_GLASS_GREEN,M_GLASS_SILVER}};
  const Mat glass=glass_family[style%glass_family.size()];
  const Mat frame=style%4==0?M_BRONZE:style%4==1?M_CONCRETE_WHITE:M_WHITE_METAL;
  for(int f=0;f<floors;++f) {
    const float y=base+f*floor_h;
    const bool setback=f>0&&((style%3==0&&f%3==0)||(style%3==1&&f%2==0)||(style%3==2&&f==1));
    if(setback) {
      if(detail)sc.authored_roofs.push_back({p,y,style,rng.child(600+f)});
      const float scale=.77f+.035f*(style%4);
      p=plan_scale(p,scale,c);
      if(style%3==1)for(auto& v:p)v=v+Vec2{.5f,-.35f};
    }
    sc.roof_obstructions.push_back({p,y,y+4});
    const float recess=f==0?(style%2?1.35f:.8f):.37f;
    Emit glazing(&sc.opaque,glass);glazing.element_random=rng.child(f+20).next();
    glazing.wall(plan_offset(p,-recess),y+.22f,y+3.81f,true);
    slab(sc.opaque,p,y+4,.19f,frame);
    // Stone-topped slabs and narrow exposed fascia have distinct thicknesses;
    // there is no deep white roof plate repeated at every level.
    Emit(&sc.opaque,roof_surface).polygon(plan_offset(p,-.18f),y+4.008f,true);
    if(detail) {
      Sampled edges=plan_sample(p,3.5f+(style%3)*.45f);
      for(std::size_t i=0;i<edges.points.size();++i) {
        Vec2 outward=edges.normals[i],side{-outward.y,outward.x};
        Vec3 q=P3(edges.points[i]-outward*.15f,y+2);
        Emit(&sc.opaque,frame).box(q,{.08f,1.81f,.22f},{side.x,0,side.y},{0,1,0},{outward.x,0,outward.y});
        if(f==0&&style%2) {
          box(sc,M_LOBBY_LIGHT,q+Vec3{-outward.x*.5f,1.57f,-outward.y*.5f},{.13f,.025f,.13f});
          if(i%3==0)sc.lights.push_back({q+Vec3{-outward.x*.6f,1.35f,-outward.y*.6f},5,{1,.77f,.49f},2.1f});
        }
      }
      if(!sc.asset_library.resources.empty()) {
        Sampled bays=plan_sample(p,9.5f);
        for(std::size_t i=0;i<bays.points.size();i+=2) {
          Vec2 n=bays.normals[i];float yaw=std::atan2(n.x,n.y);
          Vec3 q=P3(bays.points[i]-n*(recess-.10f),y+.10f);
          const char* module=(i+f+style)%7==0?"facade_service_panel":
                             (i+f+style)%3==0?"facade_bronze_louver":"facade_window_bay";
          add_asset_instance(sc,module,q,yaw,1.f);
          if(f==0&&i%6==0)add_asset_instance(sc,"glass_door",q,yaw,1.f);
        }
      }
    }
  }
  const float top=base+floors*floor_h;
  if(detail) {
    sc.authored_roofs.push_back({p,top+.008f,style,rng.child(180)});
    parapet(sc.opaque,p,top,.92f,.16f,frame);
  }
  ++sc.stats_standards;
}
void sustained_frontage(Scene& sc,Rng rng,const std::vector<Vec2>& outline,
                        float base,int floors,int identity,bool upper_wing) {
  // A street house holds its occupied perimeter through the lower floors.
  // Only a selected upper wing steps back, leaving a substantial roof garden
  // beside it instead of shrinking every floor into another glass pyramid.
  const Vec2 centre=plan_centroid(outline);
  const Mat finish=roof_finish(sc,identity);
  const char* name=identity%2?"warm mineral street frontage":"cool mineral street frontage";
  Mat body=M_CONCRETE_WHITE;
  for(std::size_t i=0;i<sc.materials.size();++i)if(sc.materials[i].name==name)body=static_cast<Mat>(i);
  if(body==M_CONCRETE_WHITE) {
    MaterialDesc material=sc.materials[M_CONCRETE_WHITE];material.name=name;
    material.base_color=identity%2?Vec3{.48f,.40f,.29f}:Vec3{.41f,.48f,.49f};
    material.roughness=.62f;material.normal_strength=.20f;
    body=static_cast<Mat>(sc.materials.size());sc.materials.push_back(material);
  }
  const Mat glass=identity%3==0?M_GLASS_BLUE:identity%3==1?M_GLASS_BRONZE:M_GLASS_DARK;
  auto storeys=[&](const std::vector<Vec2>& p,float floor_y,int count,Rng r) {
    const auto window_plan=inset_convex_plan(p,.50f);
    if(window_plan.size()<3)return;
    if(arrival_blockout) {
      Emit(&sc.opaque,body).wall(p,floor_y,floor_y+count*4,true);
      slab(sc.opaque,p,floor_y+count*4,.16f,body);return;
    }
    for(int floor=0;floor<count;++floor) {
      const float y=floor_y+floor*4;
      sc.roof_obstructions.push_back({p,y,y+4});
      Emit masonry(&sc.opaque,body),glazing(&sc.opaque,glass),frame(&sc.opaque,M_BRONZE);
      glazing.element_random=r.child(floor).next();
      masonry.wall(p,y+3.43f,y+3.84f,true);
      if(floor>0)masonry.wall(p,y,y+.62f,true);
      for(std::size_t edge=0;edge<window_plan.size();++edge) {
        Vec2 a=window_plan[edge],b=window_plan[(edge+1)%window_plan.size()],d=b-a;
        const float length_edge=length(d);if(length_edge<.1f)continue;
        const Vec2 along=normalize(d),normal{along.y,-along.x};
        const int bays=std::max(1,int(std::ceil(length_edge/4.3f)));
        for(int bay=0;bay<bays;++bay) {
          Vec2 one=a+d*(float(bay)/bays),two=a+d*(float(bay+1)/bays);
          const bool door=floor==0&&length_edge>10&&bay==bays/2;
          if(!door)glazing.quad_metric(P3(two,y+(floor?.62f:.16f)),P3(one,y+(floor?.62f:.16f)),P3(one,y+3.44f),P3(two,y+3.44f));
          else {
            // An actual opening in the envelope, with a supported head and
            // jambs. The continuous ground court reaches its clear threshold.
            glazing.quad_metric(P3(two,y+3.05f),P3(one,y+3.05f),P3(one,y+3.44f),P3(two,y+3.44f));
            frame.box(P3((one+two)*.5f,y+3.04f),{length_edge/bays*.5f,.09f,.14f},
                {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
          }
          const float pier=bay%3==0?.19f:.085f;
          masonry.box(P3(one+normal*.24f,y+1.75f),{pier,1.75f,.32f},
              {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
          frame.box(P3((one+two)*.5f,y+3.44f),{length_edge/bays*.5f,.045f,.08f},
              {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
        }
      }
      slab(sc.opaque,p,y+4,.16f,body);
      Emit(&sc.opaque,finish).polygon(inset_convex_plan(p,.12f),y+4.008f,true);
    }
  };
  storeys(outline,base,floors,rng.child(1));
  const float roof_y=base+floors*4+.008f;
  sc.authored_roofs.push_back({outline,roof_y,identity,rng.child(2)});
  parapet(sc.opaque,outline,roof_y,.82f,.16f,body);
  if(upper_wing&&std::abs(plan_area(outline))>600) {
    Vec2 axis=plan_long_axis(outline);float lo,hi;plan_extent(outline,axis,&lo,&hi);
    Vec2 axis_centre=centre+axis*((hi+lo)*.5f-dot(centre,axis));
    auto wing=clip_halfplane(inset_convex_plan(outline,1.3f),axis_centre+axis*((hi-lo)*.14f),axis);
    if(wing.size()>=3&&plan_inradius(wing)>3.4f&&std::abs(plan_area(wing))>90) {
      storeys(wing,base+floors*4,1,rng.child(3));
      sc.authored_roofs.push_back({wing,roof_y+4,identity+2,rng.child(4)});
      parapet(sc.opaque,wing,roof_y+4,.74f,.15f,body);
    }
  }
  ++sc.stats_standards;
}
Vec2 foreground_slab_centre(float y) {
  float t=std::clamp((y-13.2f)/193.6f,0.f,1.f);t=t*t*(3-2*t);
  return Vec2{43.17630f,342.26577f}*(1-t)+Vec2{26.05630f,340.00577f}*t;
}
float foreground_slab_scale(float y) {
  float t=std::clamp((y-13.2f)/193.6f,0.f,1.f);return 1-.20f*t*t*(3-2*t);
}
std::vector<Vec2> foreground_slab_plan(float y,float margin=0) {
  const float s=foreground_slab_scale(y);
  return plan_superellipse(21.83f*s+margin,29.47f*s+margin,2,96,foreground_slab_centre(y),.18f);
}
void foreground_dark_slab(Scene& sc,Rng rng,const std::vector<Vec2>& land) {
  // The adjacent address253 moves with the canal-parallel block survey.
  // Its unchanged shaft profile retains a bearing margin on all three frontage floors.
  const auto first=static_cast<std::uint32_t>(sc.opaque.indices.size());
  const auto podium=inset_convex_plan(land,1.2f);
  slab(sc.opaque,plan_scale(land,.985f,plan_centroid(land)),kDeck,.55f,M_SIDEWALK);
  sustained_frontage(sc,rng.child(1),podium,kDeck,3,253,false);
  MaterialDesc glass=sc.materials[M_GLASS_DARK];
  glass.name="foreground curved graphite occupied glazing";
  glass.base_color={.43f,.53f,.61f};glass.metallic=.34f;glass.roughness=.095f;
  glass.lit_probability=.23f;glass.room_h=193.6f/47.f;glass.room_w=3.6f;
  const Mat glazing=static_cast<Mat>(sc.materials.size());sc.materials.push_back(glass);
  MaterialDesc opaque=sc.materials[M_DARK_METAL];
  opaque.name="foreground graphite insulated spandrel";
  opaque.base_color={.045f,.054f,.061f};opaque.metallic=.38f;opaque.roughness=.28f;
  const Mat spandrel=static_cast<Mat>(sc.materials.size());sc.materials.push_back(opaque);
  constexpr int panels=96,floors=47;constexpr float floor_h=193.6f/floors;
  const float building_seed=rng.child(2).next();
  auto position=[](float a,float y,float inset=0.f) {
    float s=foreground_slab_scale(y);Vec2 c=foreground_slab_centre(y);
    Vec2 p{(21.83f*s-inset)*std::cos(a),(29.47f*s-inset)*std::sin(a)};
    return Vec3{c.x+p.x*std::cos(.18f)-p.y*std::sin(.18f),y,
                c.y+p.x*std::sin(.18f)+p.y*std::cos(.18f)};
  };
  auto smooth_face=[&](Mat material,float a,float b,float low,float high,float u0,float u1) {
    Emit face(&sc.opaque,material);face.element_random=building_seed;
    const auto begin=sc.opaque.vertices.size();
    face.quad(position(b,low),position(a,low),position(a,high),position(b,high),
              {{u1,low-13.2f},{u0,low-13.2f},{u0,high-13.2f},{u1,high-13.2f}});
    const std::array<std::pair<float,float>,4> at{{{b,low},{a,low},{a,high},{b,high}}};
    for(std::size_t i=0;i<4;++i) {
      auto& vertex=sc.opaque.vertices[begin+i];auto [angle,y]=at[i];
      Vec3 du=position(angle+.0005f,y)-position(angle-.0005f,y);
      Vec3 dv=position(angle,y+.001f)-position(angle,y-.001f);
      vertex.normal=normalize(cross(dv,du));
      Vec3 tangent=vertex.tangent.xyz();tangent=normalize(tangent-vertex.normal*dot(tangent,vertex.normal));
      vertex.tangent={tangent.x,tangent.y,tangent.z,vertex.tangent.w};
    }
  };
  std::array<float,panels+1> distances{};
  for(int i=0;i<panels;++i)
    distances[i+1]=distances[i]+length(position((i+1)*2*kPi/panels,13.2f)-position(i*2*kPi/panels,13.2f));
  auto cores=[&](float low,float high) {
    const Vec2 a=foreground_slab_centre(low),b=foreground_slab_centre(high);
    for(float sign:{-1.f,1.f}) {
      const auto lower=plan_rect(.25f,6,a+Vec2{sign*4.5f,0});
      const auto upper=plan_rect(.25f,6,b+Vec2{sign*4.5f,0});
      Emit core(&sc.opaque,M_CONCRETE_DARK);
      core.polygon(lower,low,false);core.polygon(upper,high,true);
      for(std::size_t i=0;i<lower.size();++i) {
        const auto j=(i+1)%lower.size();
        core.quad_metric(P3(lower[j],low),P3(lower[i],low),P3(upper[i],high),P3(upper[j],high));
      }
    }
  };
  cores(kDeck,13.2f);
  for(int floor=0;floor<floors;++floor) {
    float y=13.2f+floor*floor_h,top=y+floor_h;
    const auto lower=foreground_slab_plan(y);
    slab(sc.opaque,lower,y,.20f,spandrel);
    sc.roof_obstructions.push_back({foreground_slab_plan(y,.18f),y-.20f,top+.02f});
    for(int panel=0;panel<panels;++panel) {
      float a=panel*2*kPi/panels,b=(panel+1)*2*kPi/panels;
      smooth_face(spandrel,a,b,y,y+.48f,distances[panel],distances[panel+1]);
      smooth_face(glazing,a,b,y+.48f,top-.19f,distances[panel],distances[panel+1]);
      smooth_face(spandrel,a,b,top-.19f,top,distances[panel],distances[panel+1]);
      Emit trim(&sc.opaque,panel%4==0?M_BRONZE:M_DARK_METAL);
      trim.beam(position(a,y+.46f),position(a,top-.16f),panel%4==0?.085f:.035f,.065f);
      if(floor%5==4)
        Emit(&sc.opaque,M_BRONZE).beam(position(a,top-.13f),position(b,top-.13f),.085f,.075f);
    }
    // Two staggered closed core walls and lift landings carry each floor.
    // They follow the continuous inclined shaft, rather than leaving an
    // unsupported stack of shifted floor plates above the podium.
    cores(y,top);
  }
  const auto crown=foreground_slab_plan(206.8f);
  slab(sc.opaque,crown,206.8f,.22f,spandrel);
  parapet(sc.opaque,inset_convex_plan(crown,.20f),206.8f,.65f,.15f,M_BRONZE);
  // A shaped occupied crown remains within the fixed207.45m envelope; no
  // unrelated tall lantern or flat oversized roof disc replaces its silhouette.
  for(float sign:{-1.f,1.f}) {
    Vec2 p=foreground_slab_centre(206.8f)+Vec2{sign*4,0};
    Emit(&sc.opaque,spandrel).box(P3(p,207.05f),{1.5f,.25f,2});
    Emit(&sc.opaque,M_BRONZE).box(P3(p,207.33f),{1.55f,.03f,2.05f});
  }
  sc.register_range(first,static_cast<std::uint32_t>(sc.opaque.indices.size()),{79,110,298},128);
  ++sc.stats_towers;++sc.stats_blocks;
}
void parcel(Scene& sc,Rng rng,const std::vector<Vec2>& land,int address) {
  Vec2 c=plan_centroid(land);float distance=length(c);
  if(reserved_plot(land))return;
  if(c.x<shore(c.y)+24||c.x>east_shore(c.y)-24||std::abs(plan_area(land))<450) return;
  if(address==253) {foreground_dark_slab(sc,rng,land);return;}
  auto start=std::uint32_t(sc.opaque.indices.size());
  auto footprint=plan_scale(land,.88f,c);
  // Pavements meet the arterial at grade. Only occupied building footprints
  // rise as podiums, so the city does not read as a catalogue of square trays.
  slab(sc.opaque,plan_scale(land,.985f,c),kDeck,.10f,address%3?M_SIDEWALK:M_TERRAZZO);
  Vec2 lo,hi;plan_bounds(footprint,&lo,&hi);float hx=(hi.x-lo.x)*.5f,hz=(hi.y-lo.y)*.5f;
  float rot=std::atan2(land[1].y-land[0].y,land[1].x-land[0].x);
  float roof=kDeck;
  // Each frontage follows its actual rectangular cadastral boundary. The
  // previous axis-aligned decorative wings protruded across rotated streets.
  const int podium=1,podium_floors=2+(address/3)%3;
  sustained_frontage(sc,rng.child(30),footprint,kDeck,podium_floors,address,false);
  roof=kDeck+podium_floors*4;
  sc.register_range(start,std::uint32_t(sc.opaque.indices.size()),P3(c,20),std::max(hx,hz)*1.6f+30);
  float cluster=std::exp(-std::pow(length(c-Vec2{150,-340})/600,2.f))+.8f*std::exp(-std::pow(length(c-Vec2{720,-1600})/500,2.f));
  bool tall=(address%4!=0&&cluster>.2f)||(address%7==0)||(distance<520&&address%3!=0);
  // A low civic approach preserves a legible dome/ring axis across the city.
  // This is an authored boulevard constraint shared by every camera.
  Vec2 approach_start{-292.481f,591.547f},approach_end=kDome+kDomeMove;
  Vec2 along=approach_end-approach_start;float t=dot(c-approach_start,along)/dot(along,along);
  if(t>0&&t<1&&length(c-(approach_start+along*t))<65) tall=false;
  if(tall) {
    Rng mass=rng.child(200);int floors=int(mass.range(18,39)+cluster*mass.range(13,34));
    if(const int authored=rear_middle_floors(address);authored>0)floors=authored;
    float radius=std::min(std::min(hx,hz)*mass.range(.36f,.52f),plan_inradius(footprint)*.58f);
    if(podium==0||podium==3) radius*=.76f;
    Vec2 at=c+(podium==0?Vec2{hx*.45f,0}:Vec2{0,0});
    auto spec=family(address,floors,radius,rot+mass.range(-.3f,.3f));
    // These canonical parcels supply distinct ivory exoskeleton silhouettes
    // among the bronze fins and dark curtain walls. Their occupied plans,
    // heights, setbacks and crowns remain those of their original buildings.
    if(address==478||address==475||address==398) {
      spec.facade=address==398?FacadeKind::Diagrid:FacadeKind::HexLattice;
      spec.member=M_WHITE_METAL;spec.member_r=.62f;spec.module_w=3.0f;
      spec.lattice_rows=address==398?2:3;spec.floor_bands=true;
      spec.glass=address==478?M_GLASS_BLUE:address==475?M_GLASS_DARK:M_GLASS_BRONZE;
    } else if(address==529) {
      spec.member=M_WHITE_METAL;spec.member_r=.55f;
    }
    // Inspection distance is evaluated against the complete canonical set,
    // so every preset sees the same physical facade construction.
    float inspect_distance=distance;
    for(const char* view:{"aerial","galaxy","civic","street","garden","landing"}) {
      Vec3 eye,target;shot_camera(view,eye,target);
      inspect_distance=std::min(inspect_distance,length(at-Vec2{eye.x,eye.z}));
    }
    int lod=inspect_distance<1400?2:1;
    // A stable parcel address owns the near-right architectural showcase.
    if(address==253)showcase_lattice_tower(sc,spec,at,roof,mass.child(1));
    else tower(sc,spec,at,roof,mass.child(1),lod);
    if(address%13==0&&distance<1400&&plan_inradius(footprint)>48) {
      auto partner=family(address+4,floors*3/4,radius*.59f,rot);
      Vec2 q=at+Vec2{hx*.62f,hz*.43f};tower(sc,partner,q,roof,mass.child(2),lod);
      bridge(sc,P3(at,roof+floors*1.6f),P3(q,roof+floors*1.6f),5,1);
    }
  }
  ++sc.stats_blocks;
}
struct CoastalPlot {
  std::size_t column,row;
  int address;
  std::vector<Vec2> footprint;
};
std::vector<float> coastal_rows(Rng) {
  std::vector<float> rows{920,812,704,596,488,380,272,150,72,-88,-208,-330};
  const std::array<float,5> depths{{108,120,96,132,112}};
  for(std::size_t i=0;rows.back()>-4100;++i)rows.push_back(rows.back()-depths[i%depths.size()]);
  return rows;
}
std::vector<CoastalPlot> coastal_plots(Rng root) {
  std::vector<CoastalPlot> plots;auto rows=coastal_rows(root);
  for(std::size_t col=0;col+1<kOffsets.size();++col) {
    for(std::size_t row=0;row+1<rows.size();++row) {
      bool merged=false;
      std::size_t endrow=row+(merged?2:1);
      float margin=avenue_width(col)*.5f+3;
      float right_margin=avenue_width(col+1)*.5f+3;
      float z0=rows[row]-cross_width(rows[row],row)*.5f-3;
      float z1=rows[endrow]+cross_width(rows[endrow],endrow)*.5f+3;
      auto a=street(z0,kOffsets[col]+margin),b=street(z0,kOffsets[col+1]-right_margin);
      auto c=street(z1,kOffsets[col+1]-right_margin),d=street(z1,kOffsets[col]+margin);
      std::vector<Vec2> lot{d,c,b,a};
      int address=int(row*37+col*17);
      // Only the actual natural coastline cuts rectangular surveyed blocks.
      // A convex strip clips both banks without offsetting individual corners.
      float lowz=lot.front().y,highz=lowz;
      for(Vec2 p:lot){lowz=std::min(lowz,p.y);highz=std::max(highz,p.y);}
      Vec2 west0{shore(lowz)+21,lowz},west1{shore(highz)+21,highz};
      Vec2 east0{east_shore(lowz)-21,lowz},east1{east_shore(highz)-21,highz};
      Vec2 wd=west1-west0,ed=east1-east0;
      lot=clip_halfplane(lot,west0,{wd.y,-wd.x});
      lot=clip_halfplane(lot,east0,{-ed.y,ed.x});
      if(lot.size()<3||std::abs(plan_area(lot))<450)continue;
      plots.push_back({col,row,address,std::move(lot)});
      if(merged)++row;
    }
  }
  return plots;
}
std::vector<std::vector<Vec2>> protected_footprints(bool include_canal=true) {
  std::vector<std::vector<Vec2>> result{plan_rect(80,67,kDome+kDomeMove)};
  for(const auto& reserved:transit_reserved_plans())result.push_back(reserved);
  for(const auto& corridor:arrival_primary_corridors())result.push_back(corridor);
  for(const auto& corridor:market_approach_footprints())result.push_back(corridor);
  for(const auto& r:std::array<std::array<float,4>,11>{{
      {{38,-48,150,137}},{{73,111,26,53}},{{43,144,9,69}},{{22,117,7,57}},
      {{224,149,56,33}},{{148,153,22,24}},{{kLattice.x,kLattice.y,72,78}},
      {{-125,490,47,41}},{{190,-300,42,43}},
      {{410,95,54,53}},{{458.8f,-360,82,77}}
    }})result.push_back(plan_rect(r[2],r[3],{r[0],r[1]}));
  result.push_back(plan_rect(47,41,{235,-12}));
  result.push_back(kWestCivicCourt);result.push_back(moved_plan(kWestMarketReservation,kMarketMove));result.push_back(moved_plan(kMarketPromenade,kMarketMove));
  result.push_back(moved_plan(kCanalHexParcel,kHexMove));result.push_back(moved_plan(kCanalHexAccess,kHexMove));result.push_back(kGardenCompanionParcel);
  // The square's entrance is a pedestrian forecourt. Reserving its actual
  // footprint trims the adjacent mixed-use block without dropping that block.
  result.push_back({{-95,116},{-21,116},{-32,89},{-103,89}});
  auto corridor=[&](Vec2 a,Vec2 b,float half) {
    Vec2 d=normalize(b-a),n{-d.y*half,d.x*half};
    result.push_back({a-n,b-n,b+n,a+n});
  };
  for(std::size_t i=0;i+1<kGardenApproach.size();++i)corridor(kGardenApproach[i],kGardenApproach[i+1],8);
  for(std::size_t i=0;i+1<kWestPublicRoute.size();++i)corridor(kWestPublicRoute[i],kWestPublicRoute[i+1],7);
  const std::array<Vec2,10> walking{{{-68,94},{-15,109},{-4,106.5f},{34,106.5f},{44,119},{44,194},{128,213},{kLandingEntryX,213},{kLandingEntryX,174},{200,166}}};
  for(std::size_t i=0;i+1<walking.size();++i)corridor(walking[i],walking[i+1],7);
  if(include_canal)for(int i=0;i<44;++i) {
    float za=-1350+i*60.f,zb=za+60;
    float a=canal_centre(za),b=canal_centre(zb),wa=canal_halfwidth(za)+15,wb=canal_halfwidth(zb)+15;
    result.push_back({{a-wa,za},{a+wa,za},{b+wb,zb},{b-wb,zb}});
  }
  return result;
}
std::vector<std::vector<Vec2>> subtract_convex(const std::vector<Vec2>& source,const std::vector<Vec2>& obstacle,float minimum_area=8) {
  if(!polygons_overlap(source,obstacle))return {source};
  std::vector<std::vector<Vec2>> outside;
  auto remaining=source;float orientation=plan_area(obstacle)>0?1.f:-1.f;
  for(std::size_t i=0;i<obstacle.size()&&remaining.size()>=3;++i) {
    Vec2 p=obstacle[i],d=obstacle[(i+1)%obstacle.size()]-p;
    Vec2 inside{-d.y*orientation,d.x*orientation};
    auto part=clip_halfplane(remaining,p,inside*-1.f);
    if(part.size()>=3&&std::abs(plan_area(part))>minimum_area)outside.push_back(std::move(part));
    remaining=clip_halfplane(remaining,p,inside);
  }
  return outside;
}
std::vector<Vec2> inset_convex_plan(const std::vector<Vec2>& source,float distance) {
  // Clipped canal parcels can contain edges shorter than the setback. Moving
  // their vertices along bisectors reverses those edges into a backtracking
  // loop; intersecting the original inward half-planes keeps the real parcel
  // boundary convex and makes every subsequent occupied-floor exclusion valid.
  auto result=source;
  const float winding=plan_area(source)>0?1.f:-1.f;
  for(std::size_t i=0;i<source.size()&&result.size()>=3;++i) {
    const Vec2 a=source[i],edge=source[(i+1)%source.size()]-a;
    if(length(edge)<.001f)continue;
    const Vec2 inward=normalize(Vec2{-edge.y,edge.x})*winding;
    result=clip_halfplane(result,a+inward*distance,inward);
  }
  // Consecutive clipping planes can retain coincident or collinear vertices.
  // Removing those does not alter the footprint, and prevents zero-length
  // normals in fascia and exclusion construction.
  bool changed=true;
  while(changed&&result.size()>=3) {
    changed=false;
    for(std::size_t i=0;i<result.size();++i) {
      const Vec2 a=result[(i+result.size()-1)%result.size()],b=result[i],c=result[(i+1)%result.size()];
      const Vec2 ab=b-a,bc=c-b;
      if(length(ab)<.002f||length(bc)<.002f||
          std::abs(ab.x*bc.y-ab.y*bc.x)<.001f*(length(ab)+length(bc))) {
        result.erase(result.begin()+static_cast<std::ptrdiff_t>(i));changed=true;break;
      }
    }
  }
  return result;
}
struct InfillPlot {
  int column,row,piece;
  std::vector<Vec2> footprint;
};
std::vector<InfillPlot> infill_plots(Rng rng) {
  auto protected_plans=protected_footprints();std::vector<InfillPlot> result;
  for(const auto& plot:coastal_plots(rng)) {
    Vec2 c=plan_centroid(plot.footprint);
    if(length(c)>1100||!reserved_plot(plot.footprint))continue;
    std::vector<std::vector<Vec2>> pieces{plan_scale(plot.footprint,.97f,c)};
    for(const auto& obstacle:protected_plans) {
      std::vector<std::vector<Vec2>> next;
      for(const auto& p:pieces) {
        auto fragments=subtract_convex(p,obstacle);
        for(auto& f:fragments)next.push_back(std::move(f));
      }
      pieces=std::move(next);if(pieces.empty())break;
    }
    int part=0;
    for(auto& p:pieces) {
      if(p.size()<3||std::abs(plan_area(p))<210||plan_inradius(p)<4.4f)continue;
      result.push_back({int(plot.column),int(plot.row),part++,std::move(p)});
    }
  }
  return result;
}
std::vector<CanalCrossing> canal_construction_crossings(Rng root) {
  std::vector<CanalCrossing> result;const auto rows=coastal_rows(root);
  auto segment=[&](Vec2 a,Vec2 b,float width) {
    if(std::max(a.y,b.y)<-550||std::min(a.y,b.y)>550)return;
    // Include the real approach segments and longitudinal junctions as well as
    // the three spanning decks. A bench must not obstruct a road that ends at
    // the quay merely because that particular road has no crossing deck.
    Vec2 middle=(a+b)*.5f;
    if(std::min({std::abs(a.x-canal_centre(a.y)),std::abs(b.x-canal_centre(b.y)),
                 std::abs(middle.x-canal_centre(middle.y))})>130+width)return;
    result.push_back({a,b,width});
  };
  for(std::size_t row=0;row<rows.size();++row)
    for(std::size_t col=0;col+1<kOffsets.size();++col)
      segment(street(rows[row],kOffsets[col]),street(rows[row],kOffsets[col+1]),cross_width(rows[row],row));
  for(std::size_t col=0;col+1<kOffsets.size();++col)
    for(std::size_t row=0;row+1<rows.size();++row)
      segment(street(rows[row],kOffsets[col]),street(rows[row+1],kOffsets[col]),avenue_width(col));
  return result;
}
std::vector<std::vector<Vec2>> canal_construction_exclusions(Rng root) {
  // The final 44 protected canal strips are intentionally absent here: those
  // strips reserve the very promenade that this construction helper finishes.
  auto result=protected_footprints(false);
  for(const auto& plot:coastal_plots(root))if(!reserved_plot(plot.footprint))result.push_back(plot.footprint);
  for(const auto& plot:infill_plots(root))result.push_back(plot.footprint);
  return result;
}

void occupied_infill(Scene& sc,Rng rng,Rng district_rng) {
  for(const auto& plot:infill_plots(district_rng)) {
    Rng r=rng.child(plot.column,plot.row,plot.piece);
    auto first=std::uint32_t(sc.opaque.indices.size());
    auto shape=inset_convex_plan(plot.footprint,1.7f);if(shape.size()<3)continue;
    Vec2 c=plan_centroid(shape);float area=std::abs(plan_area(shape));
    const int identity=plot.column*31+plot.row*17+plot.piece*11;
    int floors=(area<600?2:3)+r.irange(0,2);
    if(area>2500&&identity%7==0)floors=6;
    slab(sc.opaque,plot.footprint,kDeck,.12f,M_SIDEWALK);
    std::vector<std::vector<Vec2>> wings{shape};
    if(area>1800&&identity%3==0) {
      // Courtyard blocks have a real void and an open passage from the public
      // frontage. Their separate occupied wings keep an uneven roofline.
      Vec2 axis=plan_long_axis(shape),across{-axis.y,axis.x};
      float lo,hi;plan_extent(shape,axis,&lo,&hi);
      float low,high;plan_extent(shape,across,&low,&high);
      float hx=(hi-lo)*.24f,hz=(high-low)*.24f;
      std::vector<Vec2> court{c-axis*hx-across*hz,c+axis*hx-across*hz,
                              c+axis*hx+across*hz,c-axis*hx+across*hz};
      wings=subtract_convex(shape,court,60);
      const std::vector<Vec2> entrance{c-across*2.2f,c+axis*(hi-lo)+across*-2.2f,
                                     c+axis*(hi-lo)+across*2.2f,c+across*2.2f};
      std::vector<std::vector<Vec2>> opened;
      for(const auto& wing:wings)for(auto part:subtract_convex(wing,entrance,60))
        if(plan_inradius(part)>2.8f)opened.push_back(std::move(part));
      wings=std::move(opened);
      garden(sc,r.child(80),c,hx*.48f,hz*.48f,kDeck,true,true);
    } else if(area>1700&&identity%3==1) {
      // Two unequal frontage houses share a planted lane, rather than one
      // monolithic seven-storey slab filling the whole irregular parcel.
      Vec2 axis=plan_long_axis(shape);float cut=(identity%5-2)*1.2f;
      wings={clip_halfplane(shape,c+axis*(cut-2.2f),axis*-1.f),
             clip_halfplane(shape,c+axis*(cut+2.2f),axis)};
    }
    for(std::size_t wing=0;wing<wings.size();++wing) {
      const auto& part=wings[wing];if(part.size()<3||std::abs(plan_area(part))<75)continue;
      const int height=std::clamp(floors+int(wing%3)-1,2,6);
      if((plot.column==3||plot.column==2)&&plot.row==6) {
        const int street_floors=std::clamp(height,2,3);
        sustained_frontage(sc,r.child(1+wing),part,kDeck,street_floors,identity+int(wing)*7,
            (identity+int(wing))%3==0);
      } else low_building(sc,r.child(1+wing),part,plan_centroid(part),kDeck,height,identity+int(wing)*7,true);
    }
    Sampled frontage=plan_sample(shape,13);
    for(std::size_t i=0;i<frontage.points.size();i+=2) {
      Vec2 p=frontage.points[i],n=frontage.normals[i];float angle=std::atan2(n.x,n.y);
      if(!sc.asset_library.resources.empty()) {
        add_asset_instance(sc,"glass_door",P3(p+n*.15f,kDeck),angle,1.f);
        add_asset_instance(sc,"bronze_light",P3(p+n*.25f,kDeck+3.1f),angle,1.f);
      }
      if(i%4==0)lamp(sc,P3(p+n*1.3f,kDeck),3.8f);
    }
    sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(c,kDeck+floors*2),std::sqrt(area)+floors*2);
  }
}
void distant_occupied_mass(Scene& sc,const std::vector<Vec2>& plan,int floors,int identity,Mat glass,Rng rng) {
  const Vec2 centre=plan_centroid(plan);
  auto footprint=plan;
  const Mat roof_material=roof_finish(sc,identity);
  const Mat frame=identity%3==0?M_BRONZE:M_DARK_METAL;
  for(int floor=0;floor<floors;++floor) {
    // Real occupied floors and a few substantial setbacks preserve the city
    // grain. A full-height plain quad would read as a featureless pale bar.
    if(floor>1&&floor%std::max(3,floors/3)==0)
      footprint=plan_scale(footprint,identity%3==0?.84f:.93f,centre);
    const float y=kDeck+floor*4.f;
    if(arrival_blockout) {Emit(&sc.opaque,glass).wall(footprint,y,y+4,true);slab(sc.opaque,footprint,y+4,.15f,roof_material);continue;}
    Emit glazing(&sc.opaque,glass);glazing.element_random=rng.child(floor).next();
    glazing.wall(plan_offset(footprint,-.22f),y+.14f,y+3.84f,true);
    Emit(&sc.opaque,frame).wall(footprint,y+3.82f,y+4.f,true);
    Emit(&sc.opaque,roof_material).polygon(footprint,y+4.008f,true);
    if(floor%3==0) {
      const Sampled bays=plan_sample(footprint,4.8f);
      for(std::size_t i=0;i<bays.points.size();i+=2) {
        const Vec2 normal=bays.normals[i],along{-normal.y,normal.x};
        Emit(&sc.opaque,frame).box(P3(bays.points[i],y+1.95f),{.08f,1.9f,.16f},
            {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
      }
    }
  }
  const float top=kDeck+floors*4.f;
  parapet(sc.opaque,footprint,top,.65f,.17f,frame);
  const Vec2 axis=plan_long_axis(footprint),cross_axis{-axis.y,axis.x};
  float lo,hi;plan_extent(footprint,axis,&lo,&hi);
  const float width=(hi-lo)*.16f;
  if(identity%4==0) {
    // The rooflight belongs to an occupied wintergarden on the intact roof.
    auto pavilion=plan_transform(plan_rounded_rect(width,2.8f,1.1f,4),centre,std::atan2(axis.y,axis.x));
    Emit(&sc.opaque,glass).wall(pavilion,top,top+3.1f,true);
    slab(sc.opaque,pavilion,top+3.22f,.16f,M_BRONZE);
  } else {
    for(int i=0;i<2+identity%3;++i) {
      Vec2 q=centre+axis*((i-.5f)*3.2f)+cross_axis*1.8f;
      box(sc,M_DARK_METAL,P3(q,top+.65f),{1.1f,.65f,1.8f});
      box(sc,M_BRONZE,P3(q,top+1.33f),{1.18f,.06f,1.88f});
    }
  }
  ++sc.stats_standards;
}
struct NorthernPlot {
  int row,column,address,floors;
  Vec2 centre;
  std::vector<Vec2> footprint;
};
std::vector<NorthernPlot> northern_plots(Rng root) {
  std::vector<NorthernPlot> plots;
  for(int row=0;row<13;++row)for(int column=0;column<77;++column) {
    Rng r=root.child(81,row,column);
    Vec2 p{-3110+column*65.f+r.range(-6,6),-3940-row*115.f+r.range(-9,9)};
    int address=row*97+column*13;
    float cluster=std::exp(-std::pow(length(p-Vec2{-820,-4710})/510.f,2.f));
    int floors=3+r.irange(0,7)+int(cluster*r.range(4,14));
    auto shape=plan_transform(plan_rounded_rect(r.range(20,27),r.range(26,38),3.2f,3),p,r.range(-.17f,.17f));
    bool tower_clear=true;
    for(int i=0;i<76;++i) {
      Rng tower_rng=root.child(80,i);
      Vec2 tower_at{-2800+i*57.f+tower_rng.range(-12,12),-4050-tower_rng.range(0,360)};
      if(polygons_overlap(shape,plan_circle(29,20,tower_at))){tower_clear=false;break;}
    }
    if(!tower_clear)continue;
    plots.push_back({row,column,address,floors,p,std::move(shape)});
  }
  // Independent rotation/width jitter can make neighboring corner envelopes
  // overlap even when their centres are well separated. Retain both addresses
  // and carve a genuine shared lane at the four affected parcel boundaries.
  for(std::size_t i=0;i<plots.size();++i)for(std::size_t j=i+1;j<plots.size();++j) {
    if(length(plots[i].centre-plots[j].centre)>100)continue;
    if(!polygons_overlap(plots[i].footprint,plots[j].footprint))continue;
    const Vec2 direction=normalize(plots[j].centre-plots[i].centre);
    const Vec2 middle=(plots[i].centre+plots[j].centre)*.5f;
    plots[i].footprint=clip_halfplane(plots[i].footprint,middle-direction*2,direction*-1.f);
    plots[j].footprint=clip_halfplane(plots[j].footprint,middle+direction*2,direction);
  }
  return plots;
}
void districts(Scene& sc,Rng root) {
  // Independently sized ribbons follow a terrain-driven arterial network.
  auto rows=coastal_rows(root);
  for(std::size_t col=0;col+1<kOffsets.size();++col) {
    std::vector<Vec2> avenue;
    for(float z:rows) avenue.push_back(street(z,kOffsets[col]));
    road(sc,avenue,avenue_width(col),col<7,col%4==0);
  }
  for(const auto& p:coastal_plots(root))
    parcel(sc,root.child(20,p.column,p.row),p.footprint,p.address);
  for(std::size_t r=0;r<rows.size();++r) {
    std::vector<Vec2> cross;
    for(float offset:kOffsets)cross.push_back(street(rows[r],offset));
    road(sc,cross,cross_width(rows[r],r),false,cross_spans_canal(rows[r],r));
  }
  // Far shore has its own skyline rhythm, separate from the near peninsula.
  for(int i=0;i<76;++i) {
    Rng r=root.child(80,i);Vec2 p{-2800+i*57.f+r.range(-12,12),-4050-r.range(0,360)};
    auto s=family(i,int(r.range(15,60)),r.range(15,25),r.range(-.6f,.6f));
    tower(sc,s,p,kDeck,r.child(1),1);
  }
  // Continuous lower neighbourhoods occupy the headland between its skyline
  // clusters. Their shorter streets and roof steps carry detail to the horizon.
  for(const auto& plot:northern_plots(root)) {
    distant_occupied_mass(sc,plot.footprint,plot.floors,plot.address,
        plot.address%3==0?M_GLASS_BRONZE:M_GLASS_DARK,root.child(81,plot.row,plot.column).child(1));
    ++sc.stats_blocks;
  }
}
float southern_coast(float x) {
  // The peninsula meets a mainland around a wide, open eastern inlet. The
  // concave urban shore remains a physical landmark from either wide camera.
  return 2520+620*std::sin((x-3500)/2200.f)+180*std::sin(x/760.f);
}
struct SouthernPlot {
  int district,row,column,address,floors;
  Vec2 centre;
  std::vector<Vec2> footprint;
};
std::vector<SouthernPlot> southern_plots(Rng rng) {
  std::vector<SouthernPlot> result;
  // Small irregular blocks continue the existing peninsula without changing
  // any of the northern parcel addresses or hero reservations.
  for(int row=0;row<29;++row) {
    float z=1000+row*77.f;
    for(int col=0;col<21;++col) {
      Rng r=rng.child(1,row,col);float x=shore(z)+47+col*64+r.range(-3,3);
      Vec2 p{x,z+r.range(-4,4)};
      if(x>east_shore(p.y)-46||p.y>southern_coast(x)-45)continue;
      int id=row*29+col*13;
      float cluster=std::exp(-std::pow(length(p-Vec2{550,1640})/450,2.f));
      int floors=int(r.range(5,17)+cluster*r.range(12,31));
      auto plan=plan_transform(plan_rounded_rect(r.range(17,22),r.range(19,28),3,3),p,r.range(-.16f,.16f));
      result.push_back({0,row,col,id,floors,p,std::move(plan)});
    }
  }
  // Four urban bands create a depth gradient across the far shore. Coherent
  // neighbourhood centres raise groups of buildings, not isolated random bars.
  std::vector<float> frontages{-4200},depths{70};
  for(int col=0;col<116;++col)frontages.push_back(frontages.back()+rng.child(30,col).range(99,123));
  for(int row=0;row<27;++row)depths.push_back(depths.back()+rng.child(31,row).range(98,147));
  for(int row=0;row<28;++row) for(int col=0;col<117;++col) {
    Rng r=rng.child(2,row,col);
    float x=frontages[col]+46*std::sin(row*.43f)+r.range(-4,4);
    float z=southern_coast(x)+depths[row]+std::min(1.f,row/3.f)*110*std::sin(x/820+row*.07f)+r.range(-4,4);
    Vec2 p{x,z};int id=2000+row*131+col*17;
    if(row<2&&std::abs(x-3300)<70)continue;
    float cluster=0;
    for(Vec2 centre:std::array<Vec2,4>{{{1300,2880},{2950,2680},{6200,3420},{-1550,2660}}})
      cluster=std::max(cluster,std::exp(-std::pow(length(p-centre)/470,2.f)));
    int floors=int(r.range(4,12)+cluster*r.range(28,77));
    float hx=r.range(22,34),hz=r.range(20,35);
    std::vector<Vec2> plan;
    if(id%5==0)plan=plan_superellipse(hx,hz,2.4f,12,p,r.range(-.4f,.4f));
    else plan=plan_transform(plan_rounded_rect(hx,hz,id%3==0?7.f:2.f,2),p,r.range(-.2f,.2f));
    result.push_back({1,row,col,id,floors,p,std::move(plan)});
  }
  return result;
}
struct NeighborhoodStreet { Vec2 a,b;float half_width;bool local; };
struct NeighborhoodMass { std::vector<Vec2> footprint;int floors;std::string program; };
struct NeighborhoodParcel {
  int row,column,address;Vec2 centre;
  std::string program;std::vector<Vec2> envelope;std::vector<NeighborhoodMass> buildings;
  int district{1};
};
struct SouthernNeighborhood {
  std::vector<NeighborhoodStreet> streets;
  std::vector<NeighborhoodParcel> parcels;
  bool joined_properties{false};
};
bool nearshore_selection(const SouthernPlot& p) {
  return p.district==1&&p.row>=0&&p.row<=5&&p.column>=26&&p.column<=38;
}
SouthernNeighborhood southern_neighborhood(const std::vector<SouthernPlot>& plots) {
  SouthernNeighborhood result;
  std::array<std::array<const SouthernPlot*,117>,28> grid{};
  for(const auto& p:plots)if(p.district==1)grid[p.row][p.column]=&p;
  auto at=[&](int r,int c)->const SouthernPlot* {
    return r>=0&&r<28&&c>=0&&c<117?grid[r][c]:nullptr;
  };
  auto midpoint=[&](int r,int c,int rr,int cc) {return (at(r,c)->centre+at(rr,cc)->centre)*.5f;};
  auto road=[&](Vec2 a,Vec2 b,float half,bool local){result.streets.push_back({a,b,half,local});};
  for(int c:{28,34,31,37}) {
    const bool local=c==31||c==37;const int rows=local?7:8;
    std::vector<Vec2> line;
    for(int r=0;r<rows;++r)if(at(r,c)&&at(r,c+1))line.push_back(midpoint(r,c,r,c+1));
    if(!line.empty())line.insert(line.begin(),{line.front().x,southern_coast(line.front().x)+29});
    for(std::size_t i=1;i<line.size();++i)road(line[i-1],line[i],local?5.f:9.5f,local);
  }
  for(int r:{3,1,5})for(int c=r==3?24:26;c<(r==3?40:38);++c)
    if(at(r,c)&&at(r+1,c)&&at(r,c+1)&&at(r+1,c+1))
      road(midpoint(r,c,r+1,c),midpoint(r,c+1,r+1,c+1),r==3?9.5f:5.f,r!=3);
  for(int i=35;i<58;++i) {
    const float x=-4400+i*80.f;
    road({x,southern_coast(x)+29},{x+80,southern_coast(x+80)+29},8.5f,false);
  }
  auto rect=[](float x0,float x1,float z0,float z1) {
    return std::vector<Vec2>{{x0,z0},{x1,z0},{x1,z1},{x0,z1}};
  };
  auto intersect=[](std::vector<Vec2> p,const std::vector<Vec2>& boundary) {
    for(std::size_t i=0;i<boundary.size()&&p.size()>2;++i) {
      const Vec2 a=boundary[i],d=boundary[(i+1)%boundary.size()]-a;
      p=clip_halfplane(p,a,{-d.y,d.x});
    }
    return p;
  };
  auto crest=[](int r,int c) {
    if(r==3&&c==28)return 31;
    if(r==4&&c==28)return 22;
    if(r==3&&c==29)return 24;
    if(r==4&&c==29)return 16;
    if(r==2&&c==35)return 22;
    if(r==3&&c==35)return 29;
    if(r==2&&c==36)return 18;
    if(r==3&&c==36)return 15;
    return 0;
  };
  for(int r=0;r<6;++r)for(int c=26;c<39;++c) {
    const auto* old=at(r,c);if(!old)continue;
    const Vec2 p=old->centre;auto cell=rect(p.x-100,p.x+100,p.y-110,p.y+110);
    for(int dr=-1;dr<=1;++dr)for(int dc=-1;dc<=1;++dc) {
      if((!dr&&!dc)||!at(r+dr,c+dc))continue;
      const Vec2 q=at(r+dr,c+dc)->centre,n=normalize(p-q);float margin=2;
      if(!dr) {
        const int boundary=std::min(c,c+dc);
        if(boundary==28||boundary==34)margin=13;
        else if(boundary==31||boundary==37)margin=8;
      }
      if(!dc) {
        const int boundary=std::min(r,r+dr);
        if(boundary==3)margin=13;else if(boundary==1||boundary==5)margin=8;
      }
      cell=clip_halfplane(cell,(p+q)*.5f+n*margin,n);
    }
    for(float offset:{-80.f,0.f,80.f}) {
      const float x=p.x+offset;
      const Vec2 a{x-40,southern_coast(x-40)+42},b{x+40,southern_coast(x+40)+42},d=b-a;
      cell=clip_halfplane(cell,a,{-d.y,d.x});
    }
    for(const auto& street:result.streets) {
      Vec2 d=normalize(street.b-street.a),n{-d.y,d.x};
      const std::vector<Vec2> corridor{street.a-n*street.half_width,street.b-n*street.half_width,
          street.b+n*street.half_width,street.a+n*street.half_width};
      if(!polygons_overlap(cell,corridor))continue;
      if(dot(p-street.a,n)<0)n=n*-1.f;
      cell=clip_halfplane(cell,street.a+n*(street.half_width+2),n);
    }
    if(cell.size()<3||std::abs(plan_area(cell))<1000)continue;
    const int tower_floors=crest(r,c);
    std::string program=old->floors>=22&&!tower_floors?"retained_cluster":
        tower_floors||r==2||r==3?"court":r<2?"terrace":"work_court";
    if((r==1&&c==29)||(r==0&&c==35))program="court";
    if(r==2&&c==30)program="work_court";
    if(r==4&&c==32)program="terrace";
    if((r==3&&c==33)||(r==1&&c==34))program="public_court";
    NeighborhoodParcel block{r,c,old->address,p,program,cell,{}};
    const Vec2 axis=normalize(at(r,c+1)->centre-at(r,c-1)->centre),across{-axis.y,axis.x};
    auto local=[&](Vec2 q){return Vec2{dot(q-p,axis),dot(q-p,across)};};
    auto world=[&](Vec2 q){return p+axis*q.x+across*q.y;};
    std::vector<Vec2> local_cell;for(Vec2 q:cell)local_cell.push_back(local(q));
    Vec2 lo,hi;plan_bounds(local_cell,&lo,&hi);const float width=hi.x-lo.x,depth=hi.y-lo.y;
    auto mass=[&](std::vector<Vec2> shape,int floors,const char* use,bool world_plan=false) {
      if(!world_plan)for(Vec2& q:shape)q=world(q);
      shape=intersect(std::move(shape),cell);
      if(shape.size()<3||std::abs(plan_area(shape))<=50)return;
      float caliper=1e9f;
      for(std::size_t i=0;i<shape.size();++i) {
        Vec2 d=shape[(i+1)%shape.size()]-shape[i];if(length(d)<.0003f)continue;
        Vec2 n=normalize(Vec2{-d.y,d.x});float a,b;plan_extent(shape,n,&a,&b);caliper=std::min(caliper,b-a);
      }
      if(caliper>=5.999f)block.buildings.push_back({std::move(shape),floors,use});
    };
    if(program=="retained_cluster")mass(old->footprint,old->floors,"retained_cluster",true);
    else if(program=="court") {
      auto parts=subtract_convex(local_cell,rect(-width*.24f,width*.24f,-depth*.23f,depth*.23f),.01f);
      const auto entry=r%2==0?rect(-3,3,lo.y-1,0):rect(-3,3,0,hi.y+1);
      int wing=0;for(const auto& part:parts)for(const auto& q:subtract_convex(part,entry,.01f))
        mass(q,std::array<int,4>{{4,6,5,4}}[wing++%4],"sustained_court_wing");
      if(tower_floors)mass(rect(-9,9,-12,12),tower_floors,"junction_tower");
    } else if(program=="terrace") {
      for(int side=0;side<2;++side) {
        const float z0=side?hi.y-22:lo.y,z1=side?hi.y:lo.y+22;
        const int count=std::clamp(int(std::round(width/19)),3,6);
        const std::array<float,6> weights{{1.05f,.87f,1.14f,.94f,1.02f,1.08f}};
        float total=0;for(int i=0;i<count;++i)total+=weights[i];float x=lo.x;
        for(int i=0;i<count;++i) {
          const float next=x+width*weights[i]/total;
          mass(rect(x,next,z0,z1),std::array<int,6>{{4,6,5,4,5,6}}[(i+side+r)%6],"attached_street_house");x=next;
        }
      }
      if(c%3==0)mass(rect(hi.x-13,hi.x,lo.y+22,hi.y-22),3,"court_return");
    } else if(program=="public_court") {
      mass(rect(lo.x+3,hi.x-3,lo.y+3,lo.y+24),2,"neighborhood_civic_hall");
      mass(rect(lo.x+3,lo.x+19,lo.y+24,hi.y-5),3,"community_frontage");
    } else {
      mass(rect(lo.x,hi.x,lo.y,lo.y+22),6+c%3,"work_frontage");
      mass(rect(lo.x,-3,lo.y+22,hi.y-3),3,"work_hall");
    }
    result.parcels.push_back(std::move(block));
  }
  return result;
}
bool urban_redevelopment_cohort(const SouthernPlot& p) {
  return p.district==0||(p.district==1&&p.row<=9&&p.column>=46&&p.column<=75);
}
SouthernNeighborhood connected_shore_neighborhood(const std::vector<SouthernPlot>& plots,int district) {
  SouthernNeighborhood result;
  result.joined_properties=true;
  std::array<std::array<const SouthernPlot*,117>,29> grid{};
  for(const auto& p:plots)if(p.district==district)grid[p.row][p.column]=&p;
  auto at=[&](int row,int column)->const SouthernPlot* {
    return row>=0&&row<29&&column>=0&&column<117?grid[row][column]:nullptr;
  };
  auto mid=[&](int r,int c,int rr,int cc){return (at(r,c)->centre+at(rr,cc)->centre)*.5f;};
  auto add_road=[&](Vec2 a,Vec2 b,float half,bool local=false) {
    if(length(b-a)>.01f)result.streets.push_back({a,b,half,local});
  };
  if(district==0) {
    // These are the existing curved peninsula streets, including their full
    // curbs and two-metre frontage walks. Their geometry is not regenerated.
    for(int row=0;row<30;++row) {
      float z=950+row*77.f;
      add_road({shore(z)+19,z},{east_shore(z)-19,z},7.2f);
    }
    for(int col=0;col<22;++col)for(int row=0;row<29;++row) {
      float z=950+row*77.f,next=z+77;
      add_road({shore(z)+16+col*64,z},{shore(next)+16+col*64,next},
               (col%5==0?17.f:8.f)*.5f+2.2f);
    }
  } else {
    // Existing coast boulevard and the six-property arterial network remain.
    for(int i=0;i<168;++i) {
      float x=-4400+i*80.f;
      add_road({x,southern_coast(x)+29},{x+80,southern_coast(x+80)+29},11.5f);
    }
    for(int col=4;col<116;col+=6) {
      std::vector<Vec2> line;
      for(int row=0;row<28;++row)if(at(row,col)&&at(row,col+1))line.push_back(mid(row,col,row,col+1));
      if(!line.empty())line.insert(line.begin(),{line.front().x,southern_coast(line.front().x)+29});
      for(std::size_t i=1;i<line.size();++i)add_road(line[i-1],line[i],11.5f);
    }
    for(int row=3;row<27;row+=6)for(int col=0;col<116;++col)
      if(at(row,col)&&at(row+1,col)&&at(row,col+1)&&at(row+1,col+1))
        add_road(mid(row,col,row+1,col),mid(row,col+1,row+1,col+1),11.5f);
    // Three-property pedestrian lanes break the deep private blocks. These
    // are supported streets, not unpaved gaps between independent trays.
    for(int col=49;col<75;col+=6) {
      if(at(0,col)&&at(0,col+1)) {
        const Vec2 head=mid(0,col,0,col+1);
        add_road({head.x,southern_coast(head.x)+29},head,6,true);
      }
      for(int row=0;row<10;++row)
        if(at(row,col)&&at(row,col+1)&&at(row+1,col)&&at(row+1,col+1))
          add_road(mid(row,col,row,col+1),mid(row+1,col,row+1,col+1),6,true);
    }
  }
  auto rect=[](float x0,float x1,float z0,float z1) {
    return std::vector<Vec2>{{x0,z0},{x1,z0},{x1,z1},{x0,z1}};
  };
  for(const auto& old:plots) {
    if(old.district!=district||!urban_redevelopment_cohort(old)||old.floors>=30)continue;
    const int row=old.row,col=old.column;const Vec2 p=old.centre;
    const float reach=district?150.f:90.f;
    auto cell=rect(p.x-reach,p.x+reach,p.y-reach,p.y+reach);
    for(const auto& neighbor:plots) {
      if(neighbor.district!=district||(neighbor.row==row&&neighbor.column==col))continue;
      // A missing bridge-approach property can make a second-row neighbor
      // geometrically adjacent. Visit every centre whose bisector can reach
      // the initial square, rather than assuming a complete3x3 index grid.
      if(length(neighbor.centre-p)>reach*2.83f)continue;
      Vec2 q=neighbor.centre,n=normalize(p-q);
      // Adjacent private properties share a real party-wall boundary; road
      // reservations below create the actual public widths independently.
      cell=clip_halfplane(cell,(p+q)*.5f,n);
    }
    if(district==0) {
      for(float offset:{-70.f,0.f,70.f}) {
        float z=p.y+offset;
        Vec2 a{shore(z-35)+21,z-35},b{shore(z+35)+21,z+35},d=b-a;
        cell=clip_halfplane(cell,a,{d.y,-d.x});
        a={east_shore(z-35)-21,z-35};b={east_shore(z+35)-21,z+35};d=b-a;
        cell=clip_halfplane(cell,a,{-d.y,d.x});
      }
    }
    for(float offset:{-80.f,0.f,80.f}) {
      float x=p.x+offset;Vec2 a{x-40,southern_coast(x-40)+(district?43.f:-30.f)};
      Vec2 b{x+40,southern_coast(x+40)+(district?43.f:-30.f)},d=b-a;
      cell=clip_halfplane(cell,a,district?Vec2{-d.y,d.x}:Vec2{d.y,-d.x});
    }
    for(const auto& road:result.streets) {
      Vec2 delta=road.b-road.a,n=normalize(Vec2{-delta.y,delta.x});
      const auto corridor=std::vector<Vec2>{road.a-n*road.half_width,road.b-n*road.half_width,
          road.b+n*road.half_width,road.a+n*road.half_width};
      if(!polygons_overlap(cell,corridor))continue;
      if(dot(p-road.a,n)<0)n=n*-1.f;
      cell=clip_halfplane(cell,road.a+n*road.half_width,n);
    }
    // A bisector or street plane can coincide with an existing corner. Keep
    // its geometric boundary while removing repeated/collinear vertices
    // before the footprint is used for occupied slabs and parapet returns.
    cell=inset_convex_plan(cell,0);
    if(cell.size()<3||std::abs(plan_area(cell))<250||plan_inradius(cell)<5)continue;
    Vec2 axis{1,0};
    if(at(row,col-1)&&at(row,col+1))axis=normalize(at(row,col+1)->centre-at(row,col-1)->centre);
    const Vec2 across{-axis.y,axis.x};
    auto world=[&](Vec2 q){return p+axis*q.x+across*q.y;};
    std::vector<Vec2> local;for(Vec2 q:cell)local.push_back({dot(q-p,axis),dot(q-p,across)});
    Vec2 lo,hi;plan_bounds(local,&lo,&hi);float width=hi.x-lo.x,depth=hi.y-lo.y;
    const int program=(row/2+col/3)%5;
    NeighborhoodParcel block{row,col,old.address,p,
        std::array<const char*,5>{{"joined_court","attached_frontage","work_quarter","asymmetric_court","civic_corner"}}[program],cell,{},district};
    auto mass=[&](std::vector<Vec2> shape,int floors,const char* use) {
      for(Vec2& q:shape)q=world(q);
      for(std::size_t i=0;i<cell.size()&&shape.size()>2;++i) {
        Vec2 a=cell[i],d=cell[(i+1)%cell.size()]-a;shape=clip_halfplane(shape,a,{-d.y,d.x});
      }
      shape=inset_convex_plan(shape,0);
      if(shape.size()<3||std::abs(plan_area(shape))<75||plan_inradius(shape)<2.5f)return;
      block.buildings.push_back({std::move(shape),floors,use});
    };
    const int floor_bias=district?0:1;
    if(program==0||program==3) {
      const float shift=program==3?width*.09f:-width*.035f;
      const auto void_plan=rect((lo.x+hi.x)*.5f-width*.23f+shift,(lo.x+hi.x)*.5f+width*.23f+shift,
                               lo.y+depth*.28f,lo.y+depth*.73f);
      const auto entrance=rect((lo.x+hi.x)*.5f-2.2f,(lo.x+hi.x)*.5f+2.2f,lo.y-1,lo.y+depth*.5f);
      int wing=0;
      for(const auto& shape:subtract_convex(local,void_plan,.01f))
        for(const auto& opened:subtract_convex(shape,entrance,.01f))
          mass(opened,std::array<int,4>{{4,6,5,7}}[(wing+++(program==3?1:0))%4]+floor_bias,"joined_occupied_court_wing");
    } else if(program==1) {
      for(int side=0;side<2;++side) {
        const float z0=side?hi.y-depth*.32f:lo.y,z1=side?hi.y:lo.y+depth*.32f;
        const int houses=std::clamp(int(std::round(width/20)),2,6);
        const std::array<float,6> weights{{1.13f,.83f,1.02f,1.17f,.95f,.90f}};
        float total=0;for(int i=0;i<houses;++i)total+=weights[i];float x=lo.x;
        for(int house=0;house<houses;++house) {
          float next=x+width*weights[house]/total;
          mass(rect(x,next,z0,z1),3+(house+side+row)%4+floor_bias,"attached_occupied_street_house");x=next;
        }
      }
      if(col%2==0)mass(rect(lo.x,lo.x+width*.24f,lo.y+depth*.32f,hi.y-depth*.32f),4+floor_bias,"occupied_court_return");
    } else if(program==2) {
      mass(rect(lo.x,hi.x,lo.y,lo.y+depth*.34f),7+floor_bias+(district?0:col%2),"substantial_work_frontage");
      mass(rect(lo.x,lo.x+width*.57f,lo.y+depth*.34f,hi.y),4+floor_bias,"deep_work_hall");
      mass(rect(lo.x+width*.57f,hi.x,hi.y-depth*.25f,hi.y),5+floor_bias,"occupied_work_return");
    } else {
      mass(rect(lo.x,hi.x,lo.y,lo.y+depth*.35f),4+floor_bias,"community_street_frontage");
      mass(rect(lo.x,lo.x+width*.32f,lo.y+depth*.35f,hi.y),6+floor_bias,"community_occupied_wing");
      if(row%3!=0)mass(rect(lo.x+width*.32f,hi.x,hi.y-depth*.25f,hi.y),3+floor_bias,"community_garden_return");
    }
    if(!block.buildings.empty())result.parcels.push_back(std::move(block));
  }
  return result;
}
bool neighborhood_replaces(const SouthernNeighborhood& neighborhood,const SouthernPlot& plot) {
  for(const auto& p:neighborhood.parcels)
    if(p.district==plot.district&&p.row==plot.row&&p.column==plot.column)return true;
  return false;
}
void build_nearshore_neighborhood(Scene& sc,const SouthernNeighborhood& neighborhood,
                                  const std::array<Mat,4>& glazing,Rng rng) {
  // Local streets fill the previous unused gaps. Their construction reaches
  // the existing land at .65m, and short approaches meet the coastal road.
  for(const auto& street:neighborhood.streets)if(street.local) {
    Vec2 direction=normalize(street.b-street.a),side{-direction.y,direction.x};
    Vec3 a=P3(street.a,kDeck-.055f),b=P3(street.b,kDeck-.055f);
    Emit(&sc.opaque,M_CONCRETE_DARK).beam(P3(street.a,.895f),P3(street.b,.895f),10,.49f);
    Emit(&sc.opaque,M_ASPHALT).beam(a,b,6,.11f);
    for(float sign:{-1.f,1.f}) {
      const Vec2 offset=side*(sign*4);
      Emit(&sc.opaque,M_SIDEWALK).beam(P3(street.a+offset,kDeck-.075f),P3(street.b+offset,kDeck-.075f),2,.15f);
      Emit(&sc.opaque,M_CONCRETE_DARK).beam(P3(street.a+side*(sign*3.03f),kDeck-.016f),
          P3(street.b+side*(sign*3.03f),kDeck-.016f),.06f,.025f);
    }
    const int intervals=std::max(1,int(std::ceil(length(street.b-street.a)/31.f)));
    for(int i=0;i<intervals;++i) {
      const Vec2 p=street.a+(street.b-street.a)*((i+.45f)/intervals)+side*4.25f;
      lamp(sc,P3(p,kDeck),4.6f);sc.lights.back().radius=20;sc.lights.back().intensity=5.2f;
    }
  }
  std::array<Mat,4> bodies{};
  for(int i=0;i<4;++i) {
    MaterialDesc material=sc.materials[M_CONCRETE_WHITE];
    material.name="nearshore mineral frontage "+std::to_string(i);
    material.base_color=std::array<Vec3,4>{{{.48f,.44f,.36f},{.34f,.42f,.43f},{.55f,.50f,.39f},{.31f,.32f,.30f}}}[i];
    material.roughness=.62f;material.normal_strength=.20f;
    bodies[i]=static_cast<Mat>(sc.materials.size());sc.materials.push_back(material);
  }
  for(const auto& block:neighborhood.parcels) {
    const auto first=static_cast<std::uint32_t>(sc.opaque.indices.size());
    Rng r=rng.child(block.row,block.column);
    struct Neighbor {const NeighborhoodMass* house;std::uint64_t owner;};
    std::vector<Neighbor> neighbors;
    for(const auto& nearby:neighborhood.parcels)if(length(nearby.centre-block.centre)<350)
      for(std::size_t index=0;index<nearby.buildings.size();++index)
        neighbors.push_back({&nearby.buildings[index],std::uint64_t(nearby.address)*64+index});
    slab(sc.opaque,block.envelope,kDeck,.55f,M_SIDEWALK);
    float maximum_height=0;
    for(std::size_t index=0;index<block.buildings.size();++index) {
      const auto& house=block.buildings[index];const auto& footprint=house.footprint;
      const int identity=block.address+int(index)*7;
      const Mat body=bodies[(block.column+int(index))%4],glass=glazing[(block.row+int(index))%4];
      const Mat frame=identity%3?M_DARK_METAL:M_BRONZE,roof=roof_finish(sc,identity);
      const float height=house.floors*4.f;maximum_height=std::max(maximum_height,height);
      // Partition each boundary at true neighboring property corners. Shared
      // party walls are opaque, emitted once, and end at the lower roof; upper
      // portions become real exposed facades instead of overlapping glazing.
      for(std::size_t edge=0;edge<footprint.size();++edge) {
        const Vec2 a=footprint[edge],b=footprint[(edge+1)%footprint.size()],d=b-a;
        const float distance=length(d);if(distance<.025f)continue;
        const Vec2 along=d*(1.f/distance),outward{along.y,-along.x};
        std::vector<float> splits{0,distance};
        for(const auto& other:neighbors)if(other.house!=&house)
          for(Vec2 q:other.house->footprint) {
            const float t=dot(q-a,along);
            if(std::abs(dot(q-a,outward))<.08f&&t>.025f&&t<distance-.025f)splits.push_back(t);
          }
        std::sort(splits.begin(),splits.end());
        splits.erase(std::unique(splits.begin(),splits.end(),[](float x,float y){return std::abs(x-y)<.025f;}),splits.end());
        for(std::size_t part=1;part<splits.size();++part) {
          Vec2 one=a+along*splits[part-1],two=a+along*splits[part],mid=(one+two)*.5f;
          const float span=length(two-one);const Neighbor* neighbor=nullptr;
          for(const auto& other:neighbors)if(other.house!=&house)
            if(point_in_polygon(other.house->footprint,mid+outward*.035f)) {neighbor=&other;break;}
          for(int floor=0;floor<house.floors;++floor) {
            const float y=kDeck+floor*4.f;
            if(neighbor&&floor<neighbor->house->floors) {
              if(std::uint64_t(block.address)*64+index<neighbor->owner)
                Emit(&sc.opaque,body).beam(P3(one,y+2),P3(two,y+2),.18f,4);
              continue;
            }
            Emit masonry(&sc.opaque,body),windows(&sc.opaque,glass),metal(&sc.opaque,frame);
            windows.element_random=r.child(int(index),floor).next();
            auto face=[&](Emit& emit,Vec2 aa,Vec2 bb,float bottom,float top) {
              emit.quad_metric(P3(bb,bottom),P3(aa,bottom),P3(aa,top),P3(bb,top));
            };
            face(masonry,one,two,y+3.37f,y+3.94f);
            if(floor)face(masonry,one,two,y,y+.72f);
            const int bays=std::max(1,int(std::ceil(span/(house.program=="work_hall"?5.8f:3.8f))));
            for(int bay=0;bay<bays;++bay) {
              Vec2 aa=one+(two-one)*(float(bay)/bays),bb=one+(two-one)*(float(bay+1)/bays);
              const bool entrance=!floor&&span>8&&bay==bays/2;
              face(windows,aa-outward*.34f,bb-outward*.34f,y+(entrance?3.04f:floor?.72f:.16f),y+3.37f);
              masonry.box(P3(aa-outward*.10f,y+1.7f),{bay%3==0?.19f:.11f,1.7f,.28f},
                  {along.x,0,along.y},{0,1,0},{outward.x,0,outward.y});
              if(entrance) {
                metal.box(P3((aa+bb)*.5f,y+3.04f),{span/bays*.5f,.065f,.32f},
                    {along.x,0,along.y},{0,1,0},{outward.x,0,outward.y});
                Emit(&sc.opaque,M_LOBBY_LIGHT).beam(P3(aa-outward*.40f,y+3.02f),P3(bb-outward*.40f,y+3.02f),.08f,.035f);
              }
            }
          }
        }
      }
      for(int floor=0;floor<house.floors;++floor) {
        const float top=kDeck+(floor+1)*4;
        slab(sc.opaque,footprint,top,.18f,body);
      }
      const float top=kDeck+height+.008f;
      Emit(&sc.opaque,roof).polygon(footprint,top,true);
      if(neighborhood.joined_properties) {
        // At a clipped property's acute corner, a vertex-miter offset can
        // reverse its short inner edge. Build the actual inward half-plane
        // intersection and its closed coping instead of pairing those edges.
        const auto inner=inset_convex_plan(footprint,.14f);
        Emit coping(&sc.opaque,body);
        coping.wall(footprint,top,top+.62f,true,true);
        coping.wall(inner,top,top+.62f,true,false);
        for(const auto& strip:subtract_convex(footprint,inner,0)) {
          const auto cap=inset_convex_plan(strip,0);
          if(cap.size()>2)coping.polygon(cap,top+.62f,true);
        }
      } else parapet(sc.opaque,footprint,top,.62f,.14f,body);
      // Roof occupation varies by real property program. Taller roofs retain
      // screened plant; some lower houses have roof gardens and wintergardens.
      if(house.floors<=8&&identity%3==0) {
        sc.authored_roofs.push_back({footprint,top,identity,r.child(400+int(index))});
      } else {
        const auto inset=inset_convex_plan(footprint,2.1f);
        if(inset.size()>2&&plan_inradius(inset)>2.5f)roof_equipment(sc.opaque,r,inset,top,identity%3+1);
      }
      sc.roof_obstructions.push_back({footprint,kDeck,top-.008f});
      ++sc.stats_standards;
    }
    // Courts are public ground, not empty leftover grass. Selected beds are
    // tested against the complete collection of occupied wings and door axes.
    if(block.program!="retained_cluster")for(int side:{-1,1}) {
      const Vec2 p=block.centre+Vec2{side*13.f,0};
      const auto bed=plan_rect(4.5f,6.f,p);bool clear=true;
      for(Vec2 q:bed)if(!point_in_polygon(block.envelope,q))clear=false;
      for(const auto& house:block.buildings)if(polygons_overlap(bed,house.footprint))clear=false;
      if(clear) {
        garden(sc,r.child(600+side),p,4,5.5f,kDeck,true,true);
        gen_bench(sc,P3(p+Vec2{0,6.3f},kDeck),0);
      }
    }
    sc.register_range(first,static_cast<std::uint32_t>(sc.opaque.indices.size()),
        P3(block.centre,kDeck+maximum_height*.5f),std::sqrt(maximum_height*maximum_height*.25f+20000));
    ++sc.stats_blocks;
  }
}

void southern_city(Scene& sc,Rng rng) {
  // The far mainland has its own quay, coastal boulevard and depth of occupied
  // fabric. Everything lies on land; no silhouettes float over the water.
  std::vector<Vec2> coast;
  for(int i=0;i<=168;++i) {
    float x=-4400+i*80.f;coast.push_back({x,southern_coast(x)});
  }
  Emit land(&sc.opaque,M_GRASS),quay(&sc.opaque,M_CONCRETE_DARK),paving(&sc.opaque,M_PLAZA);
  Emit light(&sc.opaque,M_SIGN),asphalt(&sc.opaque,M_ASPHALT);
  std::array<Mat,4> far_glass;
  for(int i=0;i<4;++i) {
    MaterialDesc glass=sc.materials[M_GLASS_STD];glass.name="distant occupied glazing "+std::to_string(i);
    glass.base_color={.57f+i*.035f,.70f+i*.025f,.76f+i*.018f};
    glass.lit_probability=std::array<float,4>{{.045f,.08f,.13f,.19f}}[i];
    glass.roughness=.15f+i*.025f;
    far_glass[i]=static_cast<Mat>(sc.materials.size());sc.materials.push_back(glass);
  }
  for(std::size_t i=0;i+1<coast.size();++i) {
    Vec2 a=coast[i],b=coast[i+1];
    land.quad_metric(P3(b,.65f),P3(a,.65f),P3({a.x,10000},.65f),P3({b.x,10000},.65f));
    quay.beam(P3(a,0),P3(b,0),4,2.3f);
    paving.beam(P3(a+Vec2{0,8},kDeck-.15f),P3(b+Vec2{0,8},kDeck-.15f),15,.3f);
    asphalt.beam(P3(a+Vec2{0,29},kDeck-.22f),P3(b+Vec2{0,29},kDeck-.22f),17,.12f);
    for(int j=0;j<5;++j) {
      Vec2 p=a*(1-(j+.5f)/5.f)+b*((j+.5f)/5.f);
      light.box(P3(p+Vec2{0,9},kDeck+.08f),{.7f,.06f,.22f});
      if(j==1||j==3) {
        lamp(sc,P3(p+Vec2{0,12},kDeck),5.5f);
        sc.lights.back().radius=24;sc.lights.back().intensity=7;
      }
    }
  }
  for(int row=0;row<30;++row) {
    float z=950+row*77.f;
    road(sc,{{shore(z)+19,z},{east_shore(z)-19,z}},10,row%5==0);
  }
  for(int col=0;col<22;++col) {
    std::vector<Vec2> avenue;
    for(int row=0;row<30;++row)avenue.push_back({shore(950+row*77.f)+16+col*64,950+row*77.f});
    road(sc,avenue,col%5==0?17:8,col%5==0);
  }
  const auto plots=southern_plots(rng);
  std::array<std::array<const SouthernPlot*,117>,28> blocks{};
  for(const auto& p:plots)if(p.district==1)blocks[p.row][p.column]=&p;
  auto urban_street=[&](Vec2 aa,Vec2 bb,bool illuminated) {
    Vec3 a=P3(aa,kDeck-.11f),b=P3(bb,kDeck-.11f);
    Vec3 side=normalize(cross(b-a,Vec3{0,1,0}))*8;
    asphalt.beam(a,b,13,.12f);
    paving.beam(a+side,b+side,3,.20f);paving.beam(a-side,b-side,3,.20f);
    if(illuminated)for(int j=0;j<3;++j) {
      Vec3 p=lerp(a,b,(j+.3f)/3)+side+Vec3{0,.11f,0};
      lamp(sc,p,5.5f);sc.lights.back().radius=24;sc.lights.back().intensity=7;
    }
  };
  for(int col=4;col<116;col+=6) {
    std::vector<Vec2> line;
    for(int row=0;row<28;++row)
      if(blocks[row][col]&&blocks[row][col+1])line.push_back((blocks[row][col]->centre+blocks[row][col+1]->centre)*.5f);
    if(!line.empty())line.insert(line.begin(),{line.front().x,southern_coast(line.front().x)+29});
    for(std::size_t i=0;i+1<line.size();++i)urban_street(line[i],line[i+1],true);
  }
  for(int row=3;row<27;row+=6)for(int col=0;col<116;++col) {
    if(!blocks[row][col]||!blocks[row+1][col]||!blocks[row][col+1]||!blocks[row+1][col+1])continue;
    Vec2 a=(blocks[row][col]->centre+blocks[row+1][col]->centre)*.5f;
    Vec2 b=(blocks[row][col+1]->centre+blocks[row+1][col+1]->centre)*.5f;
    urban_street(a,b,col%2==0);
  }
  const auto neighborhood=southern_neighborhood(plots);
  build_nearshore_neighborhood(sc,neighborhood,far_glass,rng.child(22));
  const auto peninsula=connected_shore_neighborhood(plots,0),mainland=connected_shore_neighborhood(plots,1);
  build_nearshore_neighborhood(sc,peninsula,far_glass,rng.child(25));
  build_nearshore_neighborhood(sc,mainland,far_glass,rng.child(26));
  for(const auto& plot:plots) {
    if(nearshore_selection(plot)||neighborhood_replaces(peninsula,plot)||neighborhood_replaces(mainland,plot))continue;
    Rng r=rng.child(10,plot.district,plot.address);
    auto first=std::uint32_t(sc.opaque.indices.size());
    const auto& p=plot.footprint;const Vec2 c=plot.centre;
    float h=plot.floors*4.f;
    slab(sc.opaque,plan_scale(p,1.12f,c),kDeck,.15f,M_SIDEWALK);
    if((plot.address%7==0&&plot.floors>18)||(plot.floors>26&&plot.address%3!=0)) {
      Vec2 lo,hi;plan_bounds(p,&lo,&hi);float radius=std::min(hi.x-lo.x,hi.y-lo.y)*.48f;
      auto spec=family(plot.address,plot.floors,radius,r.range(-.5f,.5f));
      spec.base_scale=1.04f;spec.glass=far_glass[plot.address%4];
      build_tower(sc,spec,c,kDeck,r.child(1),1);++sc.stats_towers;
    } else {
      distant_occupied_mass(sc,p,plot.floors,plot.address,far_glass[plot.address%4],r.child(1));
    }
    // Door canopies, roof-service levels and pedestrian fixture rhythms retain
    // scale under haze; street lights follow the shore rather than a light plane.
    if(plot.address%2==0) {
      Vec2 edge=p.front();
      box(sc,M_LOBBY_LIGHT,P3(edge,kDeck+2.7f),{1.6f,.08f,.2f});
      light.box(P3(edge+Vec2{0,-2},kDeck+.12f),{.4f,.12f,.4f});
    }
    sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(c,kDeck+h*.5f),std::sqrt(h*h*.25f+5000));
    ++sc.stats_blocks;
  }
  // A low bridge is supported across the eastern inlet. Its deck helps read
  // the otherwise very large water distance in the night panorama.
  Vec3 a{east_shore(1900)-8,9,1900},b{3300,9,southern_coast(3300)+14};
  Emit(&sc.opaque,M_CONCRETE_WHITE).beam(a,b,14,1.5f);
  Vec3 direction=normalize(Vec3{b.x-a.x,0,b.z-a.z});
  Vec3 ramp_a=a-direction*90,ramp_b=b+Vec3{0,0,105};ramp_a.y=kDeck;ramp_b.y=kDeck;
  Emit(&sc.opaque,M_CONCRETE_WHITE).beam(ramp_a,a,14,1.2f);
  Emit(&sc.opaque,M_CONCRETE_WHITE).beam(b,ramp_b,14,1.2f);
  for(int i=1;i<5;++i)for(auto ends:{std::pair{ramp_a,a},std::pair{b,ramp_b}}) {
    Vec3 p=lerp(ends.first,ends.second,i/5.f);
    box(sc,M_CONCRETE_DARK,{p.x,(p.y-1.5f)*.5f,p.z},{.8f,(p.y+1.5f)*.5f,.8f});
  }
  Vec3 side=normalize(cross(b-a,Vec3{0,1,0}))*6.7f;
  Emit(&sc.opaque,M_BRONZE).beam(a+side,b+side,.22f,.9f);
  Emit(&sc.opaque,M_BRONZE).beam(a-side,b-side,.22f,.9f);
  int spans=int(length(b-a)/85);
  for(int i=0;i<=spans;++i) {
    Vec3 p=lerp(a,b,float(i)/spans);
    box(sc,M_CONCRETE_DARK,{p.x,3.7f,p.z},{1.3f,4.7f,2});
    light.box(p+side+Vec3{0,.65f,0},{.7f,.15f,.7f});
    light.box(p-side+Vec3{0,.65f,0},{.7f,.15f,.7f});
  }
}
struct CivicBaseWing {std::vector<Vec2> footprint;std::vector<float> levels;int identity;};
std::vector<CivicBaseWing> civic_base_wings() {
  std::vector<CivicBaseWing> result;
  const auto existing=plan_rounded_rect(52.5f,44.1f,4,12,kDome);
  std::vector<std::vector<Vec2>> base{plan_rounded_rect(76,62.75f,12,12,{-15,-16.25f})};
  // Keep the existing full stair width, then narrow gently to a generous
  // fourteen-metre public entry passage through the southern civic frontage.
  for(const auto& gap:std::vector<std::vector<Vec2>>{existing,plan_rect(38,6.4f,{-36,22.5f}),plan_rect(7,24,{-36,49})}) {
    std::vector<std::vector<Vec2>> remaining;
    for(const auto& p:base)for(auto q:subtract_convex(p,gap,.05f))remaining.push_back(std::move(q));
    base=std::move(remaining);
  }
  auto intersect=[](std::vector<Vec2> p,const std::vector<Vec2>& boundary) {
    for(std::size_t i=0;i<boundary.size()&&p.size()>2;++i) {
      const Vec2 a=boundary[i],d=boundary[(i+1)%boundary.size()]-a;
      p=clip_halfplane(p,a,{-d.y,d.x});
    }
    return p;
  };
  int identity=920;
  for(const auto& p:base) {
    if(p.size()<3||std::abs(plan_area(p))<4)continue;
    result.push_back({p,{kDeck,5.6f},identity++});
    for(const auto& region:std::vector<std::vector<Vec2>>{
        plan_rect(18.5f,36.5f,{40.5f,-24.5f}),plan_rect(26,10.5f,{29,36.5f}),
        plan_rect(19.5f,8.5f,{-68.5f,38.5f})}) {
      auto upper=intersect(p,region);
      if(upper.size()>2&&std::abs(plan_area(upper))>45&&plan_inradius(upper)>2.5f)
        result.push_back({std::move(upper),{5.6f,10.8f},identity++});
    }
  }
  // The northern institution and south-east assembly rooms form permanent
  // occupied edges around the open landmark axes. Their unequal returns leave
  // public passages instead of making another square perimeter template.
  result.push_back({plan_rounded_rect(40,15,5,8,{5,-163}),{kDeck,5.6f,10.8f,16.f},951});
  result.push_back({plan_rounded_rect(21,15,5,8,{72,-163}),{kDeck,5.6f,10.8f},952});
  result.push_back({plan_rounded_rect(41,13,7,8,{130,-12}),{kDeck,5.6f,10.8f},953});
  result.push_back({plan_rounded_rect(10,28,5,8,{164,29}),{kDeck,5.6f,10.8f,16.f},954});
  return result;
}
void build_joined_civic_base(Scene& sc,Rng rng) {
  const auto wings=civic_base_wings();
  Mat body=M_MARBLE_WHITE,ceramic=lattice_ceramic(sc),glass=M_GLASS_CLEAR;
  const auto first=static_cast<std::uint32_t>(sc.opaque.indices.size());
  for(std::size_t index=0;index<wings.size();++index) {
    const auto& wing=wings[index];const auto& p=wing.footprint;
    const float bottom=wing.levels.front(),top=wing.levels.back();
    // Deeply recessed room fronts retain occupied mass at every level. Real
    // shared faces stay opaque and connect at the dome's 5.6/10.8m datums.
    for(std::size_t floor=0;floor+1<wing.levels.size();++floor) {
      const float y=wing.levels[floor],ceiling=wing.levels[floor+1],height=ceiling-y;
      for(std::size_t edge=0;edge<p.size();++edge) {
        Vec2 a=p[edge],b=p[(edge+1)%p.size()],delta=b-a;
        const float span=length(delta);if(span<.08f)continue;
        const Vec2 along=normalize(delta),normal{along.y,-along.x};
        const int bays=std::max(1,int(std::ceil(span/4.5f)));
        for(int bay=0;bay<bays;++bay) {
          const Vec2 aa=a+delta*(float(bay)/bays),bb=a+delta*(float(bay+1)/bays),middle=(aa+bb)*.5f;
          bool shared=false;
          for(std::size_t other=0;other<wings.size();++other)if(other!=index) {
            const auto& next=wings[other];
            if(next.levels.front()>y+.03f||next.levels.back()<ceiling-.03f)continue;
            if(point_in_polygon(next.footprint,middle+normal*.03f)){shared=true;break;}
          }
          if(shared)continue;
          Emit facing(&sc.opaque,body),frames(&sc.opaque,M_BRONZE),windows(&sc.opaque,glass);
          windows.element_random=rng.child(wing.identity,int(floor)).next();
          auto face=[&](Emit& emit,Vec2 one,Vec2 two,float lo,float hi) {
            emit.quad_metric(P3(two,lo),P3(one,lo),P3(one,hi),P3(two,hi));
          };
          // Small retaining edge pieces are solid walkways, not fictitious
          // narrow rooms; larger wings have genuine door openings and bays.
          const bool walkway=plan_inradius(p)<2.5f;
          if(walkway)face(facing,aa,bb,y,ceiling);
          else {
            const bool door=bay==bays/2&&span>9;
            face(facing,aa,bb,ceiling-.64f,ceiling-.18f);
            if(!door)face(facing,aa,bb,y,y+.54f);
            face(windows,aa-normal*.55f,bb-normal*.55f,y+(door?3.15f:.54f),ceiling-.63f);
            facing.box(P3(aa-normal*.18f,y+(height-.3f)*.5f),{.16f,(height-.3f)*.5f,.38f},
                {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
            frames.box(P3(middle,ceiling-.65f),{span/bays*.5f,.055f,.12f},
                {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
            if(door) {
              frames.box(P3(middle,y+3.15f),{span/bays*.5f,.07f,.22f},
                  {along.x,0,along.y},{0,1,0},{normal.x,0,normal.y});
              Emit(&sc.opaque,M_LOBBY_LIGHT).box(P3(middle-normal*.65f,y+3.23f),{.6f,.025f,.15f});
              if(bottom==kDeck)sc.lights.push_back({P3(middle-normal*.8f,y+3.05f),7,{1,.79f,.55f},3.2f});
            }
          }
        }
      }
      slab(sc.opaque,p,ceiling,.20f,ceramic);
    }
    Emit(&sc.opaque,roof_finish(sc,wing.identity)).polygon(p,top+.008f,true);
    sc.authored_roofs.push_back({p,top+.008f,wing.identity,rng.child(wing.identity)});
    sc.roof_obstructions.push_back({p,bottom,top});
    for(std::size_t edge=0;edge<p.size();++edge) {
      Vec2 a=p[edge],b=p[(edge+1)%p.size()],direction=normalize(b-a),outward{direction.y,-direction.x};
      const int pieces=std::max(1,int(std::ceil(length(b-a)/2.5f)));
      for(int part=0;part<pieces;++part) {
        Vec2 one=a+(b-a)*(float(part)/pieces),two=a+(b-a)*(float(part+1)/pieces),mid=(one+two)*.5f;
        bool supported=false;
        if(std::abs(top-5.6f)<.03f&&point_in_polygon(plan_rounded_rect(52.5f,44.1f,4,12,kDome),mid+outward*.05f))supported=true;
        for(std::size_t other=0;other<wings.size();++other)if(other!=index) {
          const auto& q=wings[other];
          if(q.levels.front()<=top+.03f&&q.levels.back()>=top-.03f&&point_in_polygon(q.footprint,mid+outward*.05f))supported=true;
        }
        if(mid.x>49&&mid.x<55&&mid.y>11&&mid.y<40)supported=true;
        if(supported)continue;
        Emit(&sc.opaque,M_BRONZE).beam(P3(one,top+1.1f),P3(two,top+1.1f),.045f,.055f);
        Emit(&sc.opaque,M_BRONZE).tube(P3(one,top),P3(one,top+1.1f),.035f,8,true);
        Emit(&sc.opaque,body).beam(P3(one,top+.10f),P3(two,top+.10f),.18f,.20f);
      }
    }
    ++sc.stats_standards;
  }
  // Close the original raised plinth to the public ground. Its original top,
  // drum, lantern and door remain fixed, and the old 20 treads remain intact.
  const auto foundation=plan_rounded_rect(52.5f,44.1f,4,12,kDome);
  slab(sc.opaque,foundation,2.4f,1.2f,M_MARBLE_WHITE);
  slab(sc.opaque,plan_rect(37.6f,4.2f,{-36,20.3f}),2.4f,1.2f,M_MARBLE_WHITE);
  for(int step=0;step<8;++step) {
    const float top=2.4f-step*.15f;
    box(sc,M_MARBLE_WHITE,{-36,(kDeck+top)*.5f,24.5f+(step+.5f)*.42f},{36.75f,(top-kDeck)*.5f,.21f});
  }
  // One real terrace flight rises from the joined 5.6m garden to the southern
  // 10.8m roof. Its solid risers and top landing are supported by the base.
  for(int step=0;step<32;++step) {
    const float top=5.6f+(step+1)*(5.2f/32);
    box(sc,M_MARBLE_WHITE,{52,(5.6f+top)*.5f,14+(step+.5f)*.375f},{2,(top-5.6f)*.5f,.1875f});
  }

  for(float x:{49.8f,54.2f})rail(sc,{x,5.6f,14},{x,10.8f,26},false);
  // A protected upper promenade also reserves its body/head space from roof
  // planting and furniture; it corresponds to the actual supported slabs.
  sc.roof_obstructions.push_back({plan_rect(3,15,{52,26}),5.55f,13.f});
  sc.roof_obstructions.push_back({plan_rect(46,2,{8,13}),5.55f,8.5f});
  sc.register_range(first,static_cast<std::uint32_t>(sc.opaque.indices.size()),{40,10,-53},220);
}

void civic_arcade_edge(Scene& sc,Rng rng) {
  // Three occupied pavilions and a joined colonnade occupy the surveyed civic
  // edge between the avenue and nearest cross street. Both streets stay open.
  auto point=[](float lateral,float row,float y){return Vec3{kGridCos*lateral+kGridSin*row,y,-kGridSin*lateral+kGridCos*row};};
  const std::array<std::pair<float,float>,3> wings{{{-10,29},{36,77},{84,140}}};
  for(std::size_t i=0;i<wings.size();++i) {
    auto [left,right]=wings[i];const auto p=survey_rect(left,right,25,36);
    sustained_frontage(sc,rng.child(10+i),p,kDeck,2,903+int(i)*2,i!=1);
  }
  const Mat ceramic=lattice_ceramic(sc);
  const auto ground=survey_rect(-15,145,12,41),roof=survey_rect(-10,140,17,25);
  slab(sc.opaque,ground,kDeck,.6f,M_PLAZA);slab(sc.opaque,roof,5.2f,.16f,ceramic);
  Emit(&sc.opaque,M_BRONZE).beam(point(-10,19,5.06f),point(140,19,5.06f),.14f,.09f);
  sc.authored_roofs.push_back({roof,5.2f,905,rng.child(30)});sc.roof_obstructions.push_back({roof,4.90f,5.2f});
  for(int bay=0;bay<18;++bay) {
    const float x=-5.4f+bay*8.1f;
    if(!sc.asset_library.resources.empty())add_asset_instance(sc,"ceramic_arch",point(x,19,kDeck+.016f),std::atan2(kGridSin,kGridCos),1.08f);
    else {
      for(float sign:{-1.f,1.f})Emit(&sc.opaque,ceramic).beam(point(x+sign*3.65f,19,kDeck),point(x+sign*3.65f,19,4.7f),.24f,.28f);
      Emit(&sc.opaque,ceramic).beam(point(x-3.65f,19,4.7f),point(x+3.65f,19,4.7f),.24f,.28f);
    }
    Emit(&sc.opaque,M_LOBBY_LIGHT).beam(point(x-1.2f,21.1f,4.97f),point(x+1.2f,21.1f,4.97f),.052f,.15f);
    sc.lights.push_back({point(x,21.1f,4.70f),7,{1,.78f,.51f},2.4f});
  }
}

void civic(Scene& sc,Rng rng) {
  auto plaza=plan_rounded_rect(150,137,22,12,{38,-48});
  std::vector<std::vector<Vec2>> civic_ground{plaza};
  for(const auto& corridor:arrival_primary_corridors()) {
    std::vector<std::vector<Vec2>> pieces;
    for(const auto& p:civic_ground)for(auto piece:subtract_convex(p,corridor,.0001f))pieces.push_back(std::move(piece));
    civic_ground=std::move(pieces);
  }
  for(const auto& p:civic_ground)slab(sc.opaque,p,kDeck,.8f,M_PLAZA);
  std::vector<std::vector<Vec2>> west_floor{kWestCivicCourt};
  for(const auto& opening:sc.asset_library.resources.empty()?std::vector<std::vector<Vec2>>{}:west_civic_floor_openings()) {
    std::vector<std::vector<Vec2>> solid;
    for(const auto& piece:west_floor)for(auto part:subtract_convex(piece,opening,.0001f))solid.push_back(std::move(part));
    west_floor=std::move(solid);
  }
  for(const auto& piece:west_floor)slab(sc.opaque,piece,kDeck,.8f,M_PLAZA);
  if(!sc.asset_library.resources.empty())stage_civic_forecourt(sc,rng.child(230));
  // Local steps remain beside the east civic frontage; they no longer span
  // either of the two continuous arterial alignments.
  for(int step=0;step<8;++step)
    box(sc,M_MARBLE_WHITE,{159,kDeck+step*.15f,-1-step*.95f},{23,.075f,.5f});
  slab(sc.opaque,plan_circle(kRingRadius*.75f+1.6f,128,kRing),kDeck,.8f,M_PLAZA);
  build_cinematic_ring(sc,kRing,kDeck,kRingRadius,radians(14));
  const SceneTail dome_start(sc);
  build_cinematic_dome(sc,kDome,kDeck+1.2f,42,0,rng.child(1));
  build_joined_civic_base(sc,rng.child(250));
  dome_start.move(sc,kDomeMove);
  std::vector<std::vector<Vec2>> dome_ground{plan_rect(80,67,kDome+kDomeMove)};
  for(const auto& corridor:arrival_primary_corridors()) {
    std::vector<std::vector<Vec2>> pieces;
    for(const auto& p:dome_ground)for(auto piece:subtract_convex(p,corridor,.0001f))pieces.push_back(std::move(piece));
    dome_ground=std::move(pieces);
  }
  for(const auto& p:dome_ground)slab(sc.opaque,p,kDeck,.8f,M_PLAZA);
  const auto occupied_civic=civic_base_wings();
  auto civic_bed_clear=[&](Vec2 p,float hx,float hz) {
    const auto bed=plan_rect(hx+3,hz+3,p);
    for(const auto& corridor:arrival_primary_corridors())if(polygons_overlap(bed,corridor))return false;
    for(const auto& wing:occupied_civic)if(polygons_overlap(bed,moved_plan(wing.footprint,kDomeMove)))return false;
    return true;
  };
  // Ring and dome sit in connected planted courts with curved routes.
  for(int i=0;i<19;++i) {
    float a=i*2*kPi/19;Vec2 p=kRing+Vec2{std::cos(a)*74,std::sin(a)*73};
    if(p.y>kRing.y+45||!civic_bed_clear(p,3.5f,4))continue;
    garden(sc,rng.child(50+i),p,3.5f,4,kDeck,i%3==0,true);
    lamp(sc,P3(p+Vec2{4,1},kDeck));
  }
  for(int j=0;j<7;++j) {
    Vec2 p{-72.f,71-j*24.f};if(!civic_bed_clear(p,7,4))continue;
    garden(sc,rng.child(90+j),p,7,4,kDeck,true,true);
    gen_bench(sc,P3(p+Vec2{9,0},kDeck),kPi*.5f);
  }
  // Small construction joints are confined to the east civic terrace.
  for(int i=0;i<16;++i)box(sc,M_CONCRETE_DARK,{112+i*4.f,kDeck+.014f,-26},{.018f,.014f,9});
  auto needle=family(10,110,25,.1f);needle.member=M_BRONZE;needle.taper=.20f;needle.tip=.45f;
  low_building(sc,rng.child(125),plan_rounded_rect(36,36,8,10,{190,-300}),{190,-300},kDeck,3,1,true);
  tower(sc,needle,{190,-300},kDeck+12,rng.child(130),2);
  auto ribbon=family(7,43,26,.12f);ribbon.a=32;ribbon.b=23;
  low_building(sc,rng.child(126),plan_rounded_rect(41,33,8,10,{235,-12}),{235,-12},kDeck,4,1,true);
  tower(sc,ribbon,{235,-12},kDeck+16,rng.child(131),2);
  auto lens=family(2,44,32,-.18f);lens.glass=M_GLASS_BRONZE;
  low_building(sc,rng.child(127),plan_rounded_rect(49,42,9,10,{410,95}),{410,95},kDeck,3,1,true);
  tower(sc,lens,{410,95},kDeck+12,rng.child(132),2);
  civic_arcade_edge(sc,rng.child(240));
  sc.stats_plazas=1;
}
void ceramic_member(Scene& sc,Vec3 a,Vec3 b,Vec3 outward,float scale=1.f,Mat facing=M_WHITE_METAL) {
  Vec3 along=normalize(b-a);
  Vec3 u=normalize(cross(outward,along)),v=normalize(cross(along,u));
  const float w=.61f*scale,d=.28f*scale,bevel=.10f*scale;
  const std::array<Vec2,8> section{{{-w+bevel,-d},{w-bevel,-d},{w,-d+bevel},{w,d-bevel},
                                  {w-bevel,d},{-w+bevel,d},{-w,d-bevel},{-w,-d+bevel}}};
  Emit ceramic(&sc.opaque,facing),backing(&sc.opaque,M_DARK_METAL);
  backing.beam(a,b,1.12f*scale,.42f*scale,outward);
  int panels=std::max(1,int(std::ceil(length(b-a)/3.6f)));
  for(int j=0;j<panels;++j) {
    Vec3 p=lerp(a,b,float(j)/panels)+along*.016f;
    Vec3 q=lerp(a,b,float(j+1)/panels)-along*.016f;
    for(int i=0;i<8;++i) {
      int next=(i+1)%8;
      Vec3 s=u*section[i].x+v*section[i].y,t=u*section[next].x+v*section[next].y;
      ceramic.quad_metric(p+s,p+t,q+t,q+s);
      // Panel end faces expose the small bevel around each recessed gasket.
      ceramic.triangle(p,p+t,p+s);ceramic.triangle(q,q+s,q+t);
    }
  }
}
Mat lattice_ceramic(Scene& sc) {
  for(std::size_t i=0;i<sc.materials.size();++i)
    if(sc.materials[i].name=="satin ivory ceramic lattice")return static_cast<Mat>(i);
  MaterialDesc material=sc.materials[M_WHITE_METAL];
  material.name="satin ivory ceramic lattice";material.base_color={.71f,.75f,.72f};
  material.roughness=.29f;material.normal_strength=.085f;material.metallic=0;
  Mat result=static_cast<Mat>(sc.materials.size());sc.materials.push_back(material);return result;
}
void ensure_cast_junction(Scene& sc,const std::string& name,float angle,float cylinder_radius,Mat facing) {
  if(sc.asset_library.resources.empty())return;
  for(const auto& resource:sc.asset_library.resources)if(resource.name==name)return;
  MeshResource resource;resource.name=name;
  Emit ceramic(&resource.mesh,facing),gasket(&resource.mesh,M_DARK_METAL),bronze(&resource.mesh,M_BRONZE);
  constexpr int samples=80;constexpr float arm=2.45f,half_width=.61f,corner=.10f;
  auto smooth_union=[](float a,float b) {float h=std::max(.38f-std::abs(a-b),0.f)/.38f;return std::min(a,b)-h*h*.095f;};
  auto distance=[&](Vec2 p) {
    float result=100;
    for(float sx:{-1.f,1.f})for(float sy:{-1.f,1.f}) {
      Vec2 dir{sx*std::cos(angle),sy*std::sin(angle)},side{-dir.y,dir.x};
      float x=std::abs(dot(p,dir)-arm*.5f)-(arm*.5f-corner);
      float y=std::abs(dot(p,side))-(half_width-corner);
      float d=length(Vec2{std::max(x,0.f),std::max(y,0.f)})+std::min(std::max(x,y),0.f)-corner;
      result=smooth_union(result,d);
    }
    return result;
  };
  std::array<Vec2,samples> outline;
  for(int i=0;i<samples;++i) {
    float angle=i*2*kPi/samples;Vec2 direction{std::cos(angle),std::sin(angle)};
    float lo=0,hi=3.6f;
    for(int j=0;j<20;++j) {float mid=(lo+hi)*.5f;if(distance(direction*mid)<0)lo=mid;else hi=mid;}
    outline[i]=direction*((lo+hi)*.5f);
  }
  const std::array<float,4> depth{{.28f,.18f,-.18f,-.28f}},scale{{.956f,1,1,.956f}};
  auto vertex=[&](int i,int layer){Vec2 p=outline[i]*scale[layer];return Vec3{p.x,p.y,depth[layer]};};
  for(int i=0;i<samples;++i) {
    int j=(i+1)%samples;
    ceramic.triangle({0,0,depth[0]},vertex(i,0),vertex(j,0));
    ceramic.triangle({0,0,depth[3]},vertex(j,3),vertex(i,3));
    for(int layer=0;layer<3;++layer)ceramic.quad_metric(vertex(i,layer),vertex(i,layer+1),vertex(j,layer+1),vertex(j,layer));
  }
  // Brackets and inspection fasteners sit behind the casting face.
  gasket.box({0,0,-.34f},{.63f,.78f,.055f});
  bronze.box({0,0,-.44f},{.42f,.55f,.045f});
  for(float x:{-.32f,.32f})for(float y:{-.43f,.43f})
    bronze.frustum({x,y,-.34f},{x,y,-.275f},.043f,.043f,6);
  // Bake the shallow cylinder curvature into the reusable casting, including
  // the inverse-transpose normal and the corresponding tangent derivative.
  resource.mesh.bounds_min={1e30f,1e30f,1e30f};resource.mesh.bounds_max={-1e30f,-1e30f,-1e30f};
  for(auto& v:resource.mesh.vertices) {
    float derivative=v.position.x/cylinder_radius;
    v.position.z-=v.position.x*v.position.x/(2*cylinder_radius);
    v.normal=normalize(Vec3{v.normal.x+derivative*v.normal.z,v.normal.y,v.normal.z});
    Vec3 tangent{v.tangent.x,v.tangent.y,v.tangent.z-derivative*v.tangent.x};
    tangent=normalize(tangent-v.normal*dot(tangent,v.normal));
    v.tangent={tangent.x,tangent.y,tangent.z,v.tangent.w};
    resource.mesh.bounds_min=vmin(resource.mesh.bounds_min,v.position);resource.mesh.bounds_max=vmax(resource.mesh.bounds_max,v.position);
  }
  sc.asset_library.resources.push_back(std::move(resource));
}
void cylindrical_lattice(Scene& sc,Vec2 centre,float base,float height,int columns,int levels,
                         const std::function<float(float)>& radius,float member_scale,const std::string& casting_name) {
  Mat facing=lattice_ceramic(sc);bool authored=!sc.asset_library.resources.empty();
  const bool entrances=casting_name=="foreground ceramic cast crossing";
  Mat north_facing=facing;
  if(entrances) {
    MaterialDesc graphite=sc.materials[M_BRONZE];graphite.name="graphite north structural frame";
    graphite.base_color={.028f,.041f,.052f};graphite.roughness=.23f;graphite.metallic=.62f;
    graphite.albedo_set="";graphite.flags=0;
    north_facing=static_cast<Mat>(sc.materials.size());sc.materials.push_back(graphite);
  }
  float mean_radius=radius(.5f),angle=std::atan2(height/levels,2*kPi*mean_radius/columns);
  ensure_cast_junction(sc,casting_name,angle,mean_radius/member_scale,facing);
  if(entrances)ensure_cast_junction(sc,casting_name+" graphite",angle,mean_radius/member_scale,north_facing);
  auto point=[&](float col,float row) {
    float t=row/levels,a=2*kPi*col/columns,r=radius(t);
    return Vec3{centre.x+std::cos(a)*r,base+t*height,centre.y+std::sin(a)*r};
  };
  struct Portal {float angle,floor;};
  const std::array<Portal,4> portals{{{0,kGardenY},{kPi,kGardenY},{4.596f,kGardenY},{4.596f,kGardenY+8}}};
  auto local=[&](Vec3 p,const Portal& portal) {
    Vec3 normal{std::cos(portal.angle),0,std::sin(portal.angle)},side{-normal.z,0,normal.x};
    Vec3 relative=p-P3(centre,portal.floor);
    return Vec3{dot(relative,side),relative.y,dot(relative,normal)};
  };
  auto clipped_member=[&](Vec3 a,Vec3 b,Vec3 outward) {
    std::vector<std::pair<Vec3,Vec3>> segments{{a,b}};
    if(entrances)for(const auto& portal:portals) {
      std::vector<std::pair<Vec3,Vec3>> outside;
      for(const auto& segment:segments) {
        Vec3 p=local(segment.first,portal),q=local(segment.second,portal),d=q-p;
        const std::array<float,3> low{{-3.5f,-.7f,40.f}},high{{3.5f,4.5f,49.f}};
        const std::array<float,3> origin{{p.x,p.y,p.z}},direction{{d.x,d.y,d.z}};
        float enter=0,leave=1;bool intersects=true;
        for(int axis=0;axis<3;++axis) {
          if(std::abs(direction[axis])<1e-6f) {if(origin[axis]<low[axis]||origin[axis]>high[axis])intersects=false;}
          else {
            float one=(low[axis]-origin[axis])/direction[axis],two=(high[axis]-origin[axis])/direction[axis];
            if(one>two)std::swap(one,two);
            enter=std::max(enter,one);leave=std::min(leave,two);
          }
        }
        if(!intersects||enter>=leave)outside.push_back(segment);
        else {
          if(enter>1e-4f)outside.push_back({segment.first,lerp(segment.first,segment.second,enter)});
          if(leave<.9999f)outside.push_back({lerp(segment.first,segment.second,leave),segment.second});
        }
      }
      segments=std::move(outside);
    }
    for(const auto& segment:segments)if(length(segment.second-segment.first)>.08f)
      ceramic_member(sc,segment.first,segment.second,outward,member_scale,
          entrances&&garden_graphite_sector((segment.first+segment.second)*.5f,centre)?north_facing:facing);
  };
  auto connection=[&](float c0,float r0,float c1,float r1) {
    float length_hint=length(point(c1,r1)-point(c0,r0));
    bool trim0=authored&&r0>0&&r0<levels,trim1=authored&&r1>0&&r1<levels;
    float begin=trim0?2.36f*member_scale/length_hint:0,end=trim1?1-2.36f*member_scale/length_hint:1;
    for(int j=0;j<3;++j) {
      float t0=begin+(end-begin)*j/3,t1=begin+(end-begin)*(j+1)/3;
      Vec3 p=point(c0+(c1-c0)*t0,r0+(r1-r0)*t0),q=point(c0+(c1-c0)*t1,r0+(r1-r0)*t1);
      Vec3 n=normalize(Vec3{(p.x+q.x)*.5f-centre.x,0,(p.z+q.z)*.5f-centre.y});
      clipped_member(p,q,n);
    }
  };
  auto casting=[&](float col,float row) {
    if(!authored)return;
    Vec3 p=point(col,row);float yaw=kPi*.5f-2*kPi*col/columns;
    if(entrances)for(const auto& portal:portals) {
      Vec3 q=local(p,portal);
      if(std::abs(q.x)<6.2f&&q.y>-3.2f&&q.y<7.0f&&q.z>40&&q.z<49)return;
    }
    add_asset_instance(sc,entrances&&garden_graphite_sector(p,centre)?casting_name+" graphite":casting_name,p,yaw,member_scale);
  };
  for(int row=0;row<levels;++row)for(int col=0;col<columns;++col) {
    connection(col,row,col+.5f,row+.5f);connection(col+.5f,row+.5f,col+1,row+1);
    connection(col+1,row,col+.5f,row+.5f);connection(col+.5f,row+.5f,col,row+1);
    casting(col+.5f,row+.5f);
    if(row>0)casting(col,row);
  }  if(entrances)for(const auto& portal:portals) {
    Vec3 normal{std::cos(portal.angle),0,std::sin(portal.angle)},side{-normal.z,0,normal.x};
    const Vec3 centre_point=P3(centre,portal.floor)+normal*44.28f;
    Emit frame(&sc.opaque,facing),bronze(&sc.opaque,M_BRONZE);
    for(float sign:{-1.f,1.f}) {
      frame.box(centre_point+side*(sign*3.9f)+Vec3{0,2.6f,0},{.55f,2.6f,.8f},side,{0,1,0},normal);
      bronze.box(centre_point+side*(sign*3.9f)+Vec3{0,.15f,0},{.69f,.15f,.95f},side,{0,1,0},normal);
    }
    frame.box(centre_point+Vec3{0,4.65f,0},{4.45f,.55f,.8f},side,{0,1,0},normal);
    // A concealed sill and a deep head girder transfer the clipped diagonal
    // loads around an actual public opening in the independent exoskeleton.
    frame.box(centre_point-Vec3{0,.45f,0},{4.45f,.4f,.8f},side,{0,1,0},normal);
  }

}
void foreground_exoskeleton(Scene& sc) {
  const Mat ceramic=static_cast<Mat>(sculpted_diagrid_ceramic(sc));
  MaterialDesc graphite=sc.materials[M_BRONZE];
  graphite.name="graphite north structural frame";
  graphite.base_color={.028f,.041f,.052f};graphite.roughness=.23f;graphite.metallic=.62f;
  graphite.albedo_set="";graphite.flags=0;
  const Mat north=static_cast<Mat>(sc.materials.size());sc.materials.push_back(graphite);
  SculptedDiagridSpec spec;
  spec.centre=kLattice;spec.base_y=kDeck+16;
  // The north stair connects both loggia floors. Its intermediate landing
  // needs the same uninterrupted opening as the two end landings.
  spec.openings={{kPi,kGardenY},{4.596f,kGardenY,13.3f}};
  build_sculpted_diagrid(sc,spec,[&](float angle){return garden_graphite_sector(angle)?north:ceramic;});
  // Existing public portals retain their actual reinforced jamb, head and
  // sill contacts. Profiled diagonals stop at these closed structural frames.
  for(const auto& portal:spec.openings) {
    Vec3 normal{std::cos(portal.angle),0,std::sin(portal.angle)},side{-normal.z,0,normal.x};
    const Vec3 center=P3(kLattice,portal.floor_y)+normal*44.28f;
    const float height=portal.head_offset-.1f;
    Emit frame(&sc.opaque,ceramic),bronze(&sc.opaque,M_BRONZE);
    for(float sign:{-1.f,1.f}) {
      frame.box(center+side*(sign*3.9f)+Vec3{0,height*.5f,0},{.55f,height*.5f,.8f},side,{0,1,0},normal);
      bronze.box(center+side*(sign*3.9f)+Vec3{0,.15f,0},{.69f,.15f,.95f},side,{0,1,0},normal);
    }
    frame.box(center+Vec3{0,height-.55f,0},{4.45f,.55f,.8f},side,{0,1,0},normal);
    frame.box(center-Vec3{0,.45f,0},{4.45f,.4f,.8f},side,{0,1,0},normal);
  }
}
void northern_weather_envelope(Scene& sc) {
  MaterialDesc pane=sc.materials[M_GLASS_CLEAR];pane.name="northern graphite curved weather glazing";
  // Full pane coverage and stated solar transmittance: RGB * .24 gives normal
  // incidence transmission {.1392,.1584,.1776}. This remains a dielectric
  // approximation, not an invented high-IOR substitute for a reflective film.
  pane.flags=128u;pane.base_color={.58f,.66f,.74f};pane.roughness=.075f;pane.metallic=0;
  pane.room_w=1.5f;pane.room_h=.24f;pane.room_d=1.f;pane.lit_probability=.022f;
  pane.albedo_set="";pane.normal_strength=0;
  const Mat glass=static_cast<Mat>(sc.materials.size());sc.materials.push_back(pane);
  MaterialDesc backing=sc.materials[M_DARK_METAL];backing.name="north curtain graphite spandrel";
  backing.base_color={.032f,.045f,.055f};backing.roughness=.27f;backing.metallic=.34f;
  backing.albedo_set="";backing.flags=0;
  const Mat spandrel=static_cast<Mat>(sc.materials.size());sc.materials.push_back(backing);
  const float base=kDeck+16,height=544;
  auto point=[&](float angle,float y) {
    float t=std::clamp((y-base)/height,0.f,1.f),radius=44*(1-.05f*t*t)+1.38f;
    return Vec3{kLattice.x+std::cos(angle)*radius,y,kLattice.y+std::sin(angle)*radius};
  };
  for(int floor=0;floor<136;++floor) {
    float y=base+floor*4;
    for(int panel=0;panel<88;++panel) {
      float a=kPi+panel*kPi/88,b=kPi+(panel+1)*kPi/88;
      const float middle=(a+b)*.5f;
      if(!garden_graphite_sector(middle))continue;
      // Physical weather doors coincide with the actual western garden and
      // northern stair/loggia connections, including their full clear width.
      if((floor==65&&middle<kPi+.18f)||
          ((floor==65||floor==67)&&std::abs(middle-4.596f)<.12f))continue;
      Vec3 p=point(a,y+.83f),q=point(b,y+.83f),r=point(b,y+3.90f),s=point(a,y+3.90f);
      Emit(&sc.opaque,spandrel).quad_metric(point(b,y+.02f),point(a,y+.02f),point(a,y+.82f),point(b,y+.82f));
      Emit(&sc.opaque,glass).quad_metric(q,p,s,r);
      Emit(&sc.opaque,M_DARK_METAL).beam(p,s,.045f,.075f);
      Emit(&sc.opaque,M_BRONZE).beam(s,r,.055f,.045f);
    }
  }
  // Full-height transition ribs terminate the two fixed architectural skins.
  // West and south loggias expose ivory members outside their occupied inner
  // curtain; the north/northeast sector retains its independent dark screen.
  // These snapped column boundaries do not cross either actual access portal.
  for(float a:{kGardenGraphiteBegin,kGardenGraphiteEnd})for(int floor=0;floor<136;++floor) {
    Vec3 p=point(a,base+floor*4),q=point(a,base+(floor+1)*4);
    Emit(&sc.opaque,M_BRONZE).beam(p,q,.17f,.28f);
  }
}
void showcase_lattice_tower(Scene& sc,TowerSpec spec,Vec2 centre,float base,Rng rng) {
  const float original_radius=spec.a,shaft=base+spec.base_floors*spec.floor_h,height=spec.floors*spec.floor_h;
  auto profile=[&](float t) {
    float s=1-spec.taper*t*t;
    if(spec.tip>0&&t>.85f)s*=1-spec.tip*std::pow((t-.85f)/.15f,1.6f);
    return s;
  };
  spec.a-=.82f;spec.b=spec.a;spec.facade=FacadeKind::Curtain;spec.glass=M_GLASS_BLUE;
  spec.member=lattice_ceramic(sc);spec.frame=M_BRONZE;spec.module_w=2.8f;
  tower(sc,spec,centre,base,rng,2);
  auto first=std::uint32_t(sc.opaque.indices.size());
  cylindrical_lattice(sc,centre,shaft,height,12,std::max(8,spec.floors/2),
      [&](float t){return original_radius*profile(t)+.12f;},.57f,"middle ceramic cast crossing");
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(centre,base+height*.5f),height*.6f+35);
}
void oval_landmark(Scene& sc,Rng rng) {
  const Vec2 centre{458.8f,-360};constexpr float a=60.f,b=34,height=136,rot=-.2f;
  const auto vertex_first=sc.opaque.vertices.size(),instance_first=sc.asset_instances.size();
  auto spec=family(0,32,a,rot);spec.plan=PlanKind::Superellipse;spec.exponent=2;
  spec.a=a;spec.b=b;spec.taper=.035f;spec.facade=FacadeKind::Curtain;
  spec.glass=M_GLASS_BLUE;spec.base_scale=1.02f;spec.crown=CrownKind::Parapet;
  tower(sc,spec,centre,kDeck,rng.child(1),2);
  auto first=std::uint32_t(sc.opaque.indices.size());
  constexpr int columns=22,levels=9;
  auto node=[&](int column,int row) {
    float angle=column*2*kPi/columns,t=float(row)/levels,shaft_t=std::clamp((height*t-8)/128.f,0.f,1.f);
    float scale=1-.035f*shaft_t*shaft_t;
    Vec2 q{(a*scale+.7f)*std::cos(angle),(b*scale+.7f)*std::sin(angle)};
    return P3(centre+Vec2{q.x*std::cos(rot)-q.y*std::sin(rot),q.x*std::sin(rot)+q.y*std::cos(rot)},kDeck+height*t);
  };
  for(int row=0;row<levels;++row)for(int col=0;col<columns;++col) {
    Vec3 aa=node(col,row),bb=node(col+1,row+1),cc=node(col+1,row),dd=node(col,row+1);
    Vec3 outward=normalize(Vec3{(aa.x+bb.x)*.5f-centre.x,0,(aa.z+bb.z)*.5f-centre.y});
    ceramic_member(sc,aa,bb,outward,1.05f,lattice_ceramic(sc));ceramic_member(sc,cc,dd,outward,1.05f,lattice_ceramic(sc));
    Emit(&sc.opaque,M_BRONZE).frustum(aa+outward*.2f,aa+outward*.35f,.43f,.43f,8);
  }
  auto crown=plan_superellipse(a*.965f,b*.965f,2,80,centre,rot);
  slab(sc.opaque,plan_offset(crown,-.7f),kDeck+height+.04f,.07f,M_TERRAZZO);
  parapet(sc.opaque,crown,kDeck+height,1.15f,.38f,M_WHITE_METAL);
  // The broad roof is an occupied sky court with a small service enclosure,
  // rather than a deep arbitrary cap attached only to the camera silhouette.
  auto enclosure=plan_rounded_rect(26,16,8,8,centre);
  Emit(&sc.opaque,M_GLASS_STD).wall(enclosure,kDeck+height,kDeck+height+2,true);
  slab(sc.opaque,enclosure,kDeck+height+2,.28f,M_BRONZE);
  for(int i=0;i<12;++i) {
    float angle=i*2*kPi/12;
    Vec3 outer=P3(centre+Vec2{std::cos(angle)*a*.94f,std::sin(angle)*b*.94f},kDeck+height+.8f);
    Vec3 inner=P3(centre+Vec2{std::cos(angle)*a*.72f,std::sin(angle)*b*.72f},kDeck+height+1.8f);
    ceramic_member(sc,outer,inner,{0,1,0},.72f);
    if(!sc.asset_library.resources.empty()) {
      add_asset_instance(sc,"roof_service_cabinet",P3(centre+Vec2{std::cos(angle)*45,std::sin(angle)*24},kDeck+height),angle,1.f);
      add_asset_instance(sc,"roof_solar_panel",P3(centre+Vec2{std::cos(angle)*43,std::sin(angle)*23},kDeck+height),angle,1.6f);
    }
  }
  for(int i=0;i<7;++i) {
    float angle=i*2*kPi/7;Vec2 p=centre+Vec2{std::cos(angle)*44,std::sin(angle)*22};
    garden(sc,rng.child(20+i),p,6,3,kDeck+height,false,true);
  }
  // The smaller occupied context tower retains its own parcel and a coherent
  // curved envelope. Its floors and crowns are authored at the new height.
  constexpr float new_a=60.f,new_b=34,new_rot=-.862f,sx=new_a/a,sz=new_b/b;
  auto rotate=[](Vec2 p,float angle) {return Vec2{p.x*std::cos(angle)-p.y*std::sin(angle),p.x*std::sin(angle)+p.y*std::cos(angle)};};
  auto shape=[&](float y) {float t=std::clamp((y-kDeck)/height,0.f,1.f),wave=std::sin(kPi*t);return 1+.06f*wave*wave;};
  auto transform=[&](Vec3 p) {
    Vec2 local=rotate({p.x-centre.x,p.z-centre.y},-rot);float scale=shape(p.y);
    Vec2 q=rotate({local.x*sx*scale,local.y*sz*scale},new_rot);
    return Vec3{centre.x+q.x,p.y,centre.y+q.y};
  };
  for(std::size_t i=vertex_first;i<sc.opaque.vertices.size();++i) {
    auto& v=sc.opaque.vertices[i];Vec2 local=rotate({v.position.x-centre.x,v.position.z-centre.y},-rot);
    float t=std::clamp((v.position.y-kDeck)/height,0.f,1.f),scale=shape(v.position.y);
    float derivative=(v.position.y>kDeck&&v.position.y<kDeck+height)?(.12f*kPi/height*std::sin(kPi*t)*std::cos(kPi*t)):0;
    Vec2 old_normal=rotate({v.normal.x,v.normal.z},-rot);
    Vec2 nh=rotate({old_normal.x/(sx*scale),old_normal.y/(sz*scale)},new_rot);
    v.normal=normalize(Vec3{nh.x,v.normal.y-derivative/scale*dot(local,old_normal),nh.y});
    Vec2 old_tangent=rotate({v.tangent.x,v.tangent.z},-rot);
    Vec2 th=rotate({sx*(scale*old_tangent.x+derivative*local.x*v.tangent.y),sz*(scale*old_tangent.y+derivative*local.y*v.tangent.y)},new_rot);
    Vec3 tangent{th.x,v.tangent.y,th.y};tangent=normalize(tangent-v.normal*dot(v.normal,tangent));
    v.tangent={tangent.x,tangent.y,tangent.z,v.tangent.w};v.position=transform(v.position);
  }
  for(std::size_t i=instance_first;i<sc.asset_instances.size();++i) {
    auto& instance=sc.asset_instances[i];instance.translation=transform(instance.translation);instance.yaw+=rot-new_rot;
  }
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(centre,kDeck+height*.5f),180);
}
void civic_landscape(Scene& sc,Rng rng,Rng district_rng) {
  const auto plots=coastal_plots(district_rng);
  const auto infill=infill_plots(district_rng);
  const std::array<Vec2,6> civic_route{{{-68,94},{-15,109},{44,119},{44,194},{127,213},{195,213}}};
  auto route_clear=[&](const std::vector<Vec2>& shape) {
    auto intersects=[&](Vec2 a,Vec2 b,float half_width) {
      Vec2 dir=normalize(b-a),side{-dir.y*half_width,dir.x*half_width};
      return polygons_overlap(shape,{a-side,b-side,b+side,a+side});
    };
    for(std::size_t i=0;i+1<civic_route.size();++i)
      if(intersects(civic_route[i],civic_route[i+1],6))return false;
    for(std::size_t i=0;i+1<kGardenApproach.size();++i)
      if(intersects(kGardenApproach[i],kGardenApproach[i+1],6))return false;
    for(std::size_t i=0;i+1<kWestPublicRoute.size();++i)
      if(intersects(kWestPublicRoute[i],kWestPublicRoute[i+1],7))return false;
    if(polygons_overlap(shape,kWestCivicCourt)||polygons_overlap(shape,moved_plan(kWestMarketReservation,kMarketMove))||polygons_overlap(shape,moved_plan(kMarketPromenade,kMarketMove))||polygons_overlap(shape,moved_plan(kCanalHexParcel,kHexMove))||polygons_overlap(shape,moved_plan(kCanalHexAccess,kHexMove))||polygons_overlap(shape,kGardenCompanionParcel))return false;
    for(const auto& reserved:transit_reserved_plans())if(polygons_overlap(shape,reserved))return false;
    for(const auto& corridor:arrival_primary_corridors())if(polygons_overlap(shape,corridor))return false;
    for(const auto& corridor:market_approach_footprints())if(polygons_overlap(shape,corridor))return false;
    return true;
  };
  auto available=[&](const std::vector<Vec2>& shape) {
    if(!route_clear(shape))return false;
    for(const auto& wing:civic_base_wings())if(polygons_overlap(shape,moved_plan(wing.footprint,kDomeMove)))return false;
    for(const auto& p:shape) {
      if(p.x<shore(p.y)+20||p.x>east_shore(p.y)-20)return false;
      if(std::abs(p.x-canal_centre(p.y))<canal_halfwidth(p.y)+14)return false;
      if(length(p-kDome)<60||length(p-kRing)<86)return false;
    }
    for(const auto& p:plots)if(!reserved_plot(p.footprint)&&polygons_overlap(shape,p.footprint))return false;
    for(const auto& p:infill)if(polygons_overlap(shape,p.footprint))return false;
    for(const auto& r:std::array<std::array<float,4>,5>{{{{73,111,25,52}},{{224,149,56,33}},{{148,153,22,24}},{{-345,405,70,75}},{{-125,490,47,41}}}})
      if(polygons_overlap(shape,plan_rect(r[2],r[3],{r[0],r[1]})))return false;
    return true;
  };
  std::vector<std::vector<Vec2>> occupied;
  // These are densely planted public gardens, with soil, a low retained edge,
  // clear paths and several mature canopy layers. They occupy residual land
  // created by the pedestrian reservations instead of leaving rectangular lawn.
  int bed_id=0;
  for(int row=0;row<18;++row)for(int col=0;col<15;++col) {
    Rng r=rng.child(20,row,col);
    Vec2 c{-210+col*27.f+r.range(-6,6),-205+row*31.f+r.range(-6,6)};
    if(c.x>108&&c.y>85)continue;
    auto bed=plan_superellipse(r.range(8,12),r.range(9,14),2.7f,18,c,r.range(-.7f,.7f));
    if(!available(bed))continue;
    bool overlap=false;for(const auto& p:occupied)if(polygons_overlap(bed,p)){overlap=true;break;}
    if(overlap)continue;
    occupied.push_back(plan_offset(bed,2));
    slab(sc.opaque,bed,kDeck+.23f,.28f,M_CONCRETE_WHITE);
    auto soil=plan_offset(bed,-.2f);slab(sc.opaque,soil,kDeck+.25f,.025f,M_SOIL);
    auto cover=plan_offset(soil,-.3f);slab(sc.opaque,cover,kDeck+.31f,.035f,M_GRASS);
    bool low_civic=c.y>-10&&c.y<90&&c.x>-40&&c.x<165;
    for(int t=0;t<(low_civic?2:6);++t) {
      Vec2 p=c+Vec2{r.range(-6,6),r.range(-7,7)};
      if(!low_civic)small_tree(sc,r.child(70+t),P3(p,kDeck+.32f),r.range(7.5f,12.5f),(bed_id+t)%5);
      for(int j=0;j<3;++j) {
        Vec2 q=p+Vec2{r.range(-1.9f,1.9f),r.range(-1.9f,1.9f)};
        plant(sc,r.child(100+t*3+j),P3(q,kDeck+.33f),r.range(.9f,1.5f),j);
      }
    }
    Vec2 edge=bed.front();
    gen_bench(sc,P3(edge+normalize(edge-c)*2,kDeck),std::atan2(c.x-edge.x,c.y-edge.y));
    if(bed_id%3==0)lamp(sc,P3(edge+normalize(edge-c)*3,kDeck));
    ++bed_id;
  }
}
void garden_companion(Scene& sc,Rng rng) {
  auto first=std::uint32_t(sc.opaque.indices.size());
  const Mat ceramic=lattice_ceramic(sc);
  slab(sc.opaque,kGardenCompanionParcel,kDeck,.3f,M_SIDEWALK);
  const auto plan=plan_circle(18,72,kGardenCompanion);
  box(sc,M_CONCRETE_WHITE,P3(kGardenCompanion,161.2f),{6.5f,160,6.5f});
  for(int floor=0;floor<80;++floor) {
    float y=kDeck+floor*4;
    slab(sc.opaque,plan,y,.20f,ceramic);
    for(int bay=0;bay<72;++bay) {
      float a=bay*2*kPi/72,b=(bay+1)*2*kPi/72;
      // Upper-bridge landing opens into the occupied companion floor71.
      if(floor==71&&std::abs((a+b)*.5f-1.403f)<.17f)continue;
      if(floor==0&&std::min(a,2*kPi-a)<.15f)continue;
      Vec3 p=P3(kGardenCompanion+Vec2{std::cos(a),std::sin(a)}*17.65f,y+.2f);
      Vec3 q=P3(kGardenCompanion+Vec2{std::cos(b),std::sin(b)}*17.65f,y+.2f);
      Emit glazing(&sc.opaque,floor>2?M_GLASS_BLUE:M_GLASS_CLEAR);
      glazing.element_random=rng.child(901).next();
      // One unwrapped metric facade preserves actual room/floor identities;
      // restarting every panel at UV zero repeated a single room up the tower.
      glazing.quad(q,p,p+Vec3{0,3.65f,0},q+Vec3{0,3.65f,0},
          QuadUV{{-b*17.65f,y+.2f},{-a*17.65f,y+.2f},
                 {-a*17.65f,y+3.85f},{-b*17.65f,y+3.85f}});
    }
    for(int bay=0;bay<24;++bay) {
      float a=bay*2*kPi/24;Vec3 p=P3(kGardenCompanion+Vec2{std::cos(a),std::sin(a)}*18.12f,y);
      ceramic_member(sc,p,p+Vec3{0,4,0},{std::cos(a),0,std::sin(a)},.20f,ceramic);
    }
    if(!sc.asset_library.resources.empty()&&(floor%3==0||floor==71))for(int room=0;room<4;++room) {
      float a=room*kPi*.5f;
      add_asset_instance(sc,"interior_lounge",P3(kGardenCompanion+Vec2{std::sin(a),std::cos(a)}*11.8f,y),a,.85f);
    }
  }
  slab(sc.opaque,plan,321.2f,.3f,ceramic);
  roof_garden(sc,rng.child(20),plan,kGardenCompanion,321.2f,1);
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),P3(kGardenCompanion,161.2f),180);
  ++sc.stats_towers;
}
void foreground(Scene& sc,Rng rng) {
  auto podium=plan_rounded_rect(65,70,14,12,kLattice);
  low_building(sc,rng.child(1),podium,kLattice,kDeck,4,1,true);
  auto spec=family(0,136,42.3f,0);spec.facade=FacadeKind::Curtain;
  spec.taper=.05f;spec.glass=M_GLASS_DARK;spec.base_scale=1;spec.base_floors=0;
  if(!sc.asset_library.resources.empty()) {
    MaterialDesc occupied=sc.materials[M_GLASS_DARK];
    occupied.name="foreground occupied glazing";occupied.flags=128u;
    occupied.base_color={.94f,.97f,.98f};occupied.roughness=.055f;
    occupied.room_w=1.5f;occupied.room_h=.98f;occupied.room_d=.96f;occupied.lit_probability=.012f;
    spec.glass=static_cast<Mat>(sc.materials.size());sc.materials.push_back(occupied);
  }
  tower(sc,spec,kLattice,kDeck+16,rng.child(2),2);
  auto lattice_first=std::uint32_t(sc.opaque.indices.size());
  foreground_exoskeleton(sc);
  northern_weather_envelope(sc);
  // Occupied loggias sit between the recessed curtain and the independent
  // ceramic frame. Their real slab edges and glass guards reveal the depth.
  MaterialDesc guard=sc.materials[M_GLASS_CLEAR];guard.name="foreground loggia guard";
  guard.flags=128u;guard.base_color={.94f,.97f,.98f};guard.roughness=.055f;
  guard.room_w=1.5f;guard.room_h=.98f;guard.room_d=.96f;guard.lit_probability=.012f;
  guard.metallic=0;guard.albedo_set="";
  Mat guard_id=static_cast<Mat>(sc.materials.size());sc.materials.push_back(guard);
  for(int floor=1;floor<136;++floor) {
    float t=float(floor)/136,y=kDeck+16+floor*4,scale=1-.05f*t*t;
    float inside=42.3f*scale+.03f,outside=44*scale+.35f;
    auto outer=plan_circle(outside,88,kLattice),inner=plan_circle(inside,88,kLattice);
    Emit deck(&sc.opaque,floor==65?M_MARBLE_WHITE:M_TERRAZZO),edge(&sc.opaque,M_BRONZE),glass(&sc.opaque,guard_id);
    deck.ring_cap(outer,inner,y);edge.wall(outer,y-.16f,y,true);
    edge.wall(inner,y-.16f,y,true,false);
    for(int bay=0;bay<88;++bay) {
      float a=bay*2*kPi/88,b=(bay+1)*2*kPi/88;
      // Door mouths match their supported deck widths. Split the curved
      // guard at the exact opening boundary rather than dropping coarse bays.
      std::vector<std::pair<float,float>> spans{{a,b}},openings;
      if(floor==65)openings={{kPi,.18f},{4.596f,.035f}};
      if(floor==67)openings={{4.596f,.035f}};
      for(auto [centre,half]:openings)for(float turn:{-2*kPi,0.f,2*kPi}) {
        std::vector<std::pair<float,float>> remaining;
        for(auto [begin,end]:spans) {
          float lo=centre+turn-half,hi=centre+turn+half;
          if(hi<=begin||lo>=end)remaining.push_back({begin,end});
          else {if(lo>begin)remaining.push_back({begin,lo});if(hi<end)remaining.push_back({hi,end});}
        }
        spans=std::move(remaining);
      }
      for(auto [begin,end]:spans) {
        Vec3 p{kLattice.x+std::cos(begin)*(outside-.10f),y,kLattice.y+std::sin(begin)*(outside-.10f)};
        Vec3 q{kLattice.x+std::cos(end)*(outside-.10f),y,kLattice.y+std::sin(end)*(outside-.10f)};
        glass.quad_metric(p+Vec3{0,.14f,0},q+Vec3{0,.14f,0},q+Vec3{0,1.08f,0},p+Vec3{0,1.08f,0});
        edge.tube(p+Vec3{0,1.10f,0},q+Vec3{0,1.10f,0},.018f,5);
      }
    }
  }
  sc.register_range(lattice_first,std::uint32_t(sc.opaque.indices.size()),P3(kLattice,kDeck+16+272),280);
  // One west-side garden belongs to the same tower. The floor65 loggia
  // connects its public walk to the occupied tower and upper companion stair.
  const auto garden_floors=garden_supported_floor_plans();
  for(const auto& floor:garden_floors)slab(sc.opaque,floor,kGardenY,.7f,M_MARBLE_WHITE);
  const Mat frame_material=lattice_ceramic(sc);
  const Vec3 upper{-393.534f,309.2f,375.827f},left_upper{-393,309.2f,316.6f};
  const Vec3 lower{-385.5f,249.2f,361.8f},floor_tie{-374.5f,249.2f,373.6f};
  // The low outrigger is visibly triangulated to two structural floors. Its
  // outer journal lies beyond the guard, so the walkway never cuts through it.
  ceramic_member(sc,floor_tie,lower,{0,1,0},2.3f,frame_material);
  ceramic_member(sc,{-374.5f,241.2f,373.6f},lower,{-.5295f,0,.8483f},2.0f,frame_material);
  build_sculpted_garden_frame(sc,kGardenJointPosition,upper,left_upper,lower,
                              frame_material,{-.9994f,0,-.034f});
  ceramic_member(sc,{-374.5f,309.2f,373.6f},upper,{0,1,0},2.0f,frame_material);
  ceramic_member(sc,{-374.5f,301.2f,373.6f},upper,{-.5295f,0,.8483f},1.7f,frame_material);
  // Cantilever floor beams and diagonal knees return the planted wing's load
  // to the real tower floor, rather than decorative isolated support poles.
  for(float z:{402.f,414.f}) {
    float dz=z-kLattice.y;float x=kLattice.x-std::sqrt(43.0f*43.0f-dz*dz);
    ceramic_member(sc,{x,kGardenY-.35f,z},{-416,kGardenY-.35f,z},{0,1,0},1.35f,frame_material);
    ceramic_member(sc,{x,kGardenY-8,z},{-413,kGardenY-.7f,z},{0,1,0},1.15f,frame_material);
  }
  ceramic_member(sc,{-386,257.2f,390},{-413,kGardenY-.7f,390},{0,1,0},1.8f,frame_material);
  ceramic_member(sc,{-387,269.2f,397},{-411,kGardenY-.7f,370},{0,1,0},1.5f,frame_material);
  ceramic_member(sc,{-388,269.2f,400},{-402,kGardenY-.7f,376},{0,1,0},1.25f,frame_material);
  // The transparent guard follows the union's outer perimeter. Internal floor
  // seams and the real connection to the tower's loggia remain unobstructed.
  auto cross2=[](Vec2 a,Vec2 b){return a.x*b.y-a.y*b.x;};
  std::vector<GardenGuardSpan> garden_guard_edges;
  for(std::size_t floor=0;floor<garden_floors.size();++floor) {
    const auto& polygon=garden_floors[floor];float winding=plan_area(polygon)>0?1.f:-1.f;
    for(std::size_t edge=0;edge<polygon.size();++edge) {
      Vec2 a=polygon[edge],b=polygon[(edge+1)%polygon.size()],d=b-a;
      Vec2 outward=normalize(Vec2{d.y,-d.x})*winding;std::vector<float> cuts{0,1};
      for(std::size_t other=0;other<garden_floors.size();++other)if(other!=floor) {
        const auto& boundary=garden_floors[other];
        for(std::size_t j=0;j<boundary.size();++j) {
          Vec2 p=boundary[j],q=boundary[(j+1)%boundary.size()],e=q-p;float denominator=cross2(d,e);
          if(std::abs(denominator)<1e-6f) {
            if(std::abs(cross2(p-a,d))<1e-3f)for(Vec2 point:{p,q}) {
              float t=dot(point-a,d)/dot(d,d);if(t>0&&t<1)cuts.push_back(t);
            }
          } else {
            float t=cross2(p-a,e)/denominator,u=cross2(p-a,d)/denominator;
            if(t>0&&t<1&&u>=0&&u<=1)cuts.push_back(t);
          }
        }
      }
      // Split where the perimeter meets the annular walking floor.
      Vec2 ac=a-kLattice;float qa=dot(d,d),qb=2*dot(ac,d),qc=dot(ac,ac)-43.84f*43.84f;
      float discriminant=qb*qb-4*qa*qc;
      if(discriminant>0)for(float sign:{-1.f,1.f}) {
        float t=(-qb+sign*std::sqrt(discriminant))/(2*qa);if(t>0&&t<1)cuts.push_back(t);
      }
      std::sort(cuts.begin(),cuts.end());
      for(std::size_t i=0;i+1<cuts.size();++i) {
        if(cuts[i+1]-cuts[i]<1e-5f)continue;
        Vec2 p=a+d*cuts[i],q=a+d*cuts[i+1],mid=(p+q)*.5f;bool internal=length(mid-kLattice)<43.84f;
        for(std::size_t other=0;other<garden_floors.size();++other)if(other!=floor)
          // Stay within the local boundary neighbourhood: a 2cm probe can
          // jump across the narrow exposed wedge at a rounded floor junction.
          internal|=point_in_polygon(garden_floors[other],mid+outward*.00025f);
        if(!internal)garden_guard_edges.push_back({P3(p,kGardenY),P3(q,kGardenY)});
      }
    }
  }
  build_garden_guard(sc,garden_guard_edges);
  // The original tower has real furnished rooms on all sides, including the
  // close right-hand curtain wall now revealed by the west balcony.
  if(!sc.asset_library.resources.empty())for(int floor=3;floor<134;++floor)for(int bay=0;bay<26;++bay) {
    if(!rng.child(400+floor*26+bay).chance(.48f))continue;
    float a=bay*2*kPi/26;Vec2 outward{std::sin(a),std::cos(a)};
    float t=float(floor)/136;Vec2 room=kLattice+outward*(37.3f-2.2f*t*t);
    add_asset_instance(sc,"interior_lounge",P3(room,kDeck+16+floor*4),a,1.f);
  }
  // A real two-floor switchback reaches the upper bridge from floor65. Its
  // treads stay outside the curtain and connect through open loggia guards.
  for(int floor=0;floor<2;++floor) {
    float y=kGardenY+floor*4;
    box(sc,M_MARBLE_WHITE,{kUpperStair.x,y-.13f,kUpperStair.y+3.2f},{3,.13f,1.3f});
    box(sc,M_MARBLE_WHITE,{kUpperStair.x,y+2-.13f,kUpperStair.y-3.2f},{3.6f,.13f,1.3f});
    for(int step=0;step<12;++step) {
      float t=(step+.5f)/12.f;
      box(sc,frame_material,{kUpperStair.x-1.5f,y+(step+1)/6.f-1/12.f,kUpperStair.y+2-t*4},{1.3f,1/12.f,.173f});
      box(sc,frame_material,{kUpperStair.x+1.5f,y+2+(step+1)/6.f-1/12.f,kUpperStair.y-2+t*4},{1.3f,1/12.f,.173f});
    }
    rail(sc,{kUpperStair.x-2.8f,y,kUpperStair.y+2},{kUpperStair.x-2.8f,y+2,kUpperStair.y-2});
    rail(sc,{kUpperStair.x+2.8f,y+2,kUpperStair.y-2},{kUpperStair.x+2.8f,y+4,kUpperStair.y+2});
    for(float sign:{-1.f,1.f}) {
      const float root_x=kUpperStair.x+sign*3.4f,bearing_x=kUpperStair.x+sign*3.1f;
      // The intermediate landing is two metres above this flight's start.
      // The knee's upper ceramic face meets a bearing directly under that
      // landing, rather than stopping below it in empty air.
      ceramic_member(sc,{root_x,y-8,363},{bearing_x,y+1.40f,353.2f},{1,0,0},.8f,frame_material);
      box(sc,M_BRONZE,{bearing_x,y+1.71f,353.2f},{.43f,.035f,.56f});
      for(float dz:{-.39f,.39f})
        Emit(&sc.opaque,M_BRONZE).tube({bearing_x,y+1.63f,353.2f+dz},
                                     {bearing_x,y+1.81f,353.2f+dz},.052f,8,true);
      rail(sc,{kUpperStair.x+sign*3.5f,y+2,351.95f},
              {kUpperStair.x+sign*3.5f,y+2,353.9f});
    }
    rail(sc,{kUpperStair.x-3.5f,y+2,351.95f},{kUpperStair.x+3.5f,y+2,351.95f});
  }
  box(sc,M_MARBLE_WHITE,{kUpperStair.x,kGardenY+8-.13f,kUpperStair.y+3.2f},{3,.13f,1.3f});
  bridge(sc,P3(kUpperBridgeOriginal,kGardenY-.3f),{kUpperStair.x,kGardenY-.3f,kUpperStair.y+3.2f},3.2f,0,.7f,.7f);
  // The upper connector and diagonal bridge share a single open junction.
  // Only its exposed east side needs a guard; the west edge lies inside the
  // other deck and must not fence off the actual walking connection.
  Emit(&sc.opaque,M_WHITE_METAL).beam(P3(kUpperBridgeOriginal,kGardenY+7.7f),
      {kUpperStair.x,kGardenY+7.7f,kUpperStair.y+3.2f},3.2f,.6f);
  rail(sc,{kUpperStair.x+1.48f,kGardenY+8,361.5f},
          {kUpperStair.x+1.48f,kGardenY+8,360.3f});
  bridge(sc,P3(kUpperBridgeOriginal,kGardenY+8-.3f),P3(kUpperBridgeCompanion,kGardenY+8-.3f),3.2f,1,3.6f,1.3f);
  // The eastern neighbor is a low-rise occupied frontage. Its obsolete high
  // connector is removed as a complete structure, and the tower guard closes.
  Vec2 neighbor{-125,490};
  // Keep the occupied frontage behind the full quay, including facade-module
  // overhangs. Its height, floor count and remaining rounded sides are retained.
  const Vec2 quay_edge{canal_centre(neighbor.y)+22.f,neighbor.y};
  const auto neighbor_plan=clip_halfplane(plan_rounded_rect(41,36,9,10,neighbor),
      quay_edge+riverfront::across*4.75f,riverfront::across);
  low_building(sc,rng.child(59),neighbor_plan,neighbor,kDeck,4,1,true);
}
void canal_hex_landmark(Scene& sc,Rng rng) {
  constexpr float base=13.2f,radius=18.117f,body_top=89.2f,roof=93.2f;
  const Mat ceramic=lattice_ceramic(sc);
  auto material=[&](MaterialDesc recipe,const char* name) {
    recipe.name=name;const auto id=static_cast<Mat>(sc.materials.size());
    sc.materials.push_back(std::move(recipe));return id;
  };
  MaterialDesc bronze_recipe=sc.materials[M_BRONZE];
  bronze_recipe.base_color={.24f,.145f,.072f};bronze_recipe.roughness=.30f;
  const Mat bronze=material(bronze_recipe,"canal tower dark brushed bronze");
  MaterialDesc glass_recipe=sc.materials[M_GLASS_BRONZE];
  glass_recipe.base_color={.54f,.39f,.245f};glass_recipe.metallic=.54f;
  glass_recipe.roughness=.10f;glass_recipe.room_h=4;glass_recipe.room_w=3.1f;
  glass_recipe.tint2={.79f,.69f,.51f};glass_recipe.lit_probability=.27f;
  const Mat shaft_glass=material(glass_recipe,"canal tower bronze occupied glazing");
  glass_recipe.base_color={.37f,.285f,.20f};glass_recipe.metallic=.64f;
  glass_recipe.lit_probability=.15f;
  const Mat neck_glass=material(glass_recipe,"canal tower dark clerestory glazing");
  MaterialDesc rim_recipe=sc.materials[M_SILVER];
  rim_recipe.base_color={.64f,.60f,.49f};rim_recipe.roughness=.30f;rim_recipe.metallic=.83f;
  const Mat rim=material(rim_recipe,"canal tower champagne silver crown");
  auto first=std::uint32_t(sc.opaque.indices.size());
  auto local=[](float s,float t){return riverfront::point(s,t)-kHexMove;};
  auto plan=[&](float s0,float s1,float t0,float t1) {
    return std::vector<Vec2>{local(s0,t0),local(s1,t0),local(s1,t1),local(s0,t1)};
  };
  const Vec3 across{riverfront::across.x,0,riverfront::across.y};
  const Vec3 along{riverfront::along.x,0,riverfront::along.y};
  const float survey_yaw=std::atan2(-riverfront::across.y,riverfront::across.x);
  MaterialDesc paving_recipe=sc.materials[M_PLAZA];
  paving_recipe.base_color={.49f,.55f,.53f};paving_recipe.roughness=.66f;
  paving_recipe.flags&=~512u;
  const Mat terrace_stone=material(paving_recipe,"canal garden dry grey stone");
  MaterialDesc arcade_recipe=sc.materials[M_GLASS_BRONZE];
  arcade_recipe.base_color={.67f,.55f,.39f};arcade_recipe.room_h=4;
  arcade_recipe.room_w=5.6f;arcade_recipe.lit_probability=.44f;
  const Mat arcade_glass=material(arcade_recipe,"canal podium four metre occupied bays");
  const auto lower=plan(37,144,20,188);
  const std::vector<std::vector<Vec2>> courts{plan(100,132,43,71),plan(99,132,141,168)};
  const std::vector<std::vector<Vec2>> middle{
    plan(41,86,26,180),plan(86,139,76,134),plan(86,135,26,41),plan(86,136,173,180)};
  const std::vector<std::vector<Vec2>> upper{
    plan(47,96,78,132),plan(49,81,32,67),plan(48,83,145,174)};
  const std::vector<std::vector<Vec2>> walks{
    plan(96,147.96f,105,111),plan(49,58,67,78),plan(55,63,132,145)};
  slab(sc.opaque,kCanalHexParcel,kDeck,.55f,M_SIDEWALK);
  slab(sc.opaque,kCanalHexAccess,kDeck,.55f,M_SIDEWALK);
  auto plate=[&](const std::vector<Vec2>& footprint,float y,float thickness,Mat mat) {
    std::vector<std::vector<Vec2>> pieces{footprint};
    for(const auto& court:courts) {
      std::vector<std::vector<Vec2>> next;
      for(const auto& piece:pieces) {
        if(!polygons_overlap(piece,court)){next.push_back(piece);continue;}
        auto parts=subtract_convex(piece,court,.01f);
        for(auto& part:parts)next.push_back(std::move(part));
      }
      pieces=std::move(next);
    }
    for(const auto& piece:pieces)slab(sc.opaque,piece,y,thickness,mat);
  };
  auto frontage=[&](const std::vector<Vec2>& footprint,float lo,float hi,float recess) {
    for(std::size_t edge=0;edge<footprint.size();++edge) {
      const Vec2 a=footprint[edge],b=footprint[(edge+1)%footprint.size()];
      const Vec2 d=normalize(b-a),out{d.y,-d.x};
      const float width=length(b-a);const int bays=std::max(1,int(std::ceil(width/5.8f)));
      for(int bay=0;bay<bays;++bay) {
        const Vec2 left=a+d*(width*bay/bays),right=a+d*(width*(bay+1)/bays);
        const Vec3 p=P3(left+d*.19f-out*recess,lo+.26f);
        const Vec3 q=P3(right-d*.19f-out*recess,lo+.26f);
        if(lo>kDeck+.01f||bay%7!=3) {
          const auto start=sc.opaque.vertices.size();
          Emit pane(&sc.opaque,arcade_glass);pane.element_random=rng.child(300+int(edge)*100+bay).next();
          pane.quad_metric(q,p,p+Vec3{0,hi-lo-.65f,0},q+Vec3{0,hi-lo-.65f,0});
          for(auto i=start;i<sc.opaque.vertices.size();++i)
            sc.opaque.vertices[i].aux.y=sc.opaque.vertices[i].position.y-kDeck;
        }
        // Deep jamb returns join recessed glazing to the projecting outer
        // ledge. Their sheltered arcade remains a usable ground-level walk.
        Emit(&sc.opaque,ceramic).beam(P3(left,lo),P3(left,hi-.25f),.24f,.38f,
          {out.x,0,out.y});
        Emit(&sc.opaque,bronze).beam(P3(left-out*recess,hi-.42f),P3(left,hi-.42f),.15f,.22f);
        if(bay%3==1) {
          const Vec3 light=P3((left+right)*.5f-out*(recess*.62f),hi-.42f);
          Emit(&sc.opaque,M_LOBBY_LIGHT).box(light,{.70f,.045f,.075f},
            {d.x,0,d.y},{0,1,0},{-out.x,0,-out.y});
        }
      }
    }
  };
  plate(lower,5.2f,.34f,terrace_stone);frontage(lower,kDeck,5.2f,2.15f);
  for(auto court:courts) {
    std::reverse(court.begin(),court.end());frontage(court,kDeck,5.2f,1.15f);
  }
  for(const auto& footprint:middle) {
    plate(footprint,9.2f,.34f,terrace_stone);frontage(footprint,5.2f,9.2f,.92f);
  }
  for(const auto& footprint:upper) {
    plate(footprint,base,.34f,terrace_stone);frontage(footprint,9.2f,base,.78f);
  }
  for(const auto& footprint:walks)plate(footprint,base,.38f,ceramic);
  // The upper public spine is open throughout; its narrow end is carried by
  // real columns and beams instead of an unsupported extension of a roof.
  for(float s:{100.f,112.f,124.f,136.f,145.f})for(float t:{105.3f,110.7f}) {
    const float foot=s<139?9.2f:kDeck;
    Emit(&sc.opaque,ceramic).beam(P3(local(s,t),foot),P3(local(s,t),base-.38f),.28f,.34f);
  }
  for(float t:{105.3f,110.7f})
    Emit(&sc.opaque,bronze).beam(P3(local(96,t),base-.42f),P3(local(147.96f,t),base-.42f),.22f,.28f);
  for(const auto& footprint:{walks[1],walks[2]}) {
    const auto samples=plan_sample(footprint,7);
    for(const auto& p:samples.points)
      Emit(&sc.opaque,ceramic).beam(P3(p,9.2f),P3(p,base-.38f),.20f,.26f);
  }
  auto guard=[&](const std::vector<std::vector<Vec2>>& regions,float y) {
    for(std::size_t n=0;n<regions.size();++n)for(std::size_t e=0;e<regions[n].size();++e) {
      const Vec2 a=regions[n][e],b=regions[n][(e+1)%regions[n].size()],d=b-a;
      const auto mounting=plan_offset(regions[n],-.09f);
      const Vec2 rail_a=mounting[e],rail_b=mounting[(e+1)%mounting.size()];
      const float length2=dot(d,d);std::vector<float> cuts{0,1};
      for(const auto& region:regions)for(const auto& p:region) {
        const float t=dot(p-a,d)/length2;
        if(t>0&&t<1)cuts.push_back(t);
      }
      if(y==base)for(float opening_t:{105.8f,110.2f}) {
        const float t=dot(local(147.96f,opening_t)-a,d)/length2;
        if(t>0&&t<1)cuts.push_back(t);
      }
      std::sort(cuts.begin(),cuts.end());
      const Vec2 out=normalize(Vec2{d.y,-d.x});
      bool active=false;float start=0,end=0;
      auto flush=[&] {
        if(active&&end-start>.001f) {
          rail(sc,P3(rail_a+(rail_b-rail_a)*start,y),P3(rail_a+(rail_b-rail_a)*end,y),true);
        }
        active=false;
      };
      for(std::size_t k=0;k+1<cuts.size();++k) {
        if(cuts[k+1]-cuts[k]<.00001f)continue;
        const Vec2 p=a+d*((cuts[k]+cuts[k+1])*.5f)+out*.025f;
        bool covered=false;
        for(std::size_t m=0;m<regions.size();++m)
          if(m!=n&&point_in_polygon(regions[m],p)){covered=true;break;}
        // The landing meets the external stair without a guard across its mouth.
        const Vec2 address=riverfront::coordinates(p+kHexMove);
        if(y==base&&address.x>147.8f&&address.y>105.8f&&address.y<110.2f)covered=true;
        if(covered){flush();continue;}
        if(!active){active=true;start=cuts[k];}end=cuts[k+1];
      }
      flush();
    }
  };
  guard({lower},5.2f);
  for(const auto& court:courts) {
    const auto mounting=plan_offset(court,.09f);
    for(std::size_t i=0;i<mounting.size();++i)
      rail(sc,P3(mounting[i],5.2f),P3(mounting[(i+1)%mounting.size()],5.2f),true);
  }
  guard(middle,9.2f);
  auto upper_walks=upper;upper_walks.insert(upper_walks.end(),walks.begin(),walks.end());
  guard(upper_walks,base);
  // Retained garden masses occupy the terraces around branching stone paths.
  // A bed is one rounded soil outline with real cut-outs for rooms and walks;
  // triangulation seams never become repeated strips of pale coping.
  auto cut_plans=[](std::vector<std::vector<Vec2>> pieces,
                    const std::vector<std::vector<Vec2>>& holes) {
    for(const auto& hole:holes) {
      std::vector<std::vector<Vec2>> next;
      for(const auto& p:pieces) {
        if(!polygons_overlap(p,hole)){next.push_back(p);continue;}
        for(auto& part:subtract_convex(p,hole,.01f))next.push_back(std::move(part));
      }
      pieces=std::move(next);
    }
    return pieces;
  };
  auto inside_any=[](const std::vector<std::vector<Vec2>>& polygons,Vec2 p) {
    for(const auto& polygon:polygons)if(point_in_polygon(polygon,p))return true;
    return false;
  };
  // Mature crowns may overhang a terrace, but never grow through an occupied
  // floor or the cylindrical tower. Test their actual uniformly scaled mesh.
  auto occupied=[&](Vec3 p) {
    const Vec2 q{p.x,p.z};
    if(p.y>kDeck+.08f&&p.y<5.18f&&point_in_polygon(lower,q)&&!inside_any(courts,q))return true;
    if(p.y>5.22f&&p.y<9.18f&&inside_any(middle,q))return true;
    if(p.y>9.22f&&p.y<base-.02f&&inside_any(upper,q))return true;
    if(p.y>base-.38f&&p.y<base&&inside_any(walks,q))return true;
    return p.y>base+.02f&&length(q-kCanalHex)<radius+.78f;
  };
  auto bed=[&](float s0,float s1,float t0,float t1,float floor,int key,int trees,
               std::vector<std::vector<Vec2>> holes=std::vector<std::vector<Vec2>>{}) {
    const float half_s=(s1-s0)*.5f,half_t=(t1-t0)*.5f;
    auto edge=plan_rounded_rect(half_s,half_t,std::min({2.8f,half_s*.65f,half_t*.65f}),5,
                                {(s0+s1)*.5f,(t0+t1)*.5f});
    for(auto& p:edge)p=local(p.x,p.y);
    const auto soil=inset_convex_plan(edge,.20f);
    std::vector<std::vector<Vec2>> soil_holes;
    for(const auto& hole:holes)soil_holes.push_back(plan_offset(hole,.20f));
    const auto edge_parts=cut_plans({edge},holes);
    const auto soil_parts=cut_plans({soil},soil_holes);
    for(const auto& p:edge_parts)slab(sc.opaque,p,floor+.54f,.54f,ceramic);
    for(const auto& p:soil_parts)slab(sc.opaque,p,floor+.55f,.035f,M_SOIL);
    const int ns=std::max(1,int((s1-s0)/1.6f)),nt=std::max(1,int((t1-t0)/1.6f));
    for(int x=0;x<ns;++x)for(int z=0;z<nt;++z) {
      Rng r=rng.child(key).child(x,z);
      const Vec2 q=local(s0+.45f+(s1-s0-.9f)*(x+r.range(.15f,.85f))/ns,
                        t0+.45f+(t1-t0-.9f)*(z+r.range(.15f,.85f))/nt);
      if(!inside_any(soil_parts,q))continue;
      // Low overlapping groundcover, fern sprays and fewer upright accents
      // leave occasional dark soil visible beneath the taller tree groups.
      const int choice=(x*7+z*3+key)%10;
      const int species=choice<6?3:choice<8?1:choice==8?0:2;
      const auto before=sc.asset_instances.size();
      plant(sc,r,P3(q,floor+.55f),r.range(.92f,1.32f),species);
      if(sc.asset_instances.size()>before) {
        const auto& instance=sc.asset_instances.back();
        const auto& mesh=sc.asset_library.resources[instance.resource].mesh;
        bool collision=false;
        for(const auto& v:mesh.vertices)if(occupied(asset_transform_point(instance,v.position))) {
          collision=true;break;
        }
        if(collision)sc.asset_instances.pop_back();
      }
    }
    std::vector<Vec2> trunks;
    for(int tree=0;tree<trees;++tree)for(int attempt=0;attempt<16;++attempt) {
      Rng r=rng.child(key+731).child(tree,attempt);
      const Vec2 q=local(r.range(s0+.55f,s1-.55f),r.range(t0+.55f,t1-.55f));
      if(!inside_any(soil_parts,q))continue;
      bool crowded=false;for(const auto& trunk:trunks)if(length(trunk-q)<4.0f)crowded=true;
      if(crowded)continue;
      const int species=tree%5==0?4:0;
      const float height=r.range(8.5f,12.2f);
      const auto before=sc.asset_instances.size();
      small_tree(sc,r,P3(q,floor+.55f),height,species);
      if(sc.asset_instances.size()>before) {
        const auto& instance=sc.asset_instances.back();
        const auto& mesh=sc.asset_library.resources[instance.resource].mesh;
        bool collision=false;
        for(const auto& v:mesh.vertices)if(occupied(asset_transform_point(instance,v.position))) {
          collision=true;break;
        }
        if(collision){sc.asset_instances.pop_back();continue;}
      }
      trunks.push_back(q);break;
    }
  };
  // The low commercial frontage stays exposed under these planted shoulders.
  bed(38.5f,40.5f,30,73,5.2f,1000,0);
  bed(38.5f,40.5f,138,178,5.2f,1010,0);
  bed(87.6f,98.0f,44,70,5.2f,1020,5);
  bed(133.5f,142.5f,44,72,5.2f,1030,5);
  bed(136.5f,142.5f,27,41,5.2f,1040,2);
  bed(87.6f,97.3f,138,171,5.2f,1050,6);
  bed(133.5f,142.5f,137,172,5.2f,1060,6);
  bed(100.5f,131,72.5f,74.5f,5.2f,1070,0);
  bed(100.5f,131,136,139,5.2f,1080,0);
  bed(140.5f,142.5f,78,132,5.2f,1090,0);
  bed(39,141.5f,181.7f,186.5f,5.2f,1100,14);
  // Broad middle gardens wrap usable outdoor rooms instead of outlining them
  // with a single thin hedge. The six-metre upper spine stays completely open.
  bed(98,136.5f,78,102.5f,9.2f,1120,12,
      {plan(112.5f,119.5f,92.5f,100),plan(114.5f,117.5f,99,105)});
  bed(98,136.5f,112.5f,132,9.2f,1140,10,
      {plan(115.5f,122.5f,112,119.5f),plan(117.5f,120.5f,109,113)});
  bed(60,84,68.5f,76,9.2f,1160,4);
  bed(64.5f,84,133.5f,143.5f,9.2f,1180,4);
  bed(42.5f,53.5f,133.5f,143.5f,9.2f,1200,2);
  bed(42,45.5f,80,130,9.2f,1220,3);
  bed(42.5f,47.5f,34,65,9.2f,1240,2);
  bed(42.5f,46.5f,147,173,9.2f,1260,2);
  // Two upper garden rooms have clear pergolas and paths cut into a continuous
  // irregular planted envelope. Their real floor supports every retained bed.
  bed(50.5f,79.5f,33.5f,65.5f,base,1280,10,
      {plan(57.5f,72.5f,40.5f,53.5f),plan(62.5f,67.5f,52,68)});
  bed(49.5f,81.5f,146.5f,172.5f,base,1300,9,
      {plan(57.5f,72.5f,151.5f,164.5f),plan(62.5f,67.5f,144,153)});
  bed(48.5f,94.5f,79.5f,130.5f,base,1320,12,
      {plan_circle(radius+2.2f,72,kCanalHex),plan(87,98,103,113)});
  // Courtyard trees root on the real ground slab and grow through the open
  // courts; the lounge footprints and their branching approaches remain open.
  bed(101.5f,130.5f,44.5f,69.5f,kDeck,1340,6,
      {plan(110.5f,119.5f,50,58),plan(113,117,42,51),plan(119,133,52.5f,56.5f)});
  bed(100.5f,130.5f,142.5f,166.5f,kDeck,1360,6,
      {plan(110.5f,119.5f,151,159),plan(113,117,158,169),plan(119,133,153.5f,157.5f)});
  if(!sc.asset_library.resources.empty()&&!arrival_blockout) {
    // Small outdoor rooms stay inside their real roof or courtyard, with
    // uniform-size furniture and clear branching circulation around them.
    for(const auto& p:std::array<Vec3,6>{{{118,5.2f,37},{115,5.2f,177},
        {116,9.2f,96},{119,9.2f,116},{65,base,47},{65,base,158}}}) {
      add_asset_instance(sc,"street_table",P3(local(p.x,p.z),p.y),survey_yaw,1.f);
      for(float sign:{-1.f,1.f})
        add_asset_instance(sc,"street_seat",P3(local(p.x+sign*1.25f,p.z),p.y),survey_yaw-sign*kPi*.5f,1.f);
    }
    for(float t:{54.f,155.f})
      add_asset_instance(sc,"interior_lounge",P3(local(115,t),kDeck),survey_yaw,.85f);
    // Reuse the rooted, bent climber over narrow coping channels. Local +Z
    // clears the outer roof edge before the stems descend past the frontage.
    for(float t:{38.f,52.f,64.f,146.f,158.f,171.f}) {
      const auto edge=plan(143.47f,143.99f,t-1.3f,t+1.3f);
      slab(sc.opaque,edge,5.74f,.54f,ceramic);
      slab(sc.opaque,plan(143.53f,143.93f,t-1.24f,t+1.24f),5.68f,.025f,M_SOIL);
      stage_market_canopy_climbers(sc,P3(local(143.78f,t),5.68f),survey_yaw+kPi*.5f,.85f);
    }
  }
  // Two small open shade structures share the actual upper floor. Their
  // planted neighbors remain separate and their timber seating stays below.
  for(float t:{47.f,158.f}) {
    for(float s:{59.f,71.f})for(float dt:{-5.f,5.f})
      Emit(&sc.opaque,bronze).beam(P3(local(s,t+dt),base),P3(local(s,t+dt),base+3.4f),.14f,.14f);
    for(float s:{59.f,71.f})
      Emit(&sc.opaque,bronze).beam(P3(local(s,t-5.2f),base+3.4f),P3(local(s,t+5.2f),base+3.4f),.18f,.22f);
    for(int slat=0;slat<18;++slat) {
      const float z=t-5+slat*(10.f/17);
      Emit(&sc.opaque,M_PANEL_WARM).beam(P3(local(58.8f,z),base+3.50f),P3(local(71.2f,z),base+3.50f),.18f,.18f);
    }
  }
  // Real 166.7 mm risers and320 mm treads reach the preserved upper spine.
  // Twin stringers, cross bearings and two intermediate supports carry them.
  for(int step=0;step<riverfront::stair_steps;++step) {
    const float top=kDeck+(step+1)/6.f;
    const float s=riverfront::stair_bottom-(step+.5f)*riverfront::stair_run;
    Emit(&sc.opaque,ceramic).box(P3(local(s,riverfront::access_row),top-1/12.f),
      {.164f,1/12.f,2.2f},across,{0,1,0},along);
  }
  const float stair_top=riverfront::stair_bottom-riverfront::stair_steps*riverfront::stair_run;
  for(float t:{105.9f,110.1f}) {
    Emit handrail(&sc.opaque,bronze);
    auto rail_y=[&](float s){return kDeck+(riverfront::stair_bottom-s)/riverfront::stair_run/6.f+1.25f;};
    handrail.tube(P3(local(171.16f,t),rail_y(171.16f)),P3(local(147.8f,t),rail_y(147.8f)),.034f,8,true);
    // Posts bear at tread centres, not at the continuous slope datum between
    // risers. The top rail clears every nosing by at least1.08 metres.
    for(int step=0;step<72;step+=6) {
      const float s=riverfront::stair_bottom-(step+.5f)*riverfront::stair_run;
      handrail.tube(P3(local(s,t),kDeck+(step+1)/6.f),P3(local(s,t),rail_y(s)),.032f,8,true);
    }
    for(float s:{171.16f,147.8f})
      handrail.tube(P3(local(s,t),s>170?kDeck:base),P3(local(s,t),rail_y(s)),.032f,8,true);
    Emit(&sc.opaque,bronze).beam(P3(local(riverfront::stair_bottom,t),kDeck-.32f),
      P3(local(stair_top,t),base-.32f),.25f,.38f);
  }
  for(float s:{155.f,163.f}) {
    const float top=kDeck+(riverfront::stair_bottom-s)/riverfront::stair_run/6.f-.51f;
    for(float t:{105.9f,110.1f})
      Emit(&sc.opaque,ceramic).beam(P3(local(s,t),kDeck),P3(local(s,t),top),.34f,.34f);
    Emit(&sc.opaque,bronze).beam(P3(local(s,105.5f),top),P3(local(s,110.5f),top),.25f,.24f);
  }
  // The public entrance is an open arc in the real lobby wall. A central core
  // and radial columns bear the inhabited floors above it.
  const auto floor_plan=plan_circle(radius,72,kCanalHex);
  slab(sc.opaque,floor_plan,base,.22f,M_MARBLE_WHITE);
  box(sc,M_CONCRETE_WHITE,P3(kCanalHex,base+4),{7,4,7});
  for(int i=0;i<72;++i) {
    float a=i*2*kPi/72,b=(i+1)*2*kPi/72;
    if(std::min(a,2*kPi-a)<.23f||std::min(b,2*kPi-b)<.23f)continue;
    Vec3 p=P3(kCanalHex+Vec2{std::cos(a),std::sin(a)}*(radius-.5f),base+.15f);
    Vec3 q=P3(kCanalHex+Vec2{std::cos(b),std::sin(b)}*(radius-.5f),base+.15f);
    Emit(&sc.opaque,M_GLASS_CLEAR).quad_metric(q,p,p+Vec3{0,7.65f,0},q+Vec3{0,7.65f,0});
  }
  for(int i=0;i<12;++i) {
    float a=(i+.5f)*2*kPi/12;Vec3 p=P3(kCanalHex+Vec2{std::cos(a),std::sin(a)}*(radius-.9f),base);
    ceramic_member(sc,p,p+Vec3{0,8,0},{std::cos(a),0,std::sin(a)},.62f,ceramic);
  }
  if(!sc.asset_library.resources.empty()) {
    add_asset_instance(sc,"interior_lounge",P3(kCanalHex+Vec2{9,0},base),kPi*.5f,1.f);
    add_asset_instance(sc,"glass_door",P3(kCanalHex+Vec2{radius-.4f,0},base),kPi*.5f,1.7f);
  }
  constexpr float lattice_base=21.2f,lattice_top=76.8f;
  for(int floor=2;floor<19;++floor) {
    const float y=base+floor*4;
    const float bulge=1+.025f*std::sin((floor-2)/17.f*kPi);
    const auto p=plan_circle(radius*bulge-.5f,108,kCanalHex);
    const auto start=sc.opaque.vertices.size();
    Emit(&sc.opaque,y>=77.2f?neck_glass:shaft_glass).wall(p,y+.18f,y+3.82f,true);
    for(auto i=start;i<sc.opaque.vertices.size();++i)
      sc.opaque.vertices[i].aux.y=sc.opaque.vertices[i].position.y-base;
    // The physical four-metre floor and its room-grid datum agree. Thin dark
    // slab edges stay behind the outer cells, with an occupied bronze neck.
    slab(sc.opaque,plan_offset(p,.20f),y+4,.20f,bronze);
    for(int i=0;i<72;++i) {
      const float a=i*2*kPi/72;
      const Vec3 foot=P3(kCanalHex+Vec2{std::cos(a),std::sin(a)}*(radius*bulge-.46f),y+.18f);
      Emit(&sc.opaque,bronze).beam(foot,foot+Vec3{0,3.64f,0},.072f,.105f,
        {std::cos(a),0,std::sin(a)});
    }
  }
  // Thin elongated six-sided cells stop beneath the dark upper neck. Their
  // circumference is closed and shared edges are emitted only once.
  constexpr int columns=18;
  const float circumference=2*kPi*(radius+.45f),cell_w=circumference/columns,cell_h=14.8f;
  constexpr float cell_extent=lattice_top-lattice_base;
  auto point=[&](float u,float v) {
    const float a=u/(radius+.45f);
    const float bulge=1+.025f*std::sin(std::clamp(v/68.f,0.f,1.f)*kPi);
    return P3(kCanalHex+Vec2{std::cos(a),std::sin(a)}*((radius+.45f)*bulge),lattice_base+v);
  };
  std::set<std::array<int,4>> hex_edges;
  for(int row=0;row<6;++row)for(int column=0;column<columns;++column) {
    float x=(column+(row%2)*.5f)*cell_w,y=cell_h*.5f+row*cell_h*.75f;
    const std::array<Vec2,6> hex{{{x,y-cell_h*.5f},{x+cell_w*.5f,y-cell_h*.25f},{x+cell_w*.5f,y+cell_h*.25f},
       {x,y+cell_h*.5f},{x-cell_w*.5f,y+cell_h*.25f},{x-cell_w*.5f,y-cell_h*.25f}}};
    for(int edge=0;edge<6;++edge) {
      Vec2 a=hex[edge],b=hex[(edge+1)%6];
      if(a.y>=cell_extent&&b.y>=cell_extent)continue;
      if(a.y>cell_extent)a=a+(b-a)*((cell_extent-a.y)/(b.y-a.y));
      if(b.y>cell_extent)b=b+(a-b)*((cell_extent-b.y)/(a.y-b.y));
      auto canonical=[&](Vec2 p) {float u=std::fmod(p.x+circumference*2,circumference);return std::array<int,2>{int(std::lround(u*1000)),int(std::lround(p.y*1000))};};
      auto ka=canonical(a),kb=canonical(b);if(kb<ka)std::swap(ka,kb);
      if(!hex_edges.insert({ka[0],ka[1],kb[0],kb[1]}).second)continue;
      for(int piece=0;piece<3;++piece) {
        const Vec2 p=a+(b-a)*(piece/3.f),q=a+(b-a)*((piece+1)/3.f);
        const float angle=(p.x+q.x)*.5f/(radius+.45f);
        ceramic_member(sc,point(p.x,p.y),point(q.x,q.y),{std::cos(angle),0,std::sin(angle)},.43f,ceramic);
      }
    }
  }
  auto annulus=[&](float outer,float inner,float top,float thickness,Mat mat) {
    const auto a=plan_circle(outer,108,kCanalHex),b=plan_circle(inner,108,kCanalHex);
    Emit e(&sc.opaque,mat);e.ring_cap(a,b,top);e.ring_cap(a,b,top-thickness,false);
    e.wall(a,top-thickness,top,true);e.wall(b,top-thickness,top,true,false);
  };
  annulus(radius+.58f,radius+.12f,lattice_top+.08f,.18f,bronze);
  // The maintenance disk occupies seventy percent of the crown diameter.
  // A recessed dark roof and the open air between narrow formed rings make
  // their actual layered construction visible from the elevated city approach.
  MaterialDesc crown_recipe=sc.materials[M_DARK_METAL];
  crown_recipe.base_color={.095f,.073f,.046f};crown_recipe.roughness=.43f;
  const Mat crown_deck=material(crown_recipe,"canal crown dark bronze recessed roof");
  constexpr float disk_radius=13.6f,dark_roof_top=90.95f,disk_top=91.18f;
  slab(sc.opaque,plan_circle(radius-.12f,108,kCanalHex),dark_roof_top,.25f,crown_deck);
  slab(sc.opaque,plan_circle(disk_radius,108,kCanalHex),disk_top,.23f,M_MARBLE_WHITE);
  annulus(disk_radius+.20f,disk_radius-.10f,disk_top+.20f,.30f,rim);
  Emit(&sc.opaque,neck_glass).wall(plan_circle(radius-.5f,108,kCanalHex),body_top,dark_roof_top-.25f,true);
  for(int support=0;support<12;++support) {
    const float a=(support+.5f)*2*kPi/12;
    const Vec3 out{std::cos(a),0,std::sin(a)};
    const Vec3 foot=P3(kCanalHex,body_top)+out*(radius-1.3f);
    Emit(&sc.opaque,bronze).beam(foot,{foot.x,dark_roof_top-.36f,foot.z},.16f,.20f,out);
    Emit(&sc.opaque,bronze).beam(P3(kCanalHex,dark_roof_top-.36f),
      P3(kCanalHex,dark_roof_top-.36f)+out*(radius-.15f),.18f,.22f);
  }
  for(int level=0;level<4;++level) {
    const float y=body_top+.30f+level*1.11f;
    annulus(radius+1.30f,radius+.72f,y,.14f,level==0?ceramic:rim);
    annulus(radius+.83f,radius+.71f,y-.22f,.055f,bronze);
  }
  for(int i=0;i<54;++i) {
    const float a=i*2*kPi/54;const Vec3 outward{std::cos(a),0,std::sin(a)};
    const Vec3 foot=P3(kCanalHex,body_top)+outward*(radius+.73f);
    Emit(&sc.opaque,rim).beam(foot,foot+Vec3{0,5.08f,0},.065f,.10f,outward);
    Emit(&sc.opaque,bronze).beam(P3(kCanalHex,body_top)+outward*(radius-.4f),foot,.09f,.18f);
  }
  for(float y:{roof-.33f,roof+.18f,roof+.66f,roof+1.08f})
    annulus(radius+.81f,radius+.74f,y,.065f,rim);
  for(int i=0;i<18;++i) {
    const float a=i*2*kPi/18;
    const Vec3 p=P3(kCanalHex+Vec2{std::cos(a),std::sin(a)}*(disk_radius+.055f),disk_top+.225f);
    box(sc,M_LOBBY_LIGHT,p,{.09f,.025f,.09f});
  }
  Vec3 bounds_lo{1e30f,1e30f,1e30f},bounds_hi{-1e30f,-1e30f,-1e30f};
  for(auto i=first;i<sc.opaque.indices.size();++i) {
    const auto p=sc.opaque.vertices[sc.opaque.indices[i]].position;
    bounds_lo=vmin(bounds_lo,p);bounds_hi=vmax(bounds_hi,p);
  }
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),
    (bounds_lo+bounds_hi)*.5f,length(bounds_hi-bounds_lo)*.5f+.1f);
  ++sc.stats_towers;
}
void west_market(Scene& sc,Rng rng) {
  const Vec2 centre{-144,108};
  const auto envelope=plan_rounded_rect(22,46,8,12,centre);
  const auto forecourt=plan_rounded_rect(31,54,10,12,{-141,108});
  const Mat exposed_stone=static_cast<Mat>(market_paving_material(sc));
  slab(sc.opaque,forecourt,kDeck,.6f,exposed_stone);
  slab(sc.opaque,kMarketPromenade,kDeck,.6f,exposed_stone);
  slab(sc.opaque,envelope,7,.48f,M_WHITE_METAL);
  // A genuine upper mixed-use building carries the deep planted canopy.
  low_building(sc,rng.child(1),plan_rounded_rect(21,44,8,12,centre),centre,7,5,0,true);
  build_market_canopy(sc);
  box(sc,M_PANEL_WARM,{-163.4f,3.8f,108},{.28f,2.6f,37});
  box(sc,M_PANEL_WARM,{-144,3.8f,64},{14,2.6f,.25f});
  box(sc,M_PANEL_WARM,{-144,3.8f,152},{14,2.6f,.25f});
  for(int bay=0;bay<10;++bay) {
    const float z=70+bay*8.f;
    box(sc,M_BRONZE,{-122.5f,3.8f,z},{.12f,2.6f,.12f});
    box(sc,M_LOBBY_LIGHT,{-131,6.25f,z+2},{4.8f,.07f,.18f});
    sc.lights.push_back({{-131,5.7f,z+2},13,{1,.76f,.5f},2.8f});
    if(!sc.asset_library.resources.empty()) {
      add_asset_instance(sc,"facade_jamb",{-122.3f,kDeck,z},kPi*.5f,1.35f);
      add_asset_instance(sc,"window_lintel",{-122.3f,kDeck+4.7f,z+3.5f},kPi*.5f,1.7f);
      // Real panes enclose the market while alternate entrances remain open.
      if(bay%3!=1)add_asset_instance(sc,"glass_door",{-122.15f,kDeck,z+3.6f},kPi*.5f,{1.6f,1.6f,1});
    }
    if(bay%2==0&&bay!=8)garden(sc,rng.child(20+bay),{-123.2f,z},1.1f,2,kDeck,false,true);
  }
  build_market_structure(sc,lattice_ceramic(sc));
  // Planted lips span substantial roof area; the public front remains clear.
  garden(sc,rng.child(80),{-126,138},3.1f,10,7.02f,true,true);
  garden(sc,rng.child(81),{-128,88},4,13,7.02f,true,true);
  if(!sc.asset_library.resources.empty())for(int i=0;i<12;++i)
    stage_market_canopy_climbers(sc,{-119.22f,7.20f,72.f+i*6},kPi*.5f,.85f);
  for(float z:{83.f,107.f,131.f}) {
    auto puddle=plan_superellipse(2.6f,.63f,2.6f,24,{-109,z},.16f);
    slab(sc.opaque,puddle,kDeck+.022f,.009f,M_WATER);
    if(!sc.asset_library.resources.empty())add_asset_instance(sc,"street_drain",{-106.5f,kDeck-.055f,z},0,1.f);
    lamp(sc,{-102,kDeck,z+5},3.7f);
  }
}
void street_detail(Scene& sc,Rng rng) {
  // A continuous pedestrian surface joins the square to the occupied shop
  // frontages. Planting remains inside its retained beds at either edge.
  std::vector<Vec2> promenade{{-9,89},{62,89},{65,160},{108,211},{109,225},{-9,225}};
  std::vector<std::vector<Vec2>> paving{promenade};
  for(int i=0;i<8;++i) {
    float z=70+i*11.f;if(z<89)continue;
    auto drain=plan_rect(.23f,.72f,{44,z});std::vector<std::vector<Vec2>> next;
    for(const auto& p:paving) {
      auto parts=subtract_convex(p,drain);
      for(auto& part:parts)next.push_back(std::move(part));
    }
    paving=std::move(next);
  }
  for(const auto& p:paving)slab(sc.opaque,p,kDeck,.12f,M_PLAZA);
  slab(sc.opaque,{{-103,89},{-32,89},{-21,116},{-95,116}},kDeck,.12f,M_PLAZA);
  Vec2 c{58,116};
  auto building=plan_rounded_rect(14,47,4,8,{77,111});
  low_building(sc,rng.child(1),building,{77,111},kDeck+5.8f,4,0,true);
  slab(sc.opaque,plan_rounded_rect(17,48,5,8,{73,111}),7,.6f,M_WHITE_METAL);
  box(sc,M_PANEL_WARM,{87,3.8f,111},{.3f,2.6f,46});
  for(int bay=0;bay<11;++bay) {
    float z=68+bay*8.f;
    box(sc,M_BRONZE,{58,3.8f,z},{.14f,2.6f,.14f});
    box(sc,M_LOBBY_LIGHT,{69,6.55f,z+2},{4,.05f,.22f});
    sc.lights.push_back({{69,5.7f,z+2},11,{1,.72f,.41f},2.8f});
    for(int shelf=0;shelf<4;++shelf) {
      box(sc,M_BRONZE,{85.8f,1.8f+shelf*.85f,z+2},{.65f,.035f,2.6f});
      for(int item=0;item<5;++item)box(sc,item%2?M_PANEL_WARM:M_MARBLE_WHITE,{85.7f,2+shelf*.85f,z+item*.9f},{.16f,.18f,.22f});
    }
    gen_bench(sc,{66,kDeck,z+3},kPi*.5f);
    garden(sc,rng.child(20+bay),{55,z},1.2f,1.8f,kDeck,bay%3==0,true);
    if(!sc.asset_library.resources.empty()) {
      add_asset_instance(sc,"market_counter",{81,kDeck,z+1},kPi*.5f,1.f);
      add_asset_instance(sc,"pendant_lamp",{72,6.4f,z+2},0,1.f);
      add_asset_instance(sc,"street_table",{64,kDeck,z},0,1.f);
      add_asset_instance(sc,"street_seat",{63,kDeck,z-1.1f},0,1.f);
      add_asset_instance(sc,"street_seat",{65,kDeck,z+1.1f},kPi,1.f);
      add_asset_instance(sc,"facade_jamb",{86.5f,kDeck,z-1.5f},-kPi*.5f,1.f);
      add_asset_instance(sc,"window_lintel",{85,kDeck+3.6f,z+2},-kPi*.5f,1.f);
      if(bay%2==0)add_asset_instance(sc,"service_pipe",{85.5f,kDeck,z+3.5f},-kPi*.5f,1.f);
    }
  }
  for(int slat=0;slat<100;++slat)box(sc,M_BRONZE,{69,6.44f,65+slat*.93f},{12,.04f,.035f});
  for(int i=0;i<7;++i) {garden(sc,rng.child(70+i),{22.f,74+i*13.f},3,4,kDeck,true,true);lamp(sc,{30,kDeck,74+i*13.f});}
  // Low points retain a thin film of water; drains sit immediately beside it.
  for(int i=0;i<8;++i) {
    auto p=plan_superellipse(2.8f+i*.2f,.6f,2.6f,18,{40.f+i%3,70+i*11.f},.15f*i);
    slab(sc.opaque,p,kDeck+.022f,.009f,M_WATER);
    box(sc,M_DARK_METAL,{44,kDeck-.10f,70+i*11.f},{.22f,.04f,.72f});
    if(!sc.asset_library.resources.empty()) {
      add_asset_instance(sc,"street_drain",{44,kDeck-.055f,70+i*11.f},kPi*.5f,1.f);
      if(i%3==0)add_asset_instance(sc,"street_access_cover",{40,kDeck-.052f,73+i*11.f},.2f,1.f);
    }
  }
}
void landing(Scene& sc,Rng rng) {
  constexpr float y=34,radius=24;
  const Vec2 centre{224,144};
  const Mat ceramic=lattice_ceramic(sc);
  MaterialDesc stone=sc.materials[market_paving_material(sc)];
  stone.name="exposed landing stone";
  const Mat exterior=static_cast<Mat>(sc.materials.size());sc.materials.push_back(stone);
  MaterialDesc glazing=sc.materials[M_GLASS_CLEAR];
  glazing.name="landing curved occupied glass";glazing.flags=128u;
  glazing.base_color={.93f,.97f,.98f};glazing.roughness=.055f;glazing.metallic=0;
  glazing.room_w=1.5f;glazing.room_h=.985f;glazing.room_d=.96f;glazing.lit_probability=.018f;
  glazing.albedo_set="";glazing.normal_strength=0;
  const Mat pane=static_cast<Mat>(sc.materials.size());sc.materials.push_back(glazing);
  auto first=std::uint32_t(sc.opaque.indices.size());
  // A joined irregular terrace carries the modest canopies, planting and
  // existing public stair mouth. The occupied circular pavilion carries its
  // own continuous floors all the way down to the quay.
  const std::vector<Vec2> terrace{{172,136},{178,126},{212,119},{251,127},{274,146},
                                {274,169},{260,179},{198,179},{186,172},{177,155}};
  // Annular strips pair corresponding angular samples. Unequal ring counts
  // would twist floor plates across the open stairwell.
  const auto circular=plan_circle(radius,144,centre),stairwell=plan_circle(4.7f,144,centre);
  for(const auto& part:subtract_convex(terrace,circular,.0001f))slab(sc.opaque,part,y,.8f,exterior);
  for(Vec2 p:std::array<Vec2,10>{{{179,139},{186,130},{202,125},{258,139},{265,150},
                                {267,166},{252,174},{211,175},{193,168},{183,152}}}) {
    Emit(&sc.opaque,ceramic).frustum(P3(p,kDeck),P3(p,y-.8f),.85f,.56f,12);
    Vec2 inward=normalize(centre-p);
    ceramic_member(sc,P3(p,y-7),P3(p+inward*5,y-.6f),{0,1,0},1.1f,ceramic);
  }
  std::vector<float> levels;for(int i=0;i<=8;++i)levels.push_back(kDeck+i*4.1f);
  for(int i=1;i<=3;++i)levels.push_back(y+i*7.2f);
  constexpr float entrance=137.5f*kPi/180;
  auto point=[&](float angle,float r,float height){return P3(centre+Vec2{std::cos(angle),std::sin(angle)}*r,height);};
  // The internal helical stair has an open well in every slab. Its full turn
  // per storey returns each landing to the same clear radial entrance aisle.
  Emit(&sc.opaque,ceramic).frustum(P3(centre,kDeck),P3(centre,55.6f),1.5f,1.5f,36);
  for(std::size_t floor=0;floor<levels.size();++floor) {
    float base=levels[floor];
    Emit plate(&sc.opaque,M_MARBLE_WHITE),edge(&sc.opaque,M_BRONZE);
    if(floor==0)slab(sc.opaque,circular,base,.4f,M_MARBLE_WHITE);
    else {
      plate.ring_cap(circular,stairwell,base);plate.ring_cap(circular,stairwell,base-.42f,false);
      edge.wall(circular,base-.42f,base,true);edge.wall(stairwell,base-.42f,base,true,false);
      for(int bay=0;bay<72;++bay) {
        float a=bay*2*kPi/72,b=(bay+1)*2*kPi/72;
        float delta=std::abs(std::atan2(std::sin((a+b)*.5f-entrance),std::cos((a+b)*.5f-entrance)));
        if(delta>.24f)rail(sc,point(a,4.78f,base),point(b,4.78f,base),true);
      }
    }
    if(floor+1==levels.size())break;
    const float next=levels[floor+1],height=next-base;
    const bool occupied=base>=y-.01f;
    for(int bay=0;bay<144;++bay) {
      float a=bay*2*kPi/144,b=(bay+1)*2*kPi/144,mid=(a+b)*.5f;
      float delta=std::abs(std::atan2(std::sin(mid-entrance),std::cos(mid-entrance)));
      bool door=(floor==0||floor==8)&&delta<radians(4.5f);
      Vec3 p=point(a,radius,base+.10f),q=point(b,radius,base+.10f);
      Emit glass(&sc.opaque,occupied?pane:M_GLASS_BLUE);
      if(!door)glass.quad_metric(q,p,p+Vec3{0,height-.70f,0},q+Vec3{0,height-.70f,0});
      else glass.quad_metric(point(b,radius,base+3.4f),point(a,radius,base+3.4f),
                            point(a,radius,next-.6f),point(b,radius,next-.6f));
      if(bay%3==0&&!(door&&delta<radians(3.2f)))
        Emit(&sc.opaque,M_BRONZE).beam(point(a,radius+.055f,base),point(a,radius+.055f,next-.42f),.12f,.13f);
    }
    // Recessed ceiling panels and deep perimeter frames belong to each
    // complete occupied storey, including the rear of the pavilion.
    const auto ceiling=plan_circle(radius-.12f,144,centre);
    Emit(&sc.opaque,M_PANEL_WARM).ring_cap(ceiling,stairwell,next-.60f,false);
    for(int column=0;column<12;++column) {
      float a=column*2*kPi/12;
      Emit(&sc.opaque,M_BRONZE).frustum(point(a,23.45f,base),point(a,23.45f,next-.42f),.15f,.15f,12);
    }
    const int steps=occupied?44:25;
    for(int step=0;step<steps;++step) {
      float a=entrance+step*2*kPi/steps,b=entrance+(step+1)*2*kPi/steps;
      float top=base+(step+1)*height/steps;
      const std::vector<Vec2> tread{{point(a,1.65f,0).x,point(a,1.65f,0).z},
        {point(a,4.45f,0).x,point(a,4.45f,0).z},{point(b,4.45f,0).x,point(b,4.45f,0).z},
        {point(b,1.65f,0).x,point(b,1.65f,0).z}};
      slab(sc.opaque,tread,top,.14f,M_MARBLE_WHITE);
      const float entry_delta=std::abs(std::atan2(std::sin((a+b)*.5f-entrance),std::cos((a+b)*.5f-entrance)));
      if(entry_delta>.25f)rail(sc,point(a,4.50f,top),point(b,4.50f,top+height/steps));
    }
    std::vector<Vec2> landing;
    for(float r:{1.65f,5.2f})for(float sign:{-1.f,1.f}) {
      Vec3 p=point(entrance+sign*.13f,r,next);landing.push_back({p.x,p.z});
    }
    std::swap(landing[2],landing[3]);slab(sc.opaque,landing,next,.16f,M_MARBLE_WHITE);
  }
  // Closed formed shells reuse the original facade bearings, shade columns,
  // envelopes and practical lights. Curved undersides have actual returns.
  stage_landing_canopies(sc);
  // Existing planted edges and the stair arrival remain at their established
  // level. The new wet stone is confined to this exposed exterior terrace.
  rail(sc,{188,y,174},{kLandingEntryX-2.5f,y,174},true);
  rail(sc,{kLandingEntryX+2.5f,y,174},{270,y,174},true);
  for(int i=0;i<6;++i) {
    Vec2 bed{197+i*13.f,170};if(length(bed-centre)<28)continue;
    garden(sc,rng.child(i),bed,3.4f,2,y,i==0||i==5,true);
    if(length(bed+Vec2{0,-5}-centre)>26)gen_bench(sc,P3(bed+Vec2{0,-5},y),0);
  }
  garden(sc,rng.child(12),{187.5f,156},2.4f,12,y,true,true);
  bridge(sc,{190,y-.35f,153},{160,22-.35f,153},4,0);
  slab(sc.opaque,plan_rounded_rect(17,21,4,8,{148,153}),22,.7f,M_TERRAZZO);
  box(sc,M_GLASS_STD,{148,11,153},{16,11,20});
  sc.register_range(first,std::uint32_t(sc.opaque.indices.size()),{222,28,149},95);
}
void allocate_authored_roofs(Scene& sc) {
  for(const auto& roof:sc.authored_roofs) {
    if(roof.polygon.size()<3||std::abs(plan_area(roof.polygon))<30)continue;
    std::vector<std::vector<Vec2>> exclusions;
    for(const auto& obstruction:sc.roof_obstructions) {
      if(obstruction.top<=roof.y+.06f||obstruction.bottom>=roof.y+14)continue;
      if(!polygons_overlap(roof.polygon,obstruction.polygon))continue;
      auto part=obstruction.polygon;
      float winding=plan_area(roof.polygon)>0?1.f:-1.f;
      for(std::size_t i=0;i<roof.polygon.size()&&part.size()>=3;++i) {
        Vec2 p=roof.polygon[i],d=roof.polygon[(i+1)%roof.polygon.size()]-p;
        part=clip_halfplane(part,p,Vec2{-d.y,d.x}*winding);
      }
      if(part.size()<3||std::abs(plan_area(part))<.1f)continue;
      bool covered=false;
      for(const auto& previous:exclusions) {
        bool inside=true;for(Vec2 p:part)if(!point_in_polygon(previous,p)){inside=false;break;}
        if(inside){covered=true;break;}
      }
      if(covered)continue;
      exclusions.erase(std::remove_if(exclusions.begin(),exclusions.end(),[&](const auto& previous){
        for(Vec2 p:previous)if(!point_in_polygon(part,p))return false;
        return true;
      }),exclusions.end());
      exclusions.push_back(std::move(part));
    }
    std::vector<std::vector<Vec2>> clear{roof.polygon};
    for(const auto& obstacle:exclusions) {
      std::vector<std::vector<Vec2>> remaining;
      for(const auto& surface:clear)for(auto part:subtract_convex(surface,obstacle,.1f))remaining.push_back(std::move(part));
      clear=std::move(remaining);if(clear.empty())break;
    }
    float area=0;for(const auto& part:clear)area+=std::abs(plan_area(part));
    if(area<36)continue;
    auto beds=roof_garden(sc,roof.rng.child(1),roof.polygon,plan_centroid(roof.polygon),roof.y,roof.style,exclusions);
    exclusions.insert(exclusions.end(),beds.begin(),beds.end());
    if(!sc.asset_library.resources.empty())
      stage_occupied_roof(sc,roof.polygon,roof.y,exclusions,static_cast<RoofUse>(roof.style%5),roof.rng.child(2));
    else for(const auto& part:clear) {
      Rng equipment=roof.rng.child(3);roof_equipment(sc.opaque,equipment,part,roof.y,2);
    }
  }
}
std::vector<Vec2> relocated_market_approach() {
  return {street(kNearestBridgeRow,-355-kGridLateralOrigin),street(54,-355-kGridLateralOrigin),{-304.5f,136},{-304.5f,85.6f},{-316.8f,85.6f}};
}
void public_access(Scene& sc,Rng rng) {
  // The square, pedestrian street and terrace stairs form a continuous
  // public route. Landing levels are supported by columns down to the quay.
  auto walk=[&](const std::vector<Vec2>& points) {
    for(std::size_t i=0;i+1<points.size();++i) {
      Vec2 a=points[i],b=points[i+1],d=normalize(b-a);
      const bool roadside=std::abs(riverfront::coordinates(a).x-176.5f)<.1f&&std::abs(riverfront::coordinates(b).x-176.5f)<.1f;
      const float half_width=roadside?1.5f:3.5f;Vec2 n{-d.y*half_width,d.x*half_width};
      slab(sc.opaque,{a+n,b+n,b-n,a-n},kDeck,.12f,M_PLAZA);
    }
    for(Vec2 p:points) {
      const float radius=std::abs(riverfront::coordinates(p).x-176.5f)<.1f?1.5f:3.5f;
      slab(sc.opaque,plan_circle(radius,24,p),kDeck,.12f,M_PLAZA);
    }
  };
  walk({{-68,94},{-15,109},{-4,106.5f},{34,106.5f},{44,119},{44,194},{128,213}});
  walk(std::vector<Vec2>(kGardenApproach.begin(),kGardenApproach.end()));
  walk(std::vector<Vec2>(kWestPublicRoute.begin(),kWestPublicRoute.end()));
  // A founded footway joins the bridge block edge to a real open market bay.
  const auto market_walk=relocated_market_approach();
  for(std::size_t i=0;i+1<market_walk.size();++i) {
    Vec2 a=market_walk[i],b=market_walk[i+1],n=normalize(Vec2{-(b-a).y,(b-a).x})*1.8f;
    slab(sc.opaque,{a-n,b-n,b+n,a+n},kDeck,.6f,M_SIDEWALK);
  }
  for(Vec2 p:market_walk)slab(sc.opaque,plan_circle(1.8f,24,p),kDeck,.6f,M_SIDEWALK);
  const auto promenade_plans = arrival_promenade_plans();
  for(std::size_t segment=0;segment+1<kGardenApproach.size();++segment) {
    Vec2 a=kGardenApproach[segment],b=kGardenApproach[segment+1],d=normalize(b-a),side{-d.y,d.x};
    int count=int(length(b-a)/19);
    for(int i=0;i<count;++i)for(int sign:{-1,1}) {
      Vec2 p=a+(b-a)*((i+.5f)/count)+side*(sign*9.f);
      bool on_walk=false;
      for(std::size_t j=0;j+1<kWestPublicRoute.size();++j) {
        Vec2 wa=kWestPublicRoute[j],wb=kWestPublicRoute[j+1],wd=wb-wa;
        float wt=std::clamp(dot(p-wa,wd)/dot(wd,wd),0.f,1.f);
        if(length(p-(wa+wd*wt))<9.f){on_walk=true;break;}
      }
      if(on_walk)continue;
      for(const auto& reserved:transit_reserved_plans())
        if(polygons_overlap(plan_rect(5,6,p),reserved)){on_walk=true;break;}
      if(on_walk)continue;
      // The earlier approach gardens must not occupy the continuous quay.
      // Its own bank module now supplies supported planting and street furniture.
      for(const auto& promenade : promenade_plans)
        if(polygons_overlap(plan_rect(5,6,p),promenade)){on_walk=true;break;}
      if(on_walk)continue;
      Rng r=rng.child(10+segment*200+i*2+(sign>0?1:0));
      garden(sc,r,p,2.1f,3.5f,kDeck,i%3==0,true);
      if(i%4==0)lamp(sc,P3(p-side*(sign*4.f),kDeck));
    }
  }
  constexpr int steps=196;
  constexpr float top=34,rise=(top-kDeck)/steps,run=.30f;
  for(int i=0;i<steps;++i) {
    float y=kDeck+(i+1)*rise;
    box(sc,M_CONCRETE_WHITE,{128+(i+.5f)*run,y-rise*.5f,213},{run*.51f,rise*.5f,2.2f});
  }
  rail(sc,{128,kDeck,215.1f},{128+steps*run,top,215.1f});
  rail(sc,{128,kDeck,210.9f},{128+steps*run,top,210.9f});
  bridge(sc,{186.8f,top-.3f,213},{kLandingEntryX,top-.3f,213},4.4f,0,0,2.4f);
  bridge(sc,{kLandingEntryX,top-.3f,213},{kLandingEntryX,top-.3f,174},4.4f,1,2.4f,1.4f);
  for(float z:{185.f,199.f,213.f})box(sc,M_CONCRETE_WHITE,{kLandingEntryX,17,z},{.65f,17,.65f});
  // The former east high bridge and external access tower are retired.
  // The west loggia and upper companion retain their local supported routes.

}
void transit_structure(Scene& sc) {
  const Mat ceramic=lattice_ceramic(sc);
  // A closed, supported terminus follows one reserved corridor in the actual
  // city. The concourse helper adds the two platforms and permanent trackwork.
  slab(sc.opaque,transit_plan(0,kTransitLength,-10,10),kTransitDeckY,.90f,ceramic);
  for(float side:{-6.7f,6.7f})
    Emit(&sc.opaque,M_BRONZE).beam(transit_point(0,side,15.75f),
      transit_point(kTransitLength,side,15.75f),.48f,.60f);
  for(float u=4;u<kTransitLength;u+=20) {
    for(float side:{-6.7f,6.7f}) {
      Emit(&sc.opaque,ceramic).frustum(transit_point(u,side,kDeck),
        transit_point(u,side,15.5f),.69f,.48f,16);
      slab(sc.opaque,transit_plan(u-1.1f,u+1.1f,side-1.1f,side+1.1f),kDeck+.20f,.20f,M_BRONZE);
      for(float sign:{-1.f,1.f})
        ceramic_member(sc,transit_point(u,side,11.4f),
          transit_point(u+sign*3.0f,side,16.3f),{0,1,0},.70f,ceramic);
    }
    Emit(&sc.opaque,ceramic).beam(transit_point(u,-9.4f,16.15f),
      transit_point(u,9.4f,16.15f),.62f,.40f);
  }
  // The ground court opens directly off the existing pedestrian path. Both
  // switchback flights have real treads, stringers and full turning landings.
  slab(sc.opaque,transit_access_plan(-7,17,9.5f,21),kDeck,.16f,M_PLAZA);
  const Vec3 entry=transit_access_point(-4,19,kDeck);
  const std::array<Vec2,3> approach{{riverfront::point(176.5f,130),
      riverfront::point(176.5f,220),{entry.x,entry.z}}};
  for(std::size_t i=0;i+1<approach.size();++i) {
    const Vec2 a=approach[i],b=approach[i+1],d=normalize(b-a),n{-d.y*1.5f,d.x*1.5f};
    slab(sc.opaque,{a+n,b+n,b-n,a-n},kDeck,.16f,M_PLAZA);
    sc.roof_obstructions.push_back({{a+n,b+n,b-n,a-n},kDeck,kDeck+2.5f});
  }
  for(Vec2 p:approach)slab(sc.opaque,plan_circle(1.5f,24,p),kDeck,.12f,M_PLAZA);
  for(int storey=0;storey<=4;++storey) {
    const float y=kDeck+storey*4.2f;
    slab(sc.opaque,transit_access_plan(10.9f,13.5f,9.5f,17.5f),y,.22f,M_MARBLE_WHITE);
    if(storey==4)break;
    slab(sc.opaque,transit_access_plan(4.5f,7.1f,9.5f,17.5f),y+2.1f,.22f,M_MARBLE_WHITE);
    for(int step=0;step<13;++step) {
      const float run=4.f/13,rise=2.1f/13;
      float u0=11-(step+1)*run,u1=11-step*run;
      slab(sc.opaque,transit_access_plan(u0-.006f,u1+.006f,10.4f,13.2f),y+(step+1)*rise,rise,M_MARBLE_WHITE);
      u0=7+step*run;u1=7+(step+1)*run;
      slab(sc.opaque,transit_access_plan(u0-.006f,u1+.006f,13.8f,16.6f),y+2.1f+(step+1)*rise,rise,M_MARBLE_WHITE);
    }
    for(float v:{10.65f,12.95f})
      Emit(&sc.opaque,M_BRONZE).beam(transit_access_point(11,v,y-.24f),transit_access_point(7,v,y+1.86f),.075f,.16f);
    for(float v:{14.05f,16.35f})
      Emit(&sc.opaque,M_BRONZE).beam(transit_access_point(7,v,y+1.86f),transit_access_point(11,v,y+3.96f),.075f,.16f);
    for(float v:{10.3f,13.3f})rail(sc,transit_access_point(11,v,y),transit_access_point(7,v,y+2.1f));
    for(float v:{13.7f,16.7f})rail(sc,transit_access_point(7,v,y+2.1f),transit_access_point(11,v,y+4.2f));
    rail(sc,transit_access_point(4.6f,9.6f,y+2.1f),transit_access_point(4.6f,17.4f,y+2.1f));
    for(float v:{9.6f,17.4f})rail(sc,transit_access_point(4.6f,v,y+2.1f),transit_access_point(7.0f,v,y+2.1f));
    // Outer rear guard has a ground-level public entry and an upper opening
    // only where the supported platform connector is constructed below.
    if(storey>0)rail(sc,transit_access_point(13.4f,9.6f,y),transit_access_point(13.4f,17.4f,y));
    rail(sc,transit_access_point(11,17.4f,y),transit_access_point(13.4f,17.4f,y));
    rail(sc,transit_access_point(11,9.6f,y),transit_access_point(13.4f,9.6f,y));
  }
  for(float u:{4.6f,13.4f})for(float v:{9.6f,17.4f})
    Emit(&sc.opaque,ceramic).beam(transit_access_point(u,v,kDeck),transit_access_point(u,v,kTransitWalkY),.18f,.18f);
  slab(sc.opaque,transit_access_plan(10.9f,13.5f,6.8f,13.5f),kTransitWalkY,.35f,M_MARBLE_WHITE);
  for(float u:{11.f,13.4f}) {
    rail(sc,transit_access_point(u,6.9f,kTransitWalkY),transit_access_point(u,13.4f,kTransitWalkY));
    Emit(&sc.opaque,M_BRONZE).beam(transit_access_point(u,6.8f,kTransitWalkY-.5f),transit_access_point(u,13.5f,kTransitWalkY-.5f),.12f,.17f);
  }
  rail(sc,transit_access_point(13.4f,13.4f,kTransitWalkY),transit_access_point(13.4f,17.4f,kTransitWalkY));
  rail(sc,transit_access_point(11,17.4f,kTransitWalkY),transit_access_point(13.4f,17.4f,kTransitWalkY));
  sc.roof_obstructions.push_back({transit_plan(0,kTransitLength,-10.1f,10.1f),kDeck,kTransitRoofCrownY+.05f});
  sc.roof_obstructions.push_back({transit_access_plan(-7,17,9.5f,21),kDeck,kTransitWalkY+1.15f});
}
} // namespace

Scene generate_scene(const SceneParams& params) {
  struct ModeRestore {bool old=arrival_blockout;~ModeRestore(){arrival_blockout=old;}} restore;
  arrival_blockout=params.asset=="arrival-blockout";
  Scene sc;sc.reviewed_material_maps=params.reviewed_material_maps;palette(sc);
  Rng root=root_rng(params.seed);set_far_patterns(params.far_patterns);
  if(!params.asset_kit.empty()&&!arrival_blockout) {
    sc.asset_library=load_asset_library(params.asset_kit,std::uint32_t(sc.materials.size()));
    sc.materials.insert(sc.materials.end(),sc.asset_library.materials.begin(),sc.asset_library.materials.end());
  }
  if(!params.asset.empty()&&!arrival_blockout) {
    std::string error;
    if(!generate_asset(sc,params.asset,root.child(300),params.detail,&error)) {std::fprintf(stderr,"asset: %s\n",error.c_str());std::exit(2);}
    return sc;
  }
  terrain(sc,root.child(1000));districts(sc,root.child(1000));build_arrival_bridge_abutments(sc);
  southern_city(sc,root.child(2000));civic(sc,root.child(500));
  occupied_infill(sc,root.child(530),root.child(1000));
  oval_landmark(sc,root.child(550));
  {const SceneTail hex_start(sc);canal_hex_landmark(sc,root.child(560));hex_start.move(sc,kHexMove);}
  garden_companion(sc,root.child(565));civic_landscape(sc,root.child(570),root.child(1000));
  foreground(sc,root.child(600));street_detail(sc,root.child(650));landing(sc,root.child(700));public_access(sc,root.child(750));
  stage_cinematic_gardens(sc,root.child(780));
  {const SceneTail market_start(sc);
    west_market(sc,root.child(630));stage_cinematic_market(sc,root.child(790));
    stage_street_forecourt(sc,root.child(792));market_start.move(sc,kMarketMove);
  }
  transit_structure(sc);stage_transit_concourse(sc,root.child(794));
  stage_landing_lounge(sc,root.child(795));
  if(!arrival_blockout)allocate_authored_roofs(sc);
  sc.finalize_draws();sc.city_size="coastal human-tech metropolis";sc.city_radius=10000;
  shot_camera(params.shot,sc.camera_position,sc.camera_target);
  return sc;
}
bool shot_camera(const std::string& shot,Vec3& position,Vec3& target) {
  if(shot=="civic") {position={-187.3f,3,-99};target={102.5f,67.3f,-56};}
  else if(shot=="street") {position={-119,3,149};target={-128,38,-90};}
  else if(shot=="garden") {position=kWestGardenCamera;target=kWestGardenTarget;}
  else if(shot=="terrace"||shot=="landing") {position={212.5f,36,173};target={-8.8114f,50.7328f,-50.6721f};}
  else if(shot=="aerial") {position={-292.481f,228.538f,591.547f};target={119.189f,-6.035f,-289.079f};}
  else if(shot=="galaxy") {position={-235.2f,420.2f,-860.1f};target={198.1f,361.8f,39.3f};}
  else return false;
  return true;
}
float shot_fov_degrees(const std::string& shot) {
  return shot=="garden"?53.f:shot=="civic"?62.2f:shot=="street"?65.f:shot=="galaxy"?62.f:
      shot=="landing"||shot=="terrace"?48.f:46.f;
}
std::vector<SceneRoute> scene_routes() {
  std::vector<SceneRoute> routes;
  // Continuous ground-level walks also pass beneath the crossing decks.
  // Their centres stay in the protected waterside band of the actual quay.
  for (float sign : {-1.f, 1.f}) {
    SceneRoute route{sign < 0 ? "arrival_west_promenade" : "arrival_east_promenade", false, {}};
    const Vec2 land = riverfront::across * sign;
    for (int i = 0; i <= 47; ++i) {
      const float z = 500.f - i * 20.f;
      const Vec2 bank{canal_centre(z) + sign * canal_halfwidth(z), z};
      const Vec2 walk = bank + Vec2{sign * 5.f, 0} - land * 2.1f;
      const Vec3 position = P3(walk, kDeck + 1.8f);
      route.waypoints.push_back({position, position - Vec3{riverfront::along.x, 0, riverfront::along.y} * 5.f});
    }
    routes.push_back(std::move(route));
  }
  SceneRoute civic_route{"civic_to_street",false,{}};
  for(std::size_t i=0;i<kWestPublicRoute.size();++i) {
    Vec3 p=P3(kWestPublicRoute[i],3),t=i+1<kWestPublicRoute.size()?P3(kWestPublicRoute[i+1],3):Vec3{128,3,213};
    if(i==0)t={102.5f,67.3f,-56};
    if(i==6)t={-128,38,-90};
    civic_route.waypoints.push_back({p,t});
  }
  // The former market is now across the canal. Its former composition path
  // remains a public civic walk; a separate bridge route reaches the moved shop.
  civic_route.id="civic_public_walk";
  routes.push_back(std::move(civic_route));
  SceneRoute market_route{"arrival_bridge_to_market",false,{}};
  std::vector<Vec2> bridge_line;for(float offset:kOffsets)bridge_line.push_back(street(kNearestBridgeRow,offset));
  std::vector<Vec2> crossings;
  for(std::size_t i=0;i+1<bridge_line.size();++i) {
    Vec2 a=bridge_line[i],b=bridge_line[i+1];float da=a.x-canal_centre(a.y),db=b.x-canal_centre(b.y);
    if(da*db<0)crossings.push_back(a+(b-a)*(std::abs(da)/(std::abs(da)+std::abs(db))));
  }
  const float market_low=riverfront::coordinates(street(kNearestBridgeRow,-355-kGridLateralOrigin)).x;
  const float market_high=riverfront::coordinates(street(kNearestBridgeRow,-150-kGridLateralOrigin)).x;
  std::vector<Vec3> profile;
  for(const auto& segment:road_plan_segments(bridge_line)) {
    Vec2 a=segment.first,b=segment.second;int pieces=std::max(1,int(length(b-a)/14));
    for(int j=0;j<=pieces;++j) {
      Vec2 p=a+(b-a)*(float(j)/pieces);float lateral=riverfront::coordinates(p).x;
      if(lateral<market_low-20||lateral>market_high+20)continue;
      profile.push_back(P3(p,road_surface_height(p,crossings)+1.8f));
    }
  }
  std::sort(profile.begin(),profile.end(),[](Vec3 a,Vec3 b){return a.x>b.x;});
  for(std::size_t i=0;i+1<profile.size();++i) {
    Vec3 a=profile[i],b=profile[i+1];
    const float sa=riverfront::coordinates({a.x,a.z}).x,sb=riverfront::coordinates({b.x,b.z}).x;
    if(sa<market_low||sb>market_high)continue;
    const Vec3 original_a=a,original_b=b;
    if(sa>market_high)a=lerp(original_a,original_b,(sa-market_high)/(sa-sb));
    if(sb<market_low)b=lerp(original_a,original_b,(sa-market_low)/(sa-sb));
    if(market_route.waypoints.empty()||length(a-market_route.waypoints.back().position)>.001f)
      market_route.waypoints.push_back({a,b});
    market_route.waypoints.push_back({b,b-Vec3{riverfront::across.x,0,riverfront::across.y}*5});
  }
  const auto market_approach=relocated_market_approach();
  for(std::size_t i=1;i<market_approach.size();++i) {
    Vec3 p=P3(market_approach[i],3),t=i+1<market_approach.size()?P3(market_approach[i+1],3):Vec3{-320,3,85.6f};
    market_route.waypoints.push_back({p,t});
  }
  routes.push_back(std::move(market_route));
  SceneRoute hex_route{"canal_hex_podium_access",true,{}};
  auto hex_waypoint=[&](float lateral,float row,float floor) {
    const Vec2 at=riverfront::point(lateral,row);
    const Vec3 p=P3(at,floor+1.8f),look=p-Vec3{riverfront::across.x,0,riverfront::across.y}*4;
    hex_route.waypoints.push_back({p,look});
  };
  hex_waypoint(176.5f,108,kDeck);hex_waypoint(171.16f,108,kDeck);
  for(int step=0;step<riverfront::stair_steps;++step)
    hex_waypoint(riverfront::stair_bottom-(step+.5f)*riverfront::stair_run,108,kDeck+(step+1)/6.f);
  for(float lateral:{147.8f,143.f,120.f})hex_waypoint(lateral,108,13.2f);
  const Vec3 hex_door=P3(kCanalHex+kHexMove+Vec2{19.117f,0},15);
  hex_route.waypoints.push_back({hex_door,P3(kCanalHex+kHexMove,15)});
  routes.push_back(std::move(hex_route));
  SceneRoute landing_route{"landing_access",true,{
    {{44,3,194},{128,3,213}},{{128,3,213},{160,20,213}}}};
  constexpr int landing_steps=196;
  constexpr float landing_rise=(34-kDeck)/landing_steps;
  for(int i=0;i<landing_steps;++i) {
    Vec3 p{128+(i+.5f)*.30f,kDeck+(i+1)*landing_rise+1.8f,213};
    landing_route.waypoints.push_back({p,p+Vec3{4,4*landing_rise/.30f,0}});
  }
  for(Vec3 p:std::array<Vec3,5>{{{186.8f,35.8f,213},{kLandingEntryX,35.8f,213},
      {kLandingEntryX,35.8f,174},{kLandingEntryX,35.824f,173},{212.5f,36,173}}})
    landing_route.waypoints.push_back({p,{-8.8114f,50.7328f,-50.6721f}});
  routes.push_back(std::move(landing_route));
  // The occupied pavilion's interior stair is a real second access route.
  // Its terrace-floor branch joins the same exterior landing camera path.
  SceneRoute internal_route{"landing_internal_stair",true,{}};
  constexpr float entry_angle=137.5f*kPi/180;
  auto interior_point=[](float angle,float radius,float floor) {
    return Vec3{224+std::cos(angle)*radius,floor+1.8f,144+std::sin(angle)*radius};
  };
  auto inside_waypoint=[&](float angle,float radius,float floor) {
    Vec3 p=interior_point(angle,radius,floor);
    internal_route.waypoints.push_back({p,interior_point(angle+.18f,radius,floor+.15f)});
  };
  inside_waypoint(entry_angle,22,kDeck);inside_waypoint(entry_angle,5.1f,kDeck);
  inside_waypoint(entry_angle,3.15f,kDeck);
  std::vector<float> landing_levels;
  for(int i=0;i<=8;++i)landing_levels.push_back(kDeck+i*4.1f);
  for(int i=1;i<=3;++i)landing_levels.push_back(34+i*7.2f);
  for(std::size_t floor=0;floor+1<landing_levels.size();++floor) {
    const float y=landing_levels[floor],next=landing_levels[floor+1];
    const int steps=y>=33.99f?44:25;
    for(int step=0;step<steps;++step)
      inside_waypoint(entry_angle+(step+.5f)*2*kPi/steps,3.15f,y+(step+1)*(next-y)/steps);
    inside_waypoint(entry_angle,3.15f,next);
    if(std::abs(next-34)<.01f) {
      for(float radius:{5.1f,23.f,26.f})inside_waypoint(entry_angle,radius,next);
      // The terrace outlook lies on the clear strip between the established
      // rear planter and guard. Reach it through the real entry aisle, then
      // retrace that aisle before continuing up the internal stair.
      for(Vec3 p:std::array<Vec3,5>{{{kLandingEntryX,35.8f,166},{kLandingEntryX,35.8f,173},
          {212.5f,36,173},{kLandingEntryX,35.8f,173},{kLandingEntryX,35.8f,166}}})
        internal_route.waypoints.push_back({p,{-8.8114f,50.7328f,-50.6721f}});
      for(float radius:{26.f,23.f,5.1f,3.15f})inside_waypoint(entry_angle,radius,next);
    }
  }
  inside_waypoint(entry_angle,5.1f,55.6f);inside_waypoint(entry_angle,10,55.6f);
  routes.push_back(std::move(internal_route));
  // Explicit entry audit: the public walk continues onto the dome's retained
  // stairs, supported first-floor terrace and actual opening in the drum.
  SceneRoute dome_route{"dome_entry_and_terrace",true,{}};
  auto dome_waypoint=[&](float x,float floor,float z) {
    dome_route.waypoints.push_back({{x,floor+1.8f,z},{-36,floor+1.8f,z-4}});
  };
  for(Vec2 p:std::array<Vec2,5>{{{-68,94},{-82,75},{-82,55},{-36,55},{-36,40}}})
    dome_waypoint(p.x,kDeck,p.y);
  for(int step=7;step>=0;--step)dome_waypoint(-36,2.4f-step*.15f,24.5f+(step+.5f)*.42f);
  for(int step=19;step>=0;--step)dome_waypoint(-36,5.6f-step*.16f,16.31f+step*.42f);
  for(float z:{13.f,8.f,2.f,-5.f,-9.f})dome_waypoint(-36,5.6f,z);
  for(float z:{-5.f,2.f,8.f,13.f})dome_waypoint(-36,5.6f,z);
  // The joined base carries a genuinely accessible upper garden. These points
  // use its existing 5.6m floor then the new 32-riser terrace flight.
  for(Vec2 p:std::array<Vec2,4>{{{-20,13},{12,13},{40,13},{52,13}}})dome_waypoint(p.x,5.6f,p.y);
  for(int step=0;step<32;++step)dome_waypoint(52,5.6f+(step+1)*(5.2f/32),14+(step+.5f)*.375f);
  for(Vec2 p:std::array<Vec2,3>{{{52,27},{52,30},{52,35}}})dome_waypoint(p.x,10.8f,p.y);
  for(std::size_t i=0;i+1<dome_route.waypoints.size();++i) {
    auto& current=dome_route.waypoints[i];
    Vec3 direction=dome_route.waypoints[i+1].position-current.position;
    if(length(direction)>.001f)current.target=current.position+normalize(direction)*4.f;
  }
  for(auto& point:dome_route.waypoints) {
    point.position=point.position+Vec3{kDomeMove.x,0,kDomeMove.y};
    point.target=point.target+Vec3{kDomeMove.x,0,kDomeMove.y};
  }
  routes.push_back(std::move(dome_route));
  SceneRoute garden_route{"garden_floor_loggia",false,{}};
  auto point=[&](Vec3 p,Vec3 t) {garden_route.waypoints.push_back({p,t});};
  const float eye=kGardenY+1.8f;
  for(int i=0;i<=90;++i) {
    float a=i*kPi/90;Vec3 p=P3(kLattice+Vec2{std::cos(a),std::sin(a)}*43,eye);
    Vec3 t=P3(kLattice+Vec2{std::cos(a+.03f),std::sin(a+.03f)}*43,eye);point(p,t);
  }
  for(std::size_t i=0;i<kWestGardenWalk.size();++i)
    point(P3(kWestGardenWalk[i],eye),i+1<kWestGardenWalk.size()?P3(kWestGardenWalk[i+1],eye):kWestGardenTarget);
  routes.push_back(std::move(garden_route));
  SceneRoute upper_route{"garden_upper_bridge",true,{}};
  auto upper_point=[&](Vec3 p,Vec3 t){upper_route.waypoints.push_back({p,t});};
  for(std::size_t i=kWestGardenWalk.size();i-->0;)
    upper_point(P3(kWestGardenWalk[i],eye),P3(kWestGardenWalk[i?i-1:0],eye));
  for(int i=0;i<=50;++i) {
    float a=kPi+i/50.f*(4.596f-kPi);Vec3 p=P3(kLattice+Vec2{std::cos(a),std::sin(a)}*43,eye);
    upper_point(p,P3(kLattice+Vec2{std::cos(a+.03f),std::sin(a+.03f)}*43,eye));
  }
  upper_point(P3(kUpperBridgeOriginal,eye),{kUpperStair.x,eye,kUpperStair.y+3.2f});
  for(int floor=0;floor<2;++floor) {
    float y=eye+floor*4;
    upper_point({kUpperStair.x-1.5f,y,kUpperStair.y+3.2f},{kUpperStair.x-1.5f,y+2,kUpperStair.y-3.2f});
    for(int step=0;step<12;++step) {
      Vec3 p{kUpperStair.x-1.5f,y+(step+1)/6.f,kUpperStair.y+2-(step+.5f)/3.f};upper_point(p,p+Vec3{0,2,-4});
    }
    upper_point({kUpperStair.x-1.5f,y+2,kUpperStair.y-3.2f},{kUpperStair.x+1.5f,y+2,kUpperStair.y-3.2f});
    upper_point({kUpperStair.x+1.5f,y+2,kUpperStair.y-3.2f},{kUpperStair.x+1.5f,y+4,kUpperStair.y+3.2f});
    for(int step=0;step<12;++step) {
      Vec3 p{kUpperStair.x+1.5f,y+2+(step+1)/6.f,kUpperStair.y-2+(step+.5f)/3.f};upper_point(p,p+Vec3{0,2,4});
    }
    upper_point({kUpperStair.x+1.5f,y+4,kUpperStair.y+3.2f},{kUpperStair.x-1.5f,y+4,kUpperStair.y+3.2f});
  }
  upper_point({kUpperStair.x,eye+8,kUpperStair.y+3.2f},P3(kUpperBridgeOriginal,eye+8));
  upper_point(P3(kUpperBridgeOriginal,eye+8),P3(kUpperBridgeCompanion,eye+8));
  upper_point(P3(kUpperBridgeCompanion,eye+8),{-383,eye+8,314.5f});
  upper_point({-383,eye+8,314.5f},kWestGardenCamera);
  routes.push_back(std::move(upper_route));
  SceneRoute transit_route{"transit_access",true,{}};
  auto transit_waypoint=[&](float u,float v,float y,float look_u,float look_v,float look_y) {
    transit_route.waypoints.push_back({transit_point(u,v,y+1.8f),transit_point(look_u,look_v,look_y+1.8f)});
  };
  auto access_waypoint=[&](float u,float v,float y,float look_u,float look_v,float look_y) {
    transit_route.waypoints.push_back({transit_access_point(u,v,y+1.8f),transit_access_point(look_u,look_v,look_y+1.8f)});
  };
  const Vec3 entry=transit_access_point(-4,19,kDeck);
  const Vec3 approach=P3(riverfront::point(176.5f,130),3),corner=P3(riverfront::point(176.5f,220),3);
  transit_route.waypoints.push_back({approach,corner});
  transit_route.waypoints.push_back({corner,entry+Vec3{0,1.8f,0}});
  access_waypoint(-4,19,kDeck,16,19,kDeck);
  access_waypoint(16,19,kDeck,16,13.5f,kDeck);
  access_waypoint(16,13.5f,kDeck,12.2f,13.5f,kDeck);
  access_waypoint(12.2f,13.5f,kDeck,12.2f,11.8f,kDeck);
  for(int storey=0;storey<4;++storey) {
    const float y=kDeck+storey*4.2f;
    access_waypoint(12.2f,11.8f,y,7,11.8f,y+2.1f);
    for(int step=0;step<13;++step) {
      const float u=11-(step+.5f)*(4.f/13),top=y+(step+1)*(2.1f/13);
      access_waypoint(u,11.8f,top,u-4,11.8f,top+2.1f);
    }
    access_waypoint(5.8f,11.8f,y+2.1f,5.8f,15.2f,y+2.1f);
    access_waypoint(5.8f,15.2f,y+2.1f,11,15.2f,y+4.2f);
    for(int step=0;step<13;++step) {
      const float u=7+(step+.5f)*(4.f/13),top=y+2.1f+(step+1)*(2.1f/13);
      access_waypoint(u,15.2f,top,u+4,15.2f,top+2.1f);
    }
    access_waypoint(12.2f,15.2f,y+4.2f,12.2f,11.8f,y+4.2f);
  }
  access_waypoint(12.2f,13.5f,18,12.2f,6,18);
  transit_waypoint(kTransitAccessOffset+12.2f,6,18,kTransitTurnU,6,18);
  transit_waypoint(kTransitTurnU,6,18,kTransitTurnU,-6,18);
  transit_waypoint(kTransitTurnU,-6,18,20,-6,18);
  transit_waypoint(20,-6,18,0,-6,18);
  routes.push_back(std::move(transit_route));
  return routes;
}
std::string scene_layout_manifest(const std::string& seed) {
  std::ostringstream out;out.precision(9);
  auto json_string=[](const std::string& value) {
    constexpr char hex[]="0123456789abcdef";std::string result="\"";
    for(unsigned char ch:value) {
      if(ch=='"'||ch=='\\') {result.push_back('\\');result.push_back(char(ch));}
      else if(ch<32) {result+="\\u00";result.push_back(hex[ch>>4]);result.push_back(hex[ch&15]);}
      else result.push_back(char(ch));
    }
    result.push_back('"');return result;
  };
  out << "{\"version\":1,\"seed\":"<<json_string(seed)<<",\"units\":\"metres\",\"up\":\"y\",\"water_height\":0,"
      << "\"anchors\":[{\"id\":\"civic_ring\",\"position\":[171.883,1.2,-166.568],\"radius\":51.064,\"height_scale\":1.9},"
      << "{\"id\":\"civic_dome\",\"position\":[4,2.4,-120],\"radius\":42},"
      << "{\"id\":\"foreground_lattice\",\"position\":[-345,17.2,405],\"height\":544,\"north_elevation\":\"snapped_graphite_structure_and_solar_control_sector\",\"south_elevation\":\"ivory_ceramic_lattice\",\"west_elevation\":\"ivory_lattice_and_open_loggias_outside_occupied_inner_curtain\",\"north_arc_degrees\":["
      << kGardenGraphiteBegin*180.f/kPi << "," << kGardenGraphiteEnd*180.f/kPi
      << "],\"graphite_structural_column_boundaries\":[14,21],\"structural_column_count\":22,\"transition_ribs_at_sector_boundaries\":true,\"occupied_inner_curtain_retained\":true,\"normal_incidence_north_pane_transmission\":[0.1392,0.1584,0.1776],\"north_spandrel_height\":0.8},"
      << "{\"id\":\"oval_lattice\",\"position\":[458.8,1.2,-360],\"height\":136,\"half_extents\":[60,34],\"yaw_radians\":-0.862,\"mid_shaft_bulge\":0.06,\"roof_y\":137.2},"
      << "{\"id\":\"canal_hex\",\"position\":[-151.43,13.2,233.20],\"radius\":18.117,\"roof_y\":93.2},"
      << "{\"id\":\"west_market\",\"position\":[-334,1.2,88],\"facade_x\":-312.5},"
      << "{\"id\":\"south_civic_arcade\",\"position\":[70.884,1.2,7.64],\"occupied_floor_count\":2,\"asymmetric_upper_wings\":true,\"public_colonnade_roof_y\":5.2},"
      << "{\"id\":\"secondary_curved_slab\",\"position\":[410,13.2,95],\"shaft_floors\":44,\"shaft_crown_y\":197.2},"
      << "{\"id\":\"foreground_graphite_slab\",\"parcel\":\"coastal_ribbon/4/5\",\"position\":[43.17630,13.2,342.26577],\"crown_centre\":[26.05630,206.8,340.00577],\"half_axes\":[21.83,29.47],\"yaw_radians\":0.18,\"crown_scale\":0.8,\"occupied_shaft_floors\":47,\"supporting_frontage_floors\":3,\"parapet_top_y\":207.45},"
      << "{\"id\":\"bronze_needle\",\"position\":[190,13.2,-300],\"height\":440},"
      << "{\"id\":\"lattice_garden\",\"position\":[-402.5,277.2,403],\"tower_side\":\"west\"},"
      << "{\"id\":\"garden_companion\",\"position\":[-386,1.2,300],\"radius\":18,\"roof_y\":321.2},"
      << "{\"id\":\"garden_upper_bridge\",\"start\":[-350,285.2,362.2],\"end\":[-383,285.2,317.7]},"
      << "{\"id\":\"landing_terrace\",\"position\":[224,34,144],\"occupied_radius\":24,\"floor_levels\":[34,41.2,48.4],\"entry_angle_degrees\":137.5},"
      << "{\"id\":\"permanent_transit_terminus\",\"start\":["<<kTransitStart.x<<","<<kTransitDeckY<<","<<kTransitStart.y
      << "],\"end\":["<<kTransitEnd.x<<","<<kTransitDeckY<<","<<kTransitEnd.y
      << "],\"width\":20,\"platform_y\":18,\"roof_eave_y\":"<<kTransitRoofEaveY<<",\"roof_crown_y\":"<<kTransitRoofCrownY
      << ",\"main_hall_length\":"<<kTransitHallEnd<<",\"roof_gap\":"<<kTransitRoofGap<<",\"rounded_terminal_length\":"<<kTransitTerminalLength
      << ",\"end_turn_u\":"<<kTransitTurnU<<",\"end_platform_start_u\":"<<kTransitEndPlatformStart
      << ",\"access_floor_levels\":[1.2,5.4,9.6,13.8,18],\"vehicles\":false}],\"cameras\":[";
  bool first=true;
  for(const char* name:{"aerial","galaxy","civic","street","garden","landing"}) {
    Vec3 p,t;shot_camera(name,p,t);if(!first)out<<',';first=false;
    out<<"{\"id\":\""<<name<<"\",\"position\":["<<p.x<<','<<p.y<<','<<p.z
       <<"],\"target\":["<<t.x<<','<<t.y<<','<<t.z<<"],\"vertical_fov_degrees\":"<<shot_fov_degrees(name)<<'}';
  }
  out<<"],\"parcel_address\":\"coastal_ribbon/column/row\",\"massing_families\":16,"
       "\"camera_independent_geometry\":true,\"arrival_layout\":{"
       "\"legacy_outer_cross_axis\":[0.963518,-0.267645],\"legacy_outer_avenue_axis\":[0.267645,0.963518],"
       "\"legacy_lateral_origin\":-540,\"nearest_bridge_legacy_row\":72,\"nearest_bridge_width\":20,"
       "\"riverfront_survey\":{\"origin\":["<<riverfront::origin.x<<","<<riverfront::origin.y
       <<"],\"cross_axis\":["<<riverfront::across.x<<","<<riverfront::across.y<<"],\"avenue_axis\":["<<riverfront::along.x<<","<<riverfront::along.y
       <<"],\"main_boulevard_lateral\":"<<riverfront::avenue_lateral<<",\"main_boulevard_width\":"<<riverfront::avenue_width
       <<",\"tower_lot_bounds\":[35,146,18,190],\"retired_internal_cross_street\":true},"
              "\"canal_visible_x_intercept\":-259.6,\"canal_dx_dz\":0.155,\"canal_clear_width\":34,"
       "\"canal_straight_reach_z\":[-550,650],\"retired_routes\":[\"east_ground_to_garden_switchback\",\"east_high_bridge\"],\"garden_ground_access\":\"not supplied in Arrival benchmark\"},\"plots\":[";
  first=true;
  for(const auto& plot:coastal_plots(root_rng(seed).child(1000))) {
    Vec2 c=plan_centroid(plot.footprint);
    bool occupied=!reserved_plot(plot.footprint)&&c.x>=shore(c.y)+24&&c.x<=east_shore(c.y)-24;
    if(!first)out<<',';
    first=false;
    out<<"{\"id\":\"coastal_ribbon/"<<plot.column<<'/'<<plot.row<<"\",\"family\":"<<plot.address%16
       <<",\"authored_shaft_floor_override\":"<<rear_middle_floors(plot.address)
       <<",\"facade_override\":"<<json_string(plot.address==253?"curved_graphite_foreground_slab":plot.address==478||plot.address==475?"ivory_hex_lattice":plot.address==398?"ivory_diagrid":plot.address==529?"ivory_hex_members":"none")
       <<",\"occupied\":"<<(occupied?"true":"false")<<",\"centre\":["<<c.x<<','<<c.y
       <<"],\"tier\":\""<<(length(c)<900?"foreground":length(c)<1800?"middle":"far")<<"\",\"footprint\":[";
    for(std::size_t i=0;i<plot.footprint.size();++i) {
      if(i)out<<',';
      out<<'['<<plot.footprint[i].x<<','<<plot.footprint[i].y<<']';
    }
    out<<"]}";
  }
  for(const auto& plot:northern_plots(root_rng(seed).child(1000))) {
    if(!first)out<<',';
    first=false;
    out<<"{\"id\":\"northern_headland/"<<plot.row<<'/'<<plot.column<<"\",\"family\":"<<plot.address%16
       <<",\"occupied\":true,\"centre\":["<<plot.centre.x<<','<<plot.centre.y
       <<"],\"tier\":\"far\",\"floors\":"<<plot.floors<<",\"footprint\":[";
    for(std::size_t i=0;i<plot.footprint.size();++i) {
      if(i)out<<',';
      out<<'['<<plot.footprint[i].x<<','<<plot.footprint[i].y<<']';
    }
    out<<"]}";
  }
  const auto southern=southern_plots(root_rng(seed).child(2000));
  const auto nearshore=southern_neighborhood(southern);
  const auto peninsula=connected_shore_neighborhood(southern,0),mainland=connected_shore_neighborhood(southern,1);
  for(const auto& plot:southern) {
    if(!first)out<<',';
    first=false;
    const bool replaced=nearshore_selection(plot)||neighborhood_replaces(peninsula,plot)||neighborhood_replaces(mainland,plot);
    out<<"{\"id\":\"southern_city/"<<plot.district<<'/'<<plot.row<<'/'<<plot.column<<"\",\"family\":"<<plot.address%16
       <<",\"occupied\":"<<(replaced?"false":"true")<<",\"typology\":"
       <<json_string(replaced?"subdivided_neighborhood_parent":"far_occupied_building")
       <<",\"centre\":["<<plot.centre.x<<','<<plot.centre.y
       <<"],\"tier\":\"far\",\"floors\":"<<plot.floors<<",\"footprint\":[";
    for(std::size_t i=0;i<plot.footprint.size();++i) {
      if(i)out<<',';
      out<<'['<<plot.footprint[i].x<<','<<plot.footprint[i].y<<']';
    }
    out<<"]}";
  }
  for(const auto& group:std::array<std::pair<const char*,const SouthernNeighborhood*>,3>{{
        {"southern_neighborhood",&nearshore},{"peninsula_neighborhood",&peninsula},{"mainland_neighborhood",&mainland}}})
  for(const auto& block:group.second->parcels)for(std::size_t i=0;i<block.buildings.size();++i) {
    const auto& house=block.buildings[i];
    if(!first)out<<',';
    first=false;
    Vec2 centre=plan_centroid(house.footprint);
    out<<"{\"id\":\""<<group.first<<'/'<<block.row<<'/'<<block.column<<'/'<<i
       <<"\",\"parent\":\"southern_city/"<<block.district<<'/'<<block.row<<'/'<<block.column
       <<"\",\"occupied\":true,\"typology\":"<<json_string(house.program)
       <<",\"neighborhood_program\":"<<json_string(block.program)<<",\"floors\":"<<house.floors
       <<",\"ground_y\":1.2,\"foundation_bottom_y\":0.65,\"centre\":["<<centre.x<<','<<centre.y
       <<"],\"tier\":\"far\",\"footprint\":[";
    for(std::size_t j=0;j<house.footprint.size();++j){if(j)out<<',';out<<'['<<house.footprint[j].x<<','<<house.footprint[j].y<<']';}
    out<<"]}";
  }
  std::size_t civic_index=0;
  for(const auto& wing:civic_base_wings()) {
    if(!first)out<<',';
    first=false;Vec2 centre=plan_centroid(wing.footprint)+kDomeMove;
    out<<"{\"id\":\"joined_civic_base/"<<civic_index++<<"\",\"occupied\":true,\"typology\":\"joined_civic_frontage\",\"centre\":["
       <<centre.x<<','<<centre.y<<"],\"floor_levels\":[";
    for(std::size_t i=0;i<wing.levels.size();++i){if(i)out<<',';out<<wing.levels[i];}
    out<<"],\"footprint\":[";
    for(std::size_t i=0;i<wing.footprint.size();++i){if(i)out<<',';out<<'['<<wing.footprint[i].x+kDomeMove.x<<','<<wing.footprint[i].y+kDomeMove.y<<']';}
    out<<"]}";
  }
  for(const auto& plot:infill_plots(root_rng(seed).child(1000))) {
    Vec2 c=plan_centroid(plot.footprint);
    if(!first)out<<',';
    first=false;
    out<<"{\"id\":\"coastal_infill/"<<plot.column<<'/'<<plot.row<<'/'<<plot.piece
       <<"\",\"family\":"<<(plot.column+plot.row)%3<<",\"typology\":\""
       <<(((plot.column==2||plot.column==3)&&plot.row==6)?"sustained_street_frontage":"terraced_mixed_use")
       <<"\",\"occupied\":true,\"centre\":["<<c.x<<','<<c.y
       <<"],\"tier\":\"foreground\",\"footprint\":[";
    for(std::size_t i=0;i<plot.footprint.size();++i) {
      if(i)out<<',';
      out<<'['<<plot.footprint[i].x<<','<<plot.footprint[i].y<<']';
    }
    out<<"]}";
  }
  out<<"],\"nearshore_neighborhood\":{\"parent_count\":"<<nearshore.parcels.size()<<",\"streets\":[";
  bool first_street=true;
  for(const auto& street:nearshore.streets) {
    if(!first_street)out<<',';
    first_street=false;
    out<<"{\"a\":["<<street.a.x<<','<<street.a.y<<"],\"b\":["<<street.b.x<<','<<street.b.y
       <<"],\"half_width\":"<<street.half_width<<",\"new_local_street\":"<<(street.local?"true":"false")<<'}';
  }
  out<<"],\"envelopes\":[";
  bool first_envelope=true;
  for(const auto& block:nearshore.parcels) {
    if(!first_envelope)out<<',';
    first_envelope=false;out<<"{\"row\":"<<block.row<<",\"column\":"<<block.column<<",\"footprint\":[";
    for(std::size_t i=0;i<block.envelope.size();++i){if(i)out<<',';out<<'['<<block.envelope[i].x<<','<<block.envelope[i].y<<']';}
    out<<"]}";
  }
  out<<"]},\"connected_shore_neighborhoods\":[";
  for(int district=0;district<2;++district) {
    if(district)out<<',';
    const auto& neighborhood=district?mainland:peninsula;
    out<<"{\"district\":"<<district<<",\"replaced_parent_count\":"<<neighborhood.parcels.size()<<",\"streets\":[";
    for(std::size_t i=0;i<neighborhood.streets.size();++i) {
      if(i)out<<',';
      const auto& street=neighborhood.streets[i];
      out<<"{\"a\":["<<street.a.x<<','<<street.a.y<<"],\"b\":["<<street.b.x<<','<<street.b.y
         <<"],\"half_width\":"<<street.half_width<<",\"new_local_street\":"<<(street.local?"true":"false")<<'}';
    }
    out<<"],\"envelopes\":[";
    for(std::size_t i=0;i<neighborhood.parcels.size();++i) {
      if(i)out<<',';
      const auto& block=neighborhood.parcels[i];
      out<<"{\"row\":"<<block.row<<",\"column\":"<<block.column<<",\"program\":"<<json_string(block.program)<<",\"footprint\":[";
      for(std::size_t j=0;j<block.envelope.size();++j){if(j)out<<',';out<<'['<<block.envelope[j].x<<','<<block.envelope[j].y<<']';}
      out<<"]}";
    }
    out<<"]}";
  }
  out<<"],\"transit_reserved_footprints\":[";

  bool first_transit=true;
  for(const auto& polygon:transit_reserved_plans()) {
    if(!first_transit)out<<',';
    first_transit=false;out<<'[';
    for(std::size_t i=0;i<polygon.size();++i){if(i)out<<',';out<<'['<<polygon[i].x<<','<<polygon[i].y<<']';}
    out<<']';
  }
  out<<"],\"routes\":[";
  bool first_route=true;
  for(const auto& route:scene_routes()) {
    if(!first_route)out<<',';
    first_route=false;
    out<<"{\"id\":"<<json_string(route.id)
       <<",\"motion_mode\":\"walking_camera\",\"runtime_collision_controller\":false,"
         "\"eye_height\":1.8,\"nominal_eye_height\":1.8,\"contains_stairs\":"<<(route.stairs?"true":"false")
       <<",\"waypoints\":[";
    for(std::size_t i=0;i<route.waypoints.size();++i) {
      if(i)out<<',';
      const auto& p=route.waypoints[i].position;const auto& t=route.waypoints[i].target;
      out<<"{\"position\":["<<p.x<<','<<p.y<<','<<p.z<<"],\"target\":["<<t.x<<','<<t.y<<','<<t.z<<"]}";
    }
    out<<"]}";
  }
  out<<"]}";
  return out.str();
}
std::string scene_layout_manifest() {return scene_layout_manifest("83");}
} // namespace cb
