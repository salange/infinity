#include "garden_staging.hpp"
#include "garden_canopy.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
constexpr float kGardenFloor = 277.2f;
constexpr float kLandingFloor = 34.f;

bool garden_floor_contains(Vec2 p) {
  static const auto floors=garden_supported_floor_plans();
  for(const auto& floor:floors)if(point_in_polygon(floor,p))return true;
  return false;
}
bool garden_surface_supported(Vec3 centre,Vec3 half,Vec3 x={1,0,0},Vec3 z={0,0,1}) {
  // Test the full footprint perimeter, including the new concave overlook
  // notch, and reject edge-straddling cosmetic details.
  const std::array<Vec3,4> corners{{centre-x*half.x-z*half.z,centre+x*half.x-z*half.z,
                                   centre+x*half.x+z*half.z,centre-x*half.x+z*half.z}};
  for(int side=0;side<4;++side) {
    Vec3 a=corners[side],b=corners[(side+1)%4];
    const int samples=std::max(1,int(std::ceil(length(b-a)/.025f)));
    for(int i=0;i<=samples;++i) {
      Vec3 p=lerp(a,b,float(i)/samples);
      if(!garden_floor_contains({p.x,p.z}))return false;
    }
  }
  return true;
}
bool garden_instance_supported(const Scene& scene,std::string_view name,Vec3 at,float yaw,Vec3 scale) {
  const auto& mesh=scene.asset_library.resources[asset_resource(scene.asset_library,name)].mesh;
  AssetInstance instance{0,at,yaw,scale,{1,1,1}};
  Vec3 centre=asset_transform_point(instance,(mesh.bounds_min+mesh.bounds_max)*.5f);
  Vec3 half=(mesh.bounds_max-mesh.bounds_min)*.5f;
  return garden_surface_supported(centre,{half.x*scale.x,0,half.z*scale.z},
      {std::cos(yaw),0,-std::sin(yaw)},{std::sin(yaw),0,std::cos(yaw)});
}

std::uint32_t material(const Scene& sc, std::string_view name) {
  for (std::uint32_t i = 0; i < sc.materials.size(); ++i)
    if (sc.materials[i].name == name) return i;
  throw std::runtime_error("Garden staging requires authored material: " + std::string(name));
}

struct Palette {
  std::uint32_t stone, ceramic, soil, wood, bronze, bark, glass, gasket;
  explicit Palette(const Scene& sc)
      : stone(material(sc, "stone_warm")), ceramic(material(sc, "ceramic_ivory")),
        soil(material(sc, "soil_mulch")), wood(material(sc, "wood_oiled")),
        bronze(material(sc, "bronze_oxidized")), bark(material(sc, "bark_ridged")),
        glass(material(sc, "glass_clear")), gasket(material(sc, "gasket_charcoal")) {}
};

void plant(Scene& sc, Rng& rng, std::string_view name, Vec3 p, float scale) {
  const float green = rng.range(.88f, 1.08f);
  add_asset_instance(sc, name, p, rng.range(-kPi, kPi),
                     {scale * rng.range(.92f, 1.08f), scale, scale * rng.range(.93f, 1.07f)},
                     {rng.range(.93f, 1.05f), green, rng.range(.92f, 1.05f)});
}

void evergreen_mass(Scene& sc, Rng& rng, Vec3 p, float height, float width) {
  // Overlapping rooted crowns retain the source leaves' curvature and angles.
  // Compressing a tree vertically flattens nearly every blade, producing a
  // coherent sheet of grazing reflections even with correct inverse normals.
  const auto& mesh=sc.asset_library.resources[
      asset_resource(sc.asset_library,"tree_multistem")].mesh;
  const float horizontal_radius=std::hypot(
      std::max(std::fabs(mesh.bounds_min.x),std::fabs(mesh.bounds_max.x)),
      std::max(std::fabs(mesh.bounds_min.z),std::fabs(mesh.bounds_max.z)));
  const float scale=std::min(height/mesh.bounds_max.y,width*.5f/horizontal_radius);
  const float ring_radius=std::max(0.f,width*.5f-horizontal_radius*scale);
  const float rotation=rng.range(-kPi,kPi);
  for(int i=0;i<7;++i) {
    const float angle=rotation+(i-1)*(2*kPi/6);
    const Vec3 offset=i==0?Vec3{}:Vec3{std::cos(angle)*ring_radius,0,
                                      std::sin(angle)*ring_radius};
    const float crown_scale=scale*(i==0?1.f:rng.range(.91f,.99f));
    add_asset_instance(sc,"tree_multistem",p+offset,rng.range(-kPi,kPi),
                       {crown_scale,crown_scale,crown_scale},
                       {.83f,rng.range(.91f,1.0f),.84f});
  }
}

void leaning_canopy(Scene& sc, Vec3 root,float scale=.61f,float yaw=-.35f) {
  constexpr std::string_view name="canopy_broadleaf_garden_lean";
  bool present=false;
  for(const auto& resource:sc.asset_library.resources)present|=resource.name==name;
  if(!present) {
    MeshResource resource=sc.asset_library.resources[asset_resource(sc.asset_library,"canopy_broadleaf")];
    resource.name=name;resource.mesh.bounds_min={1e30f,1e30f,1e30f};
    resource.mesh.bounds_max={-1e30f,-1e30f,-1e30f};
    const float c=std::cos(.50f),s=std::sin(.50f);
    auto lean=[&](Vec3 v){return Vec3{c*v.x+s*v.y,-s*v.x+c*v.y,v.z};};
    for(auto& vertex:resource.mesh.vertices) {
      vertex.position=lean(vertex.position);vertex.normal=lean(vertex.normal);
      vertex.tangent={lean(vertex.tangent.xyz()),vertex.tangent.w};
      resource.mesh.bounds_min=vmin(resource.mesh.bounds_min,vertex.position);
      resource.mesh.bounds_max=vmax(resource.mesh.bounds_max,vertex.position);
    }
    sc.asset_library.resources.push_back(std::move(resource));
  }
  // The tree remains rooted in the supported northwest soil bed; its crown
  // leans above the seat and into the upper-left opening of the actual camera.
  add_asset_instance(sc,name,root,yaw,{scale,scale,scale},{.88f,.96f,.87f});
}

// Separate rounded coping, recessed soil and irrigation make these planted
// volumes read as maintained garden beds instead of shrubs placed on paving.
float bed(Scene& sc, const Palette& mats, Rng rng, Vec2 centre, Vec2 half,
          float floor, float height) {
  const auto outer = plan_superellipse(half.x, half.y, 3.1f, 44, centre);
  const auto inner = plan_superellipse(half.x - .20f, half.y - .20f, 3.1f, 44, centre);
  Emit stone(&sc.opaque, mats.stone), ceramic(&sc.opaque, mats.ceramic);
  stone.wall(outer, floor, floor + height - .12f, true);
  ceramic.wall(outer, floor + height - .12f, floor + height, true);
  ceramic.ring_cap(outer, inner, floor + height);
  stone.wall(inner, floor + height - .18f, floor + height, true, false);
  const float soil_y = floor + height - .13f;
  Emit(&sc.opaque, mats.soil).polygon(inner, soil_y, true);
  Emit mulch(&sc.opaque, mats.bark), irrigation(&sc.opaque, mats.gasket);
  for (int i = 0; i < int(half.x * half.y * 14.f); ++i) {
    Vec2 p{centre.x + rng.range(-half.x + .3f, half.x - .3f),
           centre.y + rng.range(-half.y + .3f, half.y - .3f)};
    if (!point_in_polygon(inner, p)) continue;
    float angle = rng.range(-kPi, kPi);
    Vec3 axis{std::cos(angle), 0, std::sin(angle)};
    mulch.element_random = rng.next();
    mulch.box({p.x, soil_y + rng.range(.012f, .027f), p.y},
              {rng.range(.035f, .105f), .012f, rng.range(.012f, .035f)},
              axis, {0, 1, 0}, {-axis.z, 0, axis.x});
  }
  const auto drip = plan_superellipse(half.x - .37f, half.y - .37f, 3.1f, 36, centre);
  for (std::size_t i = 0; i < drip.size(); ++i) {
    Vec2 a = drip[i], b = drip[(i + 1) % drip.size()];
    irrigation.tube({a.x, soil_y + .027f, a.y}, {b.x, soil_y + .027f, b.y}, .018f, 5);
  }
  return soil_y;
}

void understory(Scene& sc, Rng rng, Vec2 centre, Vec2 half, float soil_y,
                float height_scale = 1.f,bool dry_fern=false) {
  // Broad fronds, fine upright blades, flowering shrubs and a low mat overlap
  // at different heights, with small exposed mulch pockets around the stems.
  int rows = std::max(2, int(half.y * 1.4f));
  int columns = std::max(2, int(half.x * 1.3f));
  const auto planting_boundary=plan_superellipse(half.x,half.y,3.1f,44,centre);
  for (int z = 0; z < rows; ++z) for (int x = 0; x < columns; ++x) {
    Vec2 p{centre.x + (float(x) / float(columns - 1) * 2 - 1) * (half.x - .7f),
           centre.y + (float(z) / float(rows - 1) * 2 - 1) * (half.y - .65f)};
    p = p + Vec2{rng.range(-.28f, .28f), rng.range(-.25f, .25f)};
    if(dry_fern&&!point_in_polygon(planting_boundary,p))continue;
    const int species = (x + z * 3) % 6;
    std::string_view name = species < 2 ? "fern_arching" : species == 2 ? "phormium"
                                         : species == 3 ? "shrub_flowering" : "groundcover";
    if(dry_fern&&name=="fern_arching")name="fern_arching_dry";
    float scale = rng.range(.80f, 1.15f) * height_scale;
    if (species == 3) scale *= .75f;
    plant(sc, rng, name, {p.x, soil_y, p.y}, scale);
  }
}

void accent_light(Scene& sc, Vec3 p, float yaw) {
  add_asset_instance(sc, "bronze_light", p, yaw, 1.f);
  sc.lights.push_back({p + Vec3{0, .22f, 0}, 4.5f, {1.f, .72f, .42f}, 1.3f});
}

void wind_litter(Scene& sc, const Palette& mats, Rng rng, Vec3 centre,
                 Vec2 half, int count,bool on_garden_floor=false) {
  Emit leaf(&sc.opaque, mats.bark), twig(&sc.opaque, mats.wood);
  for (int i = 0; i < count; ++i) {
    Vec3 p = centre + Vec3{rng.range(-half.x, half.x), .007f, rng.range(-half.y, half.y)};
    if(on_garden_floor&&!garden_surface_supported(p,{.25f,0,.25f}))continue;
    float angle = rng.range(-kPi, kPi), length = rng.range(.07f, .19f);
    Vec3 axis{std::cos(angle), 0, std::sin(angle)}, side{-axis.z, 0, axis.x};
    Vec3 tip = p + axis * length, ridge = p + axis * (length * .48f) + Vec3{0, .018f, 0};
    Vec3 left = p + axis * (length * .43f) + side * (length * .27f);
    Vec3 right = p + axis * (length * .55f) - side * (length * .23f);
    leaf.element_random = rng.next();
    leaf.triangle(p, left, ridge); leaf.triangle(left, tip, ridge);
    leaf.triangle(tip, right, ridge); leaf.triangle(right, p, ridge);
    if (i % 7 == 0) twig.tube(p - axis * .035f, tip, .0035f, 4);
  }
}

void garden(Scene& sc, const Palette& mats, Rng rng) {
  // One physical west garden, connected to the original tower's loggia.
  // The city owns its continuous floor, supports, perimeter guard and route.
  const Vec2 finger_centre{-403.1f,381.f};
  auto east_edge=[](Vec2 centre,Vec2 half,float z) {
    const float v=std::min(std::fabs(z-centre.y)/half.y,.999f);
    return centre.x+half.x*std::pow(1-std::pow(v,3.1f),1/3.1f);
  };
  Rng finger=rng.child(40);
  const float finger_soil=bed(sc,mats,finger.child(1),finger_centre,{3.f,5.2f},kGardenFloor,.38f);
  understory(sc,finger.child(2),finger_centre,{2.45f,4.6f},finger_soil,.67f,true);
  evergreen_mass(sc,finger,{-401.65f,finger_soil,377.6f},1.05f,2.35f);
  evergreen_mass(sc,finger,{-401.65f,finger_soil,381.1f},.91f,2.25f);
  for(int i=0;i<6;++i) {
    const float z=376.7f+i*1.53f;
    const float fern_x=east_edge(finger_centre,{2.8f,5.0f},z)-.17f;
    const float fern_scale=.64f+finger.range(0.f,.06f);
    add_asset_instance(sc,"fern_arching_dry",{fern_x,finger_soil,z},finger.range(-kPi,kPi),fern_scale);
    plant(sc,finger,"groundcover",{east_edge(finger_centre,{2.8f,5.0f},z+.20f)-.20f,finger_soil,z+.20f},.83f);
    if(i==1||i==4)plant(sc,finger,"shrub_flowering",{fern_x-.55f,finger_soil,z+.26f},.43f);
    add_asset_instance(sc,"climber_cascade",{east_edge(finger_centre,{3.f,5.2f},z)-.035f,kGardenFloor+.29f,z},
                       kPi*.5f,{.61f,.29f,.61f});
  }
  // Folded tropical blades form a distinct low layer behind the visible timber
  // seat. Their full-size source geometry uses a uniform instance scale.
  add_asset_instance(sc,"garden_strelitzia",{-401.65f,finger_soil,376.9f},-.30f,.93f);

  // Continuous curved seating follows the bed's east face. Uniform arc-length
  // slats avoid the wide, piano-key gaps visible in the earlier east garden.
  Emit seat(&sc.opaque,mats.wood),support(&sc.opaque,mats.bronze);
  std::vector<Vec3> seat_curve;std::vector<float> distances{0};
  for(int i=0;i<=240;++i) {
    float angle=radians(-60.f+i*.5f);
    Vec3 p{finger_centre.x+3.30f*std::cos(angle),kGardenFloor+.46f,
           finger_centre.y+5.75f*std::sin(angle)};
    if(!seat_curve.empty())distances.push_back(distances.back()+length(p-seat_curve.back()));
    seat_curve.push_back(p);
  }
  std::size_t segment=1;int slat=0;Vec3 previous{};
  for(float distance=.045f;distance<distances.back()-.045f;distance+=.092f,++slat) {
    while(segment+1<distances.size()&&distances[segment]<distance)++segment;
    const float t=(distance-distances[segment-1])/(distances[segment]-distances[segment-1]);
    Vec3 p=lerp(seat_curve[segment-1],seat_curve[segment],t);
    Vec3 along=normalize(seat_curve[segment]-seat_curve[segment-1]);
    Vec3 outward{along.z,0,-along.x};
    seat.box(p,{.039f,.04f,.30f},along,{0,1,0},outward);
    if(slat%12==0)support.box(p-Vec3{0,.25f,0},{.045f,.21f,.24f},along,{0,1,0},outward);
    if(slat)support.beam(previous-Vec3{0,.09f,0},p-Vec3{0,.09f,0},.075f,.065f);
    previous=p;
  }
  seat.box({-399.96f,kGardenFloor+.518f,379.22f},{.13f,.018f,.18f});
  Emit cup(&sc.opaque,mats.ceramic);
  const Vec3 cup_base{-399.85f,kGardenFloor+.50f,380.0f};
  cup.frustum(cup_base,cup_base+Vec3{0,.095f,0},.044f,.050f,16,false);
  cup.torus(cup_base+Vec3{0,.095f,0},{0,1,0},.047f,.004f,16,6);
  Emit(&sc.opaque,mats.soil).polygon(plan_circle(.043f,16,{cup_base.x,cup_base.z}),cup_base.y+.084f,true);

  // A broad asymmetric fig reaches the upper image through real spreading
  // limbs. Its supported root and all leaf geometry retain uniform scale.
  const Vec2 northwest_centre{-410.5f,380.f};
  const float northwest=bed(sc,mats,rng.child(30),northwest_centre,{7.1f,11.3f},kGardenFloor,.56f);
  leaning_canopy(sc,{-404.7f,northwest,372.5f},.82f,-.25f);
  add_asset_instance(sc,"garden_fig_canopy",{-405.1f,northwest,377.0f},.30f,{1,1,1},{.88f,.96f,.87f});
  add_asset_instance(sc,"tree_multistem",{-410.7f,northwest,378.5f},.32f,1.12f);
  understory(sc,rng.child(31),northwest_centre,{6.45f,10.65f},northwest,.87f,true);
  for(int i=0;i<5;++i) {
    const float z=371.2f+i*4.25f;
    plant(sc,rng,"fern_arching_dry",{east_edge(northwest_centre,{6.9f,11.1f},z)-.23f,northwest,z},1.05f);
    if(i%2==0)evergreen_mass(sc,rng,{-406.9f,northwest,z},1.22f,2.65f);
    add_asset_instance(sc,"climber_cascade",{east_edge(northwest_centre,{7.1f,11.3f},z)-.035f,kGardenFloor+.47f,z},
                       kPi*.5f,{.92f,.64f,.92f});
  }

  // The larger planted room continues behind the viewer, with open circulation
  // from the tower entrance through (-397,402) to the fixed overlook camera.
  const Vec2 west_centre{-411.2f,406.f};
  const float west=bed(sc,mats,rng.child(7),west_centre,{5.8f,11.0f},kGardenFloor,.52f);
  understory(sc,rng.child(8),west_centre,{5.15f,10.35f},west,.87f,true);
  add_asset_instance(sc,"canopy_columnar",{-414.f,west,412.5f},.47f,.76f);
  add_asset_instance(sc,"tree_multistem",{-410.8f,west,400.4f},-.31f,.94f);
  add_asset_instance(sc,"palm_feather",{-408.4f,west,413.9f},.52f,.66f);
  for(float z:{397.f,405.f,413.f}) {
    Vec3 p=z==397.f?Vec3{-392.f,kGardenFloor,410.f}:Vec3{-404.f,kGardenFloor,z};
    add_asset_instance(sc,"planter_bench",p,kPi*.5f,{.92f,1,.83f});
    add_asset_instance(sc,"fern_arching_dry",p+Vec3{0,.8f,0},.3f,.91f);
    add_asset_instance(sc,"phormium",p+Vec3{0,.8f,1.65f},-.2f,.78f);
  }
  // Low planting under the rear curtain wall leaves its occupied rooms and
  // the source concept's upper bridge visible from the garden camera.
  const Vec2 rear_centre{-396.0f,417.8f};
  const float rear=bed(sc,mats,rng.child(5),rear_centre,{7.6f,2.3f},kGardenFloor,.45f);
  understory(sc,rng.child(6),rear_centre,{6.95f,1.65f},rear,.63f,true);

  Emit paving(&sc.opaque,M_MARBLE_WHITE),seam(&sc.opaque,mats.bronze);
  for(int row=0;row<11;++row)for(int col=0;col<3;++col) {
    paving.element_random=rng.next();
    const Vec3 at{-399.25f+col*1.35f,kGardenFloor+.016f,385.90f+row*1.76f};
    if(garden_surface_supported(at,{.665f,0,.87f}))paving.box(at,{.665f,.016f,.87f});
  }
  // The narrow seat-access strip rests on the dedicated finger extension.
  for(int row=0;row<8;++row) {
    const Vec3 at{-398.78f,kGardenFloor+.016f,375.65f+row*1.13f};
    if(garden_surface_supported(at,{.65f,0,.55f}))paving.box(at,{.65f,.016f,.55f});
  }
  const Vec3 path_axis=normalize(Vec3{9,0,3}),path_side{-path_axis.z,0,path_axis.x};
  for(int i=0;i<6;++i) {
    Vec3 p=Vec3{-394.50f,kGardenFloor+.016f,402.83f}+path_axis*(i*1.10f);
    if(garden_surface_supported(p,{.539f,0,1.38f},path_axis,path_side))
      paving.box(p,{.539f,.016f,1.38f},path_axis,{0,1,0},path_side);
  }
  for(float x:{-400.12f,-395.40f}) {
    for(int segment=0;segment<85;++segment) {
      const Vec3 at{x,kGardenFloor+.012f,386.6f+segment*.2f};
      if(garden_surface_supported(at,{.018f,0,.099f}))seam.box(at,{.018f,.010f,.099f});
    }
    for(float z:{390.2f,400.4f}) {
      const float fixture_x=x<-398&&z<395?-403.2f:x;
      const Vec3 at{fixture_x,kGardenFloor+.025f,z},drain{x,kGardenFloor,z+.9f};
      if(garden_instance_supported(sc,"bronze_light",at,kPi*.5f,{1,1,1}))accent_light(sc,at,kPi*.5f);
      if(garden_instance_supported(sc,"street_drain",drain,kPi*.5f,{.65f,1,1}))
        add_asset_instance(sc,"street_drain",drain,kPi*.5f,{.65f,1,1});
    }
  }
  add_asset_instance(sc,"roof_irrigation",{-416.3f,kGardenFloor,418.8f},.3f,.65f);
  add_asset_instance(sc,"street_access_cover",{-392.8f,kGardenFloor+.02f,413.3f},.4f,.7f);
  wind_litter(sc,mats,rng.child(11),{-400.20f,kGardenFloor,390.2f},{.36f,1.5f},44,true);
  wind_litter(sc,mats,rng.child(12),{-395.30f,kGardenFloor,392.4f},{.35f,2.1f},52,true);
  wind_litter(sc,mats,rng.child(13),{-397.3f,kGardenFloor,410.5f},{.50f,3.f},65,true);
}

void landing(Scene& sc, const Palette& mats, Rng rng) {
  // The close seating island carries a low shrub canopy, leaving the arrival
  // skyline visible above it and the occupied curved pavilion visible at right.
  Vec3 close_seat{194.f, kLandingFloor, 160.f};
  add_asset_instance(sc, "planter_bench", close_seat, -.325f, {1.08f, 1, 1});
  add_asset_instance(sc,"tree_multistem",{193.f,34.77f,159.65f},.35f,.30f);
  plant(sc, rng, "fern_arching", close_seat + Vec3{-1.65f, .8f, -.5f}, 1.03f);
  plant(sc, rng, "shrub_flowering", close_seat + Vec3{1.75f, .8f, .20f}, .40f);
  plant(sc, rng, "groundcover", close_seat + Vec3{-.50f, .8f, .30f}, 1.05f);
  // These are low-branching shrubs, not miniature trees: foliage starts near
  // the soil and forms overlapping rounded volumes instead of a raised, flat
  // hedge of exposed trunks. Keep true leaf curvature with uniform scales.
  // Locate every root in the authored bench's own rotated soil coordinates.
  const AssetInstance soil_frame{0, close_seat, -.325f, {1.08f, 1, 1}};
  auto low_plant = [&](std::string_view species, Vec2 local, float scale, float yaw) {
    add_asset_instance(sc, species,
                       asset_transform_point(soil_frame, {local.x, .77f, local.y}),
                       yaw, scale);
  };
  low_plant("shrub_flowering", {-1.90f, -.34f}, .44f, -.60f);
  low_plant("shrub_flowering", {-.82f, .16f}, .56f, 1.10f);
  low_plant("shrub_flowering", {.28f, -.12f}, .48f, -.15f);
  low_plant("shrub_flowering", {1.35f, .20f}, .55f, 2.20f);
  low_plant("shrub_flowering", {1.96f, -.48f}, .42f, .75f);
  for (int i = 0; i < 4; ++i) {
    low_plant("fern_arching", {-1.73f + i * 1.09f, .55f},
              i == 1 || i == 2 ? .61f : .53f, -.55f + i * 1.47f);
    low_plant("groundcover", {-1.49f + i * 1.07f, .08f}, .58f, i * .85f);
  }
  add_asset_instance(sc, "climber_cascade", close_seat + Vec3{-1.8f, .72f, -1.03f},
                     kPi - .325f, {.65f, .16f, .7f});
  Vec3 near_table{199.9f, kLandingFloor, 155.7f};
  add_asset_instance(sc, "street_table", near_table, -.15f, 1.f);
  add_asset_instance(sc, "street_seat", near_table + Vec3{.35f, 0, 1.12f}, kPi + .25f, 1.f);
  add_asset_instance(sc, "street_seat", near_table + Vec3{-.55f, 0, -1.15f}, .3f, 1.f);
  Emit cup(&sc.opaque, mats.ceramic), drink(&sc.opaque, mats.soil), wood(&sc.opaque, mats.wood);
  for (Vec3 offset : {Vec3{-.22f, .80f, .12f}, Vec3{.23f, .80f, -.16f}}) {
    Vec3 p = near_table + offset;
    cup.frustum(p, p + Vec3{0, .093f, 0}, .041f, .047f, 16, false);
    cup.wall(plan_circle(.039f, 16, {p.x, p.z}), p.y + .015f, p.y + .093f, true, false);
    cup.torus(p + Vec3{0, .093f, 0}, {0, 1, 0}, .044f, .004f, 16, 6);
    drink.polygon(plan_circle(.039f, 16, {p.x, p.z}), p.y + .092f, true);
    cup.torus(p + Vec3{.052f, .052f, 0}, {0, 0, 1}, .025f, .007f, 14, 6);
  }
  wood.box(near_table + Vec3{.05f, .817f, .25f}, {.13f, .017f, .17f},
           {.98f, 0, .2f}, {0, 1, 0}, {-.2f, 0, .98f});
  accent_light(sc, close_seat + Vec3{-1.4f, .06f, 1.65f}, -.325f);

  // Close the exposed soil gaps along the existing west bed with overlapping
  // foliage at ground, knee and waist heights; leave the planted tree trunks
  // legible instead of repeating a row of flower stems.
  Rng west_border=rng.child(63);
  constexpr float west_soil=kLandingFloor+.515f;
  for(int i=0;i<9;++i) {
    const float z=145.7f+i*2.35f;
    evergreen_mass(sc,west_border,{188.1f,west_soil,z},.74f+float(i%3)*.11f,2.25f);
    plant(sc,west_border,"fern_arching",{189.0f,west_soil,z+.7f},.76f);
    plant(sc,west_border,"groundcover",{188.9f,west_soil,z-.65f},1.02f);
  }
  // Thin dry paving bands, drainage and service hatches provide readable
  // foreground scale without filling the open route with unrelated objects.
  Emit stone(&sc.opaque, material(sc,"exposed landing stone")), bronze(&sc.opaque, mats.bronze);
  for (int z = 0; z < 15; ++z) for (int x = 0; x < 21; ++x) {
    float px = 192.9f + x * 2.30f + (z % 2) * 1.15f;
    float pz = 147.3f + z * 1.38f;
    // Entire tile stays outside the pavilion, including its curved facade.
    const float dx=std::max(std::fabs(px-224.f)-1.139f,0.f);
    const float dz=std::max(std::fabs(pz-144.f)-.679f,0.f);
    if(std::hypot(dx,dz)<24.3f)continue;
    stone.element_random = rng.next();
    stone.box({px, kLandingFloor + .012f, pz}, {1.139f, .012f, .679f});
  }
  for (float x : {199.7f, 250.3f}) {
    bronze.box({x, kLandingFloor + .020f, 157.f}, {.026f, .020f, 9.7f});
    add_asset_instance(sc, "street_drain", {x, kLandingFloor + .01f, 166.4f}, 0, 1.f);
  }
  add_asset_instance(sc, "street_access_cover", {250.8f, kLandingFloor + .02f, 166.2f}, .7f, 1.f);
  for (int i = 0; i < 2; ++i) {
    add_asset_instance(sc, "roof_service_cabinet", {254.f + i * 2.1f, kLandingFloor, 139.5f}, kPi, .75f);
    add_asset_instance(sc, "roof_irrigation", {257.f + i, kLandingFloor, 141.f}, .3f, .8f);
  }
  add_asset_instance(sc, "street_bollard", {250.5f, kLandingFloor, 145.5f}, 0, 1.f);
  add_asset_instance(sc, "street_bollard", {253.5f, kLandingFloor, 145.5f}, 0, 1.f);
  wind_litter(sc, mats, rng.child(21), {199.0f, kLandingFloor + .025f, 155.5f}, {.45f, .8f}, 40);
  wind_litter(sc, mats, rng.child(22), {190.5f, kLandingFloor, 155.5f}, {.45f, 4.f}, 55);
  wind_litter(sc, mats, rng.child(23), {202.2f, kLandingFloor + .025f, 159.f}, {1.6f, .45f}, 36);
}
}  // namespace

std::vector<std::vector<Vec2>> garden_supported_floor_plans() {
  return {{{-418.f,385.f},{-396.35f,385.f},{-402.f,394.f},{-418.f,394.f}},
          plan_rect(15.5f,13.5f,{-402.5f,407.5f}),
          plan_rounded_rect(7.5f,13.f,2.f,10,{-410.5f,380.f}),
          plan_rounded_rect(3.1f,7.5f,1.f,10,{-402.4f,382.5f}),
          plan_rounded_rect(1.25f,5.f,.5f,8,{-399.25f,380.f})};
}

void add_dry_fern_variant(Scene& sc) {
  constexpr std::string_view variant="fern_arching_dry";
  for(const auto& resource:sc.asset_library.resources)if(resource.name==variant)return;
  MeshResource fern=sc.asset_library.resources[asset_resource(sc.asset_library,"fern_arching")];
  fern.name=variant;
  std::array<std::pair<std::uint32_t,std::uint32_t>,2> remap;
  int entry=0;
  for(const char* source:{"leaf_middle","leaf_sunlit"}) {
    const auto original=material(sc,source);
    const std::string name=std::string(source)+"_dry_fern";
    std::uint32_t replacement=static_cast<std::uint32_t>(sc.materials.size());
    for(std::uint32_t i=0;i<sc.materials.size();++i)if(sc.materials[i].name==name){replacement=i;break;}
    if(replacement==sc.materials.size()) {
      auto leaf=sc.materials[original];leaf.name=name;leaf.roughness=.67f;
      sc.materials.push_back(std::move(leaf));
    }
    remap[entry++]={original,replacement};
  }
  for(auto& vertex:fern.mesh.vertices)
    for(const auto& [original,replacement]:remap)
      if(vertex.material==original){vertex.material=replacement;break;}
  sc.asset_library.resources.push_back(std::move(fern));
}

void stage_cinematic_gardens(Scene& sc, Rng rng,bool include_landing) {
  if (sc.asset_library.resources.empty()) return;
  add_dry_fern_variant(sc);
  add_garden_hero_plants(sc);
  const Palette mats(sc);
  const auto first = static_cast<std::uint32_t>(sc.opaque.indices.size());
  garden(sc, mats, rng.child(1));
  sc.register_range(first, static_cast<std::uint32_t>(sc.opaque.indices.size()),
                    {-402.5f, kGardenFloor + 1, 395}, 36.f);
  if(!include_landing)return;
  const auto landing_first = static_cast<std::uint32_t>(sc.opaque.indices.size());
  landing(sc, mats, rng.child(2));
  sc.register_range(landing_first, static_cast<std::uint32_t>(sc.opaque.indices.size()),
                    {225, kLandingFloor + 2, 150}, 46.f);
}
}  // namespace cb
