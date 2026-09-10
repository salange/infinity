#include "canal_construction.hpp"
#include "canal_materials.hpp"
#include "city/rng.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace cb {
namespace {
using namespace canal_detail;

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
struct BankSite {
  Scene& scene;
  const Palette& palette;
  Vec2 origin, along, land;
  float run, floor;
  const std::vector<CanalCrossing>& crossings;
  const std::vector<std::vector<Vec2>>& exclusions;

  Vec2 point(float u, float v) const { return origin + along * u + land * v; }
  std::vector<Vec2> plan(float lo, float hi, float near, float far, float radius = 0) const {
    auto result = radius > 0
        ? plan_rounded_rect((hi-lo)*.5f, (far-near)*.5f, radius, 5,
                            {(lo+hi)*.5f, (near+far)*.5f})
        : plan_rect((hi-lo)*.5f, (far-near)*.5f, {(lo+hi)*.5f, (near+far)*.5f});
    for (auto& p : result) p = point(p.x, p.y);
    if (plan_area(result) < 0) std::reverse(result.begin(), result.end());
    return result;
  }
};

float local_area(const std::vector<Vec2>& plan) {
  if (plan.size()<3) return 0;
  double sum=0;
  for (std::size_t i=1;i+1<plan.size();++i) {
    const auto a=plan[i]-plan.front(),b=plan[i+1]-plan.front();
    sum+=double(a.x)*b.y-double(a.y)*b.x;
  }
  return float(sum*.5);
}
std::vector<Vec2> clean_plan(std::vector<Vec2> plan) {
  bool changed=true;
  while (changed&&plan.size()>=3) {
    changed=false;
    for (std::size_t i=0;i<plan.size();++i) {
      const auto a=plan[(i+plan.size()-1)%plan.size()],b=plan[i],c=plan[(i+1)%plan.size()];
      if (length(b-a)<.0001f||length(c-b)<.0001f||
          (std::abs(orient(a,b,c))<.00001f&&dot(b-a,c-b)>=0)) {
        plan.erase(plan.begin()+i);changed=true;break;
      }
    }
  }
  if (local_area(plan)<0) std::reverse(plan.begin(),plan.end());
  return plan;
}
void closed_slab(Scene& scene, Material mat, const std::vector<Vec2>& plan, float top, float depth) {
  const auto outline=clean_plan(plan);
  if (outline.size()<3||local_area(outline)<.0001f) return;
  Emit e(&scene.opaque, mat);
  // Clip-generated coping slivers can have tiny areas at large world
  // coordinates. Triangulate around a local origin to preserve their winding.
  const Vec2 origin=outline.front();
  auto local=outline;for(auto& p:local)p=p-origin;
  const auto first=scene.opaque.vertices.size();
  e.polygon(local, top, true); e.polygon(local, top-depth, false);
  for(auto i=first;i<scene.opaque.vertices.size();++i) {
    auto& v=scene.opaque.vertices[i];v.position.x+=origin.x;v.position.z+=origin.y;
    v.uv=v.uv+origin;v.aux.x+=origin.x;v.aux.y+=origin.y;
  }
  e.wall(outline, top-depth, top, true);
}
std::vector<std::vector<Vec2>> subtract_convex_plan(const std::vector<Vec2>& source,
                                                  const std::vector<Vec2>& obstacle) {
  std::vector<std::vector<Vec2>> outside;
  auto remaining = source;
  const float winding = plan_area(obstacle)>0 ? 1.f : -1.f;
  for (std::size_t i=0; i<obstacle.size() && remaining.size()>=3; ++i) {
    const Vec2 p=obstacle[i], d=obstacle[(i+1)%obstacle.size()]-p;
    const Vec2 inward{-d.y*winding,d.x*winding};
    auto part=clean_plan(clip_halfplane(remaining,p,inward*-1.f));
    if (part.size()>=3 && local_area(part)>.0001f) outside.push_back(std::move(part));
    remaining=clean_plan(clip_halfplane(remaining,p,inward));
  }
  return outside;
}

std::vector<std::vector<Vec2>> subtract_plans(std::vector<std::vector<Vec2>> pieces,
                                             const std::vector<Vec2>& hole) {
  if (hole.empty()) return pieces;
  std::vector<std::vector<Vec2>> result;
  for (const auto& piece : pieces) {
    if (!overlap(piece, hole)) { result.push_back(piece); continue; }
    for (auto& part : subtract_convex_plan(piece, hole)) result.push_back(std::move(part));
  }
  return result;
}
bool inside(const std::vector<std::vector<Vec2>>& pieces, Vec2 p) {
  for (const auto& piece : pieces) if (point_in_polygon(piece, p)) return true;
  return false;
}

bool rooted_plant(const BankSite& site, std::string_view name, Vec2 position,
                   float soil, float height, float yaw,
                   const std::vector<std::vector<Vec2>>& soil_plans) {
  auto& scene = site.scene;
  if (scene.asset_library.resources.empty() || !inside(soil_plans, position)) return false;
  const auto id = asset_resource(scene.asset_library, name);
  const auto& mesh = scene.asset_library.resources[id].mesh;
  const float scale = height / (mesh.bounds_max.y - mesh.bounds_min.y);
  const AssetInstance instance{id, at(position, soil - mesh.bounds_min.y * scale), yaw,
                                {scale, scale, scale}, {1, 1, 1}};
  // The whole projected crown must avoid occupied parcels and crossing roads.
  // The separate height-aware check allows branches above the public walk.
  Vec2 lo{1e20f, 1e20f}, hi{-1e20f, -1e20f};
  for (float x : {mesh.bounds_min.x, mesh.bounds_max.x})
    for (float z : {mesh.bounds_min.z, mesh.bounds_max.z}) {
      const auto p = asset_transform_point(instance, {x, 0, z});
      lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.z);
      hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.z);
    }
  if (!clear({lo, {hi.x, lo.y}, hi, {lo.x, hi.y}}, site.crossings, site.exclusions)) return false;
  for (const auto& vertex : mesh.vertices) {
    const Vec3 p = asset_transform_point(instance, vertex.position);
    const Vec2 q{p.x, p.z}, delta = q - site.origin;
    if (p.y < soil + .025f && !inside(soil_plans, q)) return false;
    const float u = dot(delta, site.along), v = dot(delta, site.land);
    if (u >= 0 && u <= site.run && v > -3.8f && v < -.3f &&
        p.y > site.floor + .25f && p.y < site.floor + 2.25f) return false;
  }
  scene.asset_instances.push_back(instance);
  return true;
}

void shade(Scene& scene, const Palette& p, Vec2 centre, Vec2 along, Vec2 land, float floor) {
  Emit bronze(&scene.opaque, p.bronze), wood(&scene.opaque, p.wood), stone(&scene.opaque, p.stone);
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

void garden_band(const BankSite& site, float lo, float hi, int address) {
  if (hi-lo < 3.5f) return;
  auto& scene = site.scene;
  const auto& p = site.palette;
  Rng rng = root_rng("canal-bank-gardens").child(28, address);
  float far = 3.72f;
  // Broader bays grow only into genuinely unoccupied land. Their slab joins
  // the original eight-metre promenade at its exact outer edge, y1.2.
  for (float candidate : {8.8f, 6.8f, 4.8f}) {
    if (hi-lo > 13 && clear(site.plan(lo, hi, .65f, candidate+.20f), site.crossings, site.exclusions)) {
      far = candidate; break;
    }
  }
  if (far > 4) {
    closed_slab(scene, p.stone, site.plan(lo, hi, 4, far+.20f), site.floor, site.floor-.65f);
  }
  const float corner = std::min(1.4f, (hi-lo)*.18f);
  const auto outer = site.plan(lo, hi, .75f, far, corner);
  const auto inner = plan_offset(outer, -.18f);
  const bool court = far > 6 && hi-lo > 21 && address % 3 != 0;
  const float court_u = lo + (hi-lo) * (address % 2 ? .70f : .30f);
  const auto opening = court ? site.plan(court_u-4.2f, court_u+4.2f, .50f, 5.9f) : std::vector<Vec2>{};
  const auto outer_parts = subtract_plans({outer}, opening);
  const auto soil_parts = subtract_plans({inner}, opening.empty() ? opening : plan_offset(opening, .18f));
  const float soil_y = site.floor + .44f + (address % 3)*.04f;
  for (const auto& piece : outer_parts)
    closed_slab(scene, p.stone, piece, soil_y-.08f, soil_y-.08f-site.floor);
  auto rim_parts=outer_parts;
  for (const auto& piece : soil_parts) rim_parts=subtract_plans(std::move(rim_parts),piece);
  for (const auto& piece : rim_parts)
    closed_slab(scene, p.stone, piece, soil_y+.10f, soil_y+.10f-site.floor);
  for (const auto& piece : soil_parts)
    closed_slab(scene, p.soil, piece, soil_y, .08f);
  // The soil is recessed into the retaining rim, with a closed supporting
  // floor underneath. Mown/open walking space is never planted over.
  const int columns = std::max(1, int((hi-lo)/1.25f));
  const int rows = std::max(2, int((far-.95f)/.95f));
  for (int i=0; i<columns; ++i) for (int row=0; row<rows; ++row) {
    auto r = rng.child(1, i, row);
    const float u = lo+.45f+(hi-lo-.90f)*(i+r.range(.12f,.88f))/columns;
    const float v = 1.04f+(far-1.40f)*(row+r.range(.18f,.82f))/rows;
    const Vec2 root = site.point(u,v);
    const int kind = (i*3+row+address)%11;
    const auto species = kind<6 ? "groundcover" : kind<9 ? "fern_arching" : kind==9 ? "shrub_flowering" : "phormium";
    const float height = kind<6 ? r.range(.13f,.20f) : kind<9 ? r.range(.60f,.90f) : kind==9 ? r.range(.90f,1.20f) : r.range(.8f,1.10f);
    rooted_plant(site, species, root, soil_y, height, r.range(0,6.2831853f), soil_parts);
  }
  // Two uneven groups, rather than an avenue of equal isolated tree dots.
  // Fine native crowns keep their branching form through uniform scaling.
  const int trees = std::max(2, int((hi-lo)/5.1f)) + (far>6 ? 2 : 0);
  for (int tree=0; tree<trees; ++tree) for (int attempt=0; attempt<10; ++attempt) {
    auto r = rng.child(2, tree, attempt);
    const float cluster = tree < (trees+1)/2 ? .27f : .77f;
    const float u = std::clamp(lo+(hi-lo)*cluster+r.range(-5.5f,5.5f), lo+.7f, hi-.7f);
    const float v = far>6 ? r.range(2.2f,far-1.2f) : r.range(1.75f,2.75f);
    const bool multi = tree%4==0;
    const float height = multi ? r.range(6.2f,7.8f) : r.range(8.2f,11.3f);
    if (rooted_plant(site, multi ? "tree_multistem" : "canopy_broadleaf",
                     site.point(u,v), soil_y, height, r.range(0,6.2831853f), soil_parts)) break;
  }
  if (court) {
    shade(scene, p, site.point(court_u,2.45f), site.along, site.land, site.floor);
    lantern(scene, p, site.point(court_u+3.85f,.33f), site.floor);
  } else if (hi-lo>9) {
    // The outer edge of the seat is 16 cm beyond the protected walking band.
    const float u = lo+(hi-lo)*.54f;
    seat(scene,p,site.point(u,.15f),site.along,site.land,site.floor,std::min(3.2f,(hi-lo)*.22f));
    lantern(scene,p,site.point(std::min(hi-.3f,u+4.0f),.35f),site.floor);
  }
}

}  // namespace

void build_canal_bank(Scene& scene, Vec2 a, Vec2 b, float land_sign,
                      float floor_y, const std::vector<CanalCrossing>& crossings,
                      const std::vector<std::vector<Vec2>>& exclusions) {
  const Palette p(scene);
  const Vec2 along = normalize(b-a);
  Vec2 land{-along.y,along.x};
  if (land.x*land_sign<0) land=land*-1.f;
  const Vec2 qa=a+Vec2{land_sign*5,0}, qb=b+Vec2{land_sign*5,0};
  const float run=length(b-a);
  const BankSite site{scene,p,qa,along,land,run,floor_y,crossings,exclusions};
  Emit stone(&scene.opaque,p.stone), wet(&scene.opaque,p.waterline), joints(&scene.opaque,p.joints);
  Emit bronze(&scene.opaque,p.bronze);
  // Exact retained barrier envelope: the waterline and clear canal are fixed.
  joints.beam(at(a,0),at(b,0),1.36f,2.4f);
  wet.beam(at(a,-.44f),at(b,-.44f),1.4f,1.52f);
  const int courses=std::max(1,int(std::ceil(run/3.2f)));
  for (int i=0;i<courses;++i) {
    const float u0=run*i/courses+(i?.014f:0),u1=run*(i+1)/courses-(i+1<courses?.014f:0);
    const Vec2 c=a+along*u0,d=a+along*u1;
    stone.beam(at(c,.48f),at(d,.48f),1.4f,.32f);
    stone.beam(at(c,.865f),at(d,.865f),1.4f,.42f);
    stone.beam(at(c,1.14f),at(d,1.14f),1.4f,.12f);
  }
  // Honed mineral paving sits on the original eight-metre, 40 cm slab.
  // Narrow recessed joints/drains articulate it without changing the floor.
  // The original slab bottom is y0.80, above the terrain at y0.65. A
  // continuous foundation embeds 5 cm into that terrain and overlaps the
  // slab underside by 6 cm; the visible floor and plan remain unchanged.
  stone.beam(at(qa,floor_y-.47f),at(qb,floor_y-.47f),8,.26f);
  stone.beam(at(qa,floor_y-.20f),at(qb,floor_y-.20f),8,.4f);
  for (int i=1;i<courses;++i) {
    const Vec2 pos=qa+(qb-qa)*(float(i)/courses);
    joints.beam(at(pos-land*3.97f,floor_y-.012f),at(pos+land*3.97f,floor_y-.012f),.018f,.024f);
  }
  for (float v:{-3.91f,-.18f})
    joints.beam(at(qa+land*v,floor_y-.022f),at(qb+land*v,floor_y-.022f),.075f,.044f);
  for (float u=1.2f;u<run;u+=.65f) {
    const Vec2 pos=site.point(u,-.18f);
    bronze.beam(at(pos-along*.012f,floor_y-.004f),at(pos+along*.012f,floor_y-.004f),.080f,.008f);
  }
  // A continuous slender bronze guard remains subordinate to the garden mass.
  const Vec2 water_edge=land*-.60f;
  bronze.beam(at(a+water_edge,1.31f),at(b+water_edge,1.31f),.065f,.13f);
  bronze.tube(at(a+water_edge,2.28f),at(b+water_edge,2.28f),.034f,8,true);
  for (float y:{1.67f,1.96f})bronze.tube(at(a+water_edge,y),at(b+water_edge,y),.013f,6,true);
  const int posts=std::max(1,int(std::ceil(run/2.5f)));
  for (int i=0;i<posts;++i) {
    const Vec2 pos=a+(b-a)*((i+.5f)/posts)+water_edge;
    bronze.box(at(pos,1.20f),{.08f,.045f,.08f});
    bronze.tube(at(pos,1.20f),at(pos,2.28f),.026f,8,true);
  }
  // Bollards and water outlets are attached to the existing wall, safely away
  // from crossing structures and from the uninterrupted walking corridor.
  for (float u:{run*.18f,run*.76f}) {
    const Vec2 pos=a+along*u;
    if (!clear(rectangle(pos,along,land,.6f,.65f),crossings,exclusions))continue;
    bronze.box(at(pos,1.24f),{.16f,.04f,.16f});
    bronze.tube(at(pos,1.28f),at(pos,1.57f),.075f,10,true);
    bronze.beam(at(pos-along*.24f,1.57f),at(pos+along*.24f,1.57f),.12f,.12f);
    bronze.tube(at(pos-land*.56f,.62f),at(pos-land*.705f,.62f),.055f,10,true);
  }
  // Merge adjacent free cells into long planted bands. Occupied lots and the
  // complete road approach footprints cut real gaps rather than suppressing
  // all planting on a 38-metre chord because one end meets a crossing.
  const int cells=std::max(1,int(std::ceil(run/2.f)));
  int start=-1,band=0;
  const int address=int(std::lround((a.y+b.y)*.5f))*2+(land_sign>0?1:0)+10000;
  for (int cell=0;cell<=cells;++cell) {
    const float lo=run*cell/cells,hi=run*(cell+1)/cells;
    const bool available=cell<cells&&clear(site.plan(lo,hi,.62f,3.92f),crossings,exclusions);
    if (available&&start<0)start=cell;
    if (!available&&start>=0) {
      garden_band(site,run*start/cells,run*cell/cells,address+band*131);
      ++band;start=-1;
    }
  }
}

}  // namespace cb
