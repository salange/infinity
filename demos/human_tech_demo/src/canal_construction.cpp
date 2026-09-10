#include "canal_construction.hpp"
#include "riverfront_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace cb {
namespace {
using Material = std::uint32_t;
const Vec3 kUp{0, 1, 0};

Material named(const Scene& scene, std::string_view name, Material fallback) {
  for (Material i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name) return i;
  return fallback;  // The explicit procedural-only scene has no authored kit.
}
Material material(Scene& scene, MaterialDesc value) {
  for (Material i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == value.name) return i;
  scene.materials.push_back(std::move(value));
  return static_cast<Material>(scene.materials.size() - 1);
}
struct Palette {
  Material stone, waterline, joints, bronze, wood, soil, light;
  explicit Palette(Scene& scene) {
    auto desc = scene.materials[M_MARBLE_WHITE];
    desc.name = "canal honed grey limestone";
    // A dry, diffuse grey mineral face. Compensation is for the pinned marble
    // albedo only; the stone keeps its own veins and small normal variation.
    desc.base_color = scene.reviewed_material_maps
                          ? Vec3{.81f, .79f, .67f}
                          : Vec3{.51f, .50f, .47f};
    desc.flags = kMatTriplanar; desc.roughness = .92f;
    desc.normal_strength = .25f; desc.uv_scale = 1.5f; desc.metallic = 0;
    stone = material(scene, desc);
    desc.name = "canal submerged mineral course";
    desc.base_color = scene.reviewed_material_maps
                          ? Vec3{.34f, .39f, .34f}
                          : Vec3{.21f, .24f, .24f};
    desc.roughness = .75f; desc.normal_strength = .32f;
    waterline = material(scene, desc);
    joints = named(scene, "gasket_charcoal", M_DARK_METAL);
    bronze = named(scene, "bronze_satin", M_BRONZE);
    wood = named(scene, "wood_oiled", M_PANEL_WARM);
    soil = named(scene, "soil_mulch", M_SOIL);
    desc = {}; desc.name = "canal warm shielded diffuser";
    desc.base_color = {1, .77f, .42f}; desc.tint2 = {1, .69f, .32f};
    desc.flags = kMatEmissive; desc.emissive = 1.6f; desc.roughness = .52f;
    light = material(scene, desc);
  }
};

float segment_distance(Vec2 p, Vec2 a, Vec2 b) {
  const Vec2 d = b - a;
  const float t = std::clamp(dot(p - a, d) / std::max(dot(d, d), 1e-8f), 0.f, 1.f);
  return length(p - a - d * t);
}
float orient(Vec2 a, Vec2 b, Vec2 c) {
  const Vec2 u = b - a, v = c - a;
  return u.x * v.y - u.y * v.x;
}
bool segments_overlap(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  const float o0 = orient(a, b, c), o1 = orient(a, b, d);
  const float o2 = orient(c, d, a), o3 = orient(c, d, b);
  if (((o0 > 0 && o1 < 0) || (o0 < 0 && o1 > 0)) &&
      ((o2 > 0 && o3 < 0) || (o2 < 0 && o3 > 0))) return true;
  return segment_distance(a, c, d) < .001f || segment_distance(b, c, d) < .001f ||
         segment_distance(c, a, b) < .001f || segment_distance(d, a, b) < .001f;
}
bool overlap(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
  if (a.size() < 3 || b.size() < 3) return false;
  if (point_in_polygon(a, b.front()) || point_in_polygon(b, a.front())) return true;
  for (std::size_t i = 0; i < a.size(); ++i)
    for (std::size_t j = 0; j < b.size(); ++j)
      if (segments_overlap(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()])) return true;
  return false;
}
bool clear(const std::vector<Vec2>& footprint,
           const std::vector<CanalCrossing>& crossings,
           const std::vector<std::vector<Vec2>>& exclusions, float clearance = 1.f) {
  for (const auto& crossing : crossings) {
    const float margin = crossing.width * .5f + clearance;
    for (std::size_t i = 0; i < footprint.size(); ++i) {
      const Vec2 a = footprint[i], b = footprint[(i + 1) % footprint.size()];
      if (segment_distance(a, crossing.a, crossing.b) < margin ||
          segment_distance(crossing.a, a, b) < margin ||
          segment_distance(crossing.b, a, b) < margin ||
          segments_overlap(a, b, crossing.a, crossing.b)) return false;
    }
  }
  for (const auto& exclusion : exclusions) if (overlap(footprint, exclusion)) return false;
  return true;
}
std::vector<Vec2> rectangle(Vec2 centre, Vec2 along, Vec2 land, float half_length, float half_width) {
  return {centre - along * half_length - land * half_width,
          centre + along * half_length - land * half_width,
          centre + along * half_length + land * half_width,
          centre - along * half_length + land * half_width};
}
Vec3 at(Vec2 p, float y) { return {p.x, y, p.y}; }

void lantern(Scene& scene, const Palette& p, Vec2 position, float floor) {
  Emit bronze(&scene.opaque, p.bronze), light(&scene.opaque, p.light);
  bronze.box(at(position, floor + .055f), {.16f, .055f, .16f});
  for (float dx : {-.10f, .10f}) for (float dz : {-.10f, .10f})
    bronze.box({position.x + dx, floor + .53f, position.y + dz}, {.018f, .42f, .018f});
  light.box(at(position, floor + .52f), {.076f, .30f, .076f});
  for (int i = 0; i < 7; ++i) bronze.box(at(position, floor + .24f + i * .093f), {.12f, .016f, .12f});
  bronze.box(at(position, floor + .99f), {.16f, .035f, .16f});
  scene.lights.push_back({at(position, floor + .56f), 8.f, {1, .70f, .37f}, 1.5f});
}

void seat(Scene& scene, const Palette& p, Vec2 centre, Vec2 along, Vec2 land,
          float floor, float half_length) {
  Emit stone(&scene.opaque, p.stone), metal(&scene.opaque, p.bronze), wood(&scene.opaque, p.wood);
  const Vec3 cross_axis{-along.y, 0, along.x};
  for (float t : {-half_length + .30f, half_length - .30f}) {
    stone.box(at(centre + along * t, floor + .18f), {.15f, .18f, .25f}, at(along, 0), kUp, cross_axis);
    metal.box(at(centre + along * t, floor + .40f), {.075f, .06f, .29f}, at(along, 0), kUp, cross_axis);
  }
  for (int i = 0; i < 7; ++i)
    wood.box(at(centre + land * ((i - 3) * .085f), floor + .49f),
             {half_length, .035f, .033f}, at(along, 0), kUp, cross_axis);
  for (float t : {-half_length + .24f, half_length - .24f})
    metal.box(at(centre + along * t + land * .23f, floor + .65f), {.036f, .20f, .036f}, at(along, 0), kUp, cross_axis);
  for (int i = 0; i < 3; ++i)
    wood.box(at(centre + land * .26f, floor + .70f + i * .085f),
             {half_length, .033f, .035f}, at(along, 0), kUp, cross_axis);
}
void plant(Scene& scene, std::string_view name, Vec2 position, float soil, float height, float yaw) {
  if (scene.asset_library.resources.empty()) return;
  const auto id = asset_resource(scene.asset_library, name);
  const auto& mesh = scene.asset_library.resources[id].mesh;
  const float scale = height / (mesh.bounds_max.y - mesh.bounds_min.y);
  scene.asset_instances.push_back({id, at(position, soil - mesh.bounds_min.y * scale), yaw,
                                  {scale, scale, scale}, {1, 1, 1}});
}

void planted_seat(Scene& scene, const Palette& p, Vec2 centre, Vec2 along,
                  Vec2 land, float floor, int address, bool tree) {
  // A 2.7 m wide bed sits entirely on the existing landward promenade slab.
  // Its waterside seat stops 0.14 m before the protected 3.5 m walking band.
  const Vec2 half{4.9f + (address % 3) * .45f, 1.35f};
  const float yaw = std::atan2(-along.y, along.x);
  auto outer = plan_transform(plan_rounded_rect(half.x, half.y, .65f, 6), centre, -yaw);
  auto inner = plan_transform(plan_rounded_rect(half.x - .19f, half.y - .19f, .48f, 6), centre, -yaw);
  Emit stone(&scene.opaque, p.stone), soil(&scene.opaque, p.soil);
  stone.wall(outer, floor, floor + .58f, true);
  stone.ring_cap(outer, inner, floor + .58f);
  stone.wall(inner, floor + .44f, floor + .58f, true, false);
  soil.polygon(inner, floor + .45f, true);
  seat(scene, p, centre - land * 1.77f, along, land, floor, 2.2f);
  const float top = floor + .45f;
  for (int i = 0; i < 17; ++i) {
    const float x = -half.x + .70f + (2 * half.x - 1.40f) * i / 16.f;
    const float side = (i % 2 ? -.48f : .45f);
    const auto name = i % 5 == 0 ? "shrub_flowering" : i % 3 == 0 ? "phormium" : "groundcover";
    // Groundcover is naturally a 20 cm high spreading mat; preserve that
    // habit instead of scaling it into a metre-wide mass across the path.
    const float height = i % 5 == 0 ? 1.10f : i % 3 == 0 ? .93f : .095f;
    plant(scene, name, centre + along * x + land * side, top, height, (i * .71f + address) * .43f);
  }
  if (tree) {
    const char* species = address % 3 == 0 ? "canopy_columnar" : "canopy_broadleaf";
    plant(scene, species, centre + along * .9f, top, 6.8f + (address % 3) * .70f, yaw + .37f);
  }
  lantern(scene, p, centre - along * (half.x + .45f) - land * .80f, floor);
}

void shade(Scene& scene, const Palette& p, Vec2 centre, Vec2 along, Vec2 land, float floor) {
  Emit bronze(&scene.opaque, p.bronze), wood(&scene.opaque, p.wood), stone(&scene.opaque, p.stone);
  // Slim ribs span only the landward pocket; the public walk is uncovered.
  for (float x : {-3.3f, 3.3f}) for (float z : {-.92f, .92f}) {
    const Vec2 pos = centre + along * x + land * z;
    stone.box(at(pos, floor + .07f), {.21f, .07f, .21f});
    bronze.box(at(pos, floor + 1.68f), {.055f, 1.54f, .055f});
    bronze.beam(at(pos, floor + 2.7f), at(centre + along * (x * .80f) + land * z, floor + 3.25f), .065f, .085f);
  }
  for (float z : {-1.10f, 1.10f})
    bronze.beam(at(centre - along * 3.65f + land * z, floor + 3.27f),
                at(centre + along * 3.65f + land * z, floor + 3.27f), .11f, .19f);
  for (int i = 0; i < 32; ++i) {
    const float x = -3.60f + i * (7.20f / 31);
    wood.beam(at(centre + along * x - land * 1.20f, floor + 3.38f),
              at(centre + along * x + land * 1.20f, floor + 3.38f), .095f, .07f);
  }
  seat(scene, p, centre + land * .40f, along, land, floor, 2.7f);
}

void formed_edge(Scene& scene, Material material, Vec3 a, Vec3 b, Vec3 outside) {
  const std::array<Vec2, 6> profile{{{-.11f, -.035f}, {.065f, -.035f},
                                    {.11f, -.075f}, {.11f, -.16f},
                                    {.060f, -.205f}, {-.11f, -.205f}}};
  Emit emit(&scene.opaque, material);
  auto point = [&](Vec3 end, Vec2 p) { return end + outside * p.x + kUp * p.y; };
  for (std::size_t i = 0; i < profile.size(); ++i) {
    const Vec2 p = profile[i], q = profile[(i + 1) % profile.size()], d = q - p;
    const Vec3 normal = outside * -d.y + kUp * d.x;
    Vec3 aa = point(a, p), ab = point(a, q), ba = point(b, p), bb = point(b, q);
    if (dot(cross(ab - aa, bb - aa), normal) > 0) emit.quad_metric(aa, ab, bb, ba);
    else emit.quad_metric(ab, aa, ba, bb);
  }
  for (int end = 0; end < 2; ++end) {
    const Vec3 pos = end ? b : a, normal = normalize(b - a) * (end ? 1.f : -1.f);
    for (std::size_t i = 1; i + 1 < profile.size(); ++i) {
      Vec3 p = point(pos, profile[0]), q = point(pos, profile[i]), r = point(pos, profile[i + 1]);
      if (dot(cross(q - p, r - p), normal) < 0) std::swap(q, r);
      emit.triangle(p, q, r);
    }
  }
}
}  // namespace

void build_canal_bank(Scene& scene, Vec2 a, Vec2 b, float land_sign,
                      float floor_y, const std::vector<CanalCrossing>& crossings,
                      const std::vector<std::vector<Vec2>>& exclusions) {
  const Palette p(scene);
  const Vec2 along = normalize(b - a);
  Vec2 land{-along.y, along.x};
  if (land.x * land_sign < 0) land = land * -1.f;
  const Vec2 offset{land_sign * 5, 0};
  const Vec2 qa = a + offset, qb = b + offset;
  const float run = length(b - a);
  const int courses = std::max(1, static_cast<int>(std::ceil(run / 3.2f)));
  Emit stone(&scene.opaque, p.stone), wet(&scene.opaque, p.waterline), joints(&scene.opaque, p.joints);
  // Keep the exact original wall envelope and continuous water barrier. Open
  // masonry joints expose this recessed core rather than holes to the terrain.
  joints.beam(at(a, 0), at(b, 0), 1.36f, 2.4f);
  wet.beam(at(a, -.44f), at(b, -.44f), 1.4f, 1.52f);
  for (int i = 0; i < courses; ++i) {
    const float t0 = float(i) / courses + (i ? .014f / run : 0);
    const float t1 = float(i + 1) / courses - (i + 1 < courses ? .014f / run : 0);
    const Vec2 c = a + (b - a) * t0, d = a + (b - a) * t1;
    stone.beam(at(c, .69f), at(d, .69f), 1.4f, .74f);
    stone.beam(at(c, 1.13f), at(d, 1.13f), 1.4f, .14f);
  }
  // Existing 8 m promenade, exact .4 m thickness/top. Fine expansion inlays
  // are seated in the top course; no new level or detached slab is introduced.
  Emit(&scene.opaque, M_PLAZA).beam(at(qa, floor_y - .20f), at(qb, floor_y - .20f), 8, .4f);
  for (int i = 1; i < courses; ++i) {
    const Vec2 pos = qa + (qb - qa) * (float(i) / courses);
    Emit(&scene.opaque, p.bronze).beam(at(pos - land * 3.92f, floor_y - .006f),
                                    at(pos + land * 3.92f, floor_y - .006f), .025f, .018f);
  }
  // Rail posts are set into the coping and include a continuous lower curb.
  const Vec2 water_edge = land * -.60f;
  Emit bronze(&scene.opaque, p.bronze);
  bronze.beam(at(a + water_edge, 1.31f), at(b + water_edge, 1.31f), .065f, .13f);
  bronze.tube(at(a + water_edge, 2.28f), at(b + water_edge, 2.28f), .034f, 8, true);
  const int posts = std::max(1, static_cast<int>(std::ceil(run / 2.5f)));
  for (int i = 0; i < posts; ++i) {
    const Vec2 pos = a + (b - a) * (float(i) / posts) + water_edge;
    bronze.box(at(pos, 1.20f), {.08f, .045f, .08f});
    bronze.tube(at(pos, 1.20f), at(pos, 2.28f), .026f, 8, true);
  }
  const int address = std::abs(static_cast<int>(std::lround((a.y + b.y) * .5f))) + (land_sign > 0 ? 17 : 0);
  const float fraction = .40f + (address % 5) * .045f;
  const Vec2 centre = qa + (qb - qa) * fraction + land * 2.10f;
  const float half_length = 4.9f + (address % 3) * .45f;
  auto footprint = rectangle(centre - land * .40f, along, land, half_length + .90f, 1.86f);
  if (clear(footprint, crossings, exclusions)) {
    const bool tree = address % 4 != 0;
    bool crown_clear = true;
    if (tree) crown_clear = clear(rectangle(centre, along, land, 5, 5), crossings, {} , 2.f);
    planted_seat(scene, p, centre, along, land, floor_y, address, tree && crown_clear);
  }
  const float midpoint = (a.y + b.y) * .5f;
  if ((land_sign > 0 && midpoint > 90 && midpoint < 128) ||
      (land_sign < 0 && midpoint > -255 && midpoint < -217)) {
    const Vec2 pos = qa + (qb - qa) * .80f + land * 2.10f;
    const auto canopy = rectangle(pos, along, land, 3.85f, 1.35f);
    if (clear(canopy, crossings, exclusions) && !overlap(canopy, footprint))
      shade(scene, p, pos, along, land, floor_y);
  }
}

void build_canal_bridge_edge(Scene& scene, Vec3 a, Vec3 b, float width, bool arterial,
                             bool arrival_bridge) {
  const Palette p(scene);
  const Vec3 direction = normalize(b - a);
  const Vec3 side = normalize(cross(direction, kUp));
  const Vec3 slab_up = normalize(cross(side, direction));
  // Existing top is .035 m below asphalt and bottom .685 m below it.
  Emit stone(&scene.opaque, p.stone), bronze(&scene.opaque, p.bronze), dark(&scene.opaque, p.joints);
  // The formed edge replaces the outer 23 cm of the upper slab skin. Leaving
  // a full-width box behind it would bury its bevels and cause coplanar faces.
  stone.beam(a - kUp * .36f, b - kUp * .36f, width - .46f, .65f);
  stone.beam(a - kUp * .635f, b - kUp * .635f, width, .10f);
  const float walk_height=arrival_bridge?.16f:0.f;
  for (float sign : {-1.f, 1.f}) {
    const Vec3 offset = side * (sign * (width * .5f - .12f));
    const Vec3 backing = side * (sign * (width * .5f - .185f));
    dark.beam(a + backing - kUp * .385f, b + backing - kUp * .385f, .19f, .60f);
    // Layered edge stringers and their seated bronze nose remain within the
    // same slab depth. Longitudinal rails stay continuous across road pieces.
    bronze.beam(a + offset - kUp * .50f, b + offset - kUp * .50f, .16f, .22f);
    dark.beam(a + offset - kUp * .245f, b + offset - kUp * .245f, .18f, .065f);
    formed_edge(scene, p.stone, a + offset, b + offset, side * sign);
    if(arrival_bridge) {
      const Vec3 centre=side*(sign*(width*.5f-1.25f));
      stone.beam(a+centre+kUp*.08f,b+centre+kUp*.08f,2.2f,.16f);
      dark.beam(a+side*(sign*(width*.5f-2.36f))+kUp*.17f,
                b+side*(sign*(width*.5f-2.36f))+kUp*.17f,.065f,.024f);
      // A thin, continuous bearing rib keeps the span's underside readable.
      stone.beam(a+side*(sign*width*.31f)-kUp*.79f,
                 b+side*(sign*width*.31f)-kUp*.79f,.45f,.24f);
    }
    bronze.tube(a + offset + kUp * (1.08f+walk_height), b + offset + kUp * (1.08f+walk_height), .034f, 8, true);
    bronze.tube(a + offset + kUp * (.12f+walk_height), b + offset + kUp * (.12f+walk_height), .025f, 8, true);
    if (arterial)
      bronze.beam(a + offset + kUp * (.59f+walk_height), b + offset + kUp * (.59f+walk_height), .045f, .075f);
    else
      bronze.tube(a + offset + kUp * .55f, b + offset + kUp * .55f, .021f, 8, true);
    // Global dominant-axis stations prevent doubled posts at consecutive
    // 14 m slab seams. Half-open intervals assign each endpoint only once.
    const bool use_x = std::abs(direction.x) >= std::abs(direction.z);
    const float start = use_x ? a.x : a.z, end = use_x ? b.x : b.z;
    const float spacing = arterial ? 2.4f : 1.8f;
    const int first = static_cast<int>(std::ceil(std::min(start, end) / spacing));
    const int last = static_cast<int>(std::floor(std::max(start, end) / spacing));
    for (int i = first; i <= last; ++i) {
      const float t = (i * spacing - start) / (end - start);
      if (t < 0 || t >= 1) continue;
      const Vec3 base = lerp(a, b, t) + offset + kUp*walk_height;
      bronze.box(base + kUp * .025f, {.08f, .045f, .095f}, direction, slab_up, side);
      bronze.box(base + kUp * .55f, {.026f, .53f, .031f}, direction, slab_up, side);
      if (!arterial) {
        for (float delta : {-.42f, .42f})
          bronze.box(base + direction * delta + kUp * .59f, {.010f, .40f, .015f}, direction, slab_up, side);
      }
    }
    if(arrival_bridge) {
      for(int i=int(std::ceil(std::min(start,end)/1.8f));i<=int(std::floor(std::max(start,end)/1.8f));++i) {
        const float t=(i*1.8f-start)/(end-start);
        if(t<0||t>=1)continue;
        const Vec3 at=lerp(a,b,t)+side*(sign*(width*.5f-1.25f))+kUp*.171f;
        dark.box(at,{.012f,.004f,1.04f},direction,slab_up,side);
      }
      Emit light(&scene.opaque,p.light);
      for(int i=int(std::ceil(std::min(start,end)/18.f));i<=int(std::floor(std::max(start,end)/18.f));++i) {
        const float t=(i*18.f-start)/(end-start);
        if(t<0||t>=1)continue;
        const Vec3 foot=lerp(a,b,t)+side*(sign*(width*.5f-.38f))+kUp*walk_height;
        bronze.tube(foot,foot+kUp*3.65f,.036f,8,true);
        bronze.beam(foot+kUp*3.65f,foot+kUp*3.65f-side*(sign*.85f),.065f,.09f);
        const Vec3 diffuser=foot+kUp*3.59f-side*(sign*.66f);
        light.box(diffuser,{.12f,.025f,.19f},direction,slab_up,side);
        scene.lights.push_back({diffuser-kUp*.12f,9,{1,.76f,.46f},1.8f});
      }
    }
  }
}
void build_arrival_bridge_abutments(Scene& scene) {
  const Palette p(scene);
  Emit stone(&scene.opaque,p.stone),dark(&scene.opaque,p.joints),bronze(&scene.opaque,p.bronze);
  const Vec3 across{riverfront::across.x,0,riverfront::across.y};
  const Vec3 along{riverfront::along.x,0,riverfront::along.y};
  for(float sign:{-1.f,1.f}) {
    const Vec2 centre=riverfront::point(sign*24.f,0);
    stone.box(at(centre,1.8f),{2.45f,2.55f,10.1f},across,kUp,along);
    stone.box(at(centre,4.41f),{2.65f,.10f,10.3f},across,kUp,along);
    for(float side:{-6.2f,6.2f})
      bronze.box(at(centre,4.51f)+along*side,{1.9f,.075f,.35f},across,kUp,along);
    for(float y:{.45f,1.35f,2.25f,3.15f})
      dark.box(at(centre,y)-across*(sign*2.457f),{.008f,.014f,10.0f},across,kUp,along);
  }
}
}  // namespace cb
