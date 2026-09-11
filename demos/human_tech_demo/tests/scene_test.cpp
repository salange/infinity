// Headless validation of the authored set and renderer-facing scene contract.
#include "scene.hpp"
#include "arrival_tower_lots.hpp"
#include "city_routes.hpp"
#include "riverfront_layout.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace {
struct PlanPoint { double s,t; };
using Plan = std::vector<PlanPoint>;
PlanPoint survey(cb::Vec2 p) {
  const auto q=cb::riverfront::coordinates(p);return {q.x,q.y};
}
double area(const Plan& p) {
  double result=0;
  for(std::size_t i=0;i<p.size();++i) {
    const auto a=p[i],b=p[(i+1)%p.size()];result+=a.s*b.t-a.t*b.s;
  }
  return result*.5;
}
Plan halfplane(const Plan& input,PlanPoint a,PlanPoint b,double sign) {
  Plan output;
  auto distance=[&](PlanPoint p){return ((b.s-a.s)*(p.t-a.t)-(b.t-a.t)*(p.s-a.s))*sign;};
  for(std::size_t i=0;i<input.size();++i) {
    const auto p=input[i],q=input[(i+1)%input.size()];
    const double dp=distance(p),dq=distance(q);
    if(dp>=0)output.push_back(p);
    if((dp<0)!=(dq<0)) {
      const double t=dp/(dp-dq);output.push_back({p.s+(q.s-p.s)*t,p.t+(q.t-p.t)*t});
    }
  }
  return output;
}
Plan intersection(Plan p,const Plan& boundary) {
  const double sign=area(boundary)>0?1:-1;
  for(std::size_t i=0;i<boundary.size()&&!p.empty();++i)
    p=halfplane(p,boundary[i],boundary[(i+1)%boundary.size()],sign);
  return p;
}
std::vector<Plan> subtract(Plan p,const Plan& boundary) {
  std::vector<Plan> result;const double sign=area(boundary)>0?1:-1;
  for(std::size_t i=0;i<boundary.size()&&!p.empty();++i) {
    const auto a=boundary[i],b=boundary[(i+1)%boundary.size()];
    auto outside=halfplane(p,a,b,-sign);
    if(std::abs(area(outside))>.000001)result.push_back(std::move(outside));
    p=halfplane(p,a,b,sign);
  }
  return result;
}
void subtract(std::vector<Plan>& pieces,const Plan& boundary) {
  std::vector<Plan> result;
  for(auto& piece:pieces)for(auto remainder:subtract(std::move(piece),boundary))
    result.push_back(std::move(remainder));
  pieces=std::move(result);
}
std::string arrival_street_geometry(const cb::Scene& scene) {
  std::vector<Plan> domains,roads;
  for(const auto& footprint:cb::arrival_tower_cleanup_footprints()) {
    Plan p;for(auto q:footprint)p.push_back(survey(q));domains.push_back(std::move(p));
  }
  for(const auto& road:cb::arrival_tower_block_roads()) {
    const auto a=survey(road.a),b=survey(road.b);
    const double length=std::hypot(b.s-a.s,b.t-a.t);
    const PlanPoint n{-(b.t-a.t)*road.width/(2*length),(b.s-a.s)*road.width/(2*length)};
    roads.push_back({{a.s-n.s,a.t-n.t},{b.s-n.s,b.t-n.t},{b.s+n.s,b.t+n.t},{a.s+n.s,a.t+n.t}});
  }
  double expected_area=0,physical_area=0;
  for(const auto& domain:domains)for(std::size_t i=0;i<roads.size();++i) {
    std::vector<Plan> parts{intersection(roads[i],domain)};
    for(std::size_t j=0;j<i;++j)subtract(parts,roads[j]);
    for(const auto& part:parts)expected_area+=std::abs(area(part));
  }
  const auto& mesh=scene.opaque;std::size_t checked=0;
  for(std::size_t i=0;i<mesh.indices.size();i+=3) {
    const auto& va=mesh.vertices[mesh.indices[i]];
    if(va.material!=cb::M_ASPHALT)continue;
    const auto a=va.position,b=mesh.vertices[mesh.indices[i+1]].position,c=mesh.vertices[mesh.indices[i+2]].position;
    if(std::min({a.y,b.y,c.y})<.6f||std::max({a.y,b.y,c.y})>1.3f||cb::cross(b-a,c-a).y<=0)continue;
    const Plan triangle{survey({a.x,a.z}),survey({b.x,b.z}),survey({c.x,c.z})};
    for(const auto& domain:domains) {
      auto clipped=intersection(triangle,domain);const double surface=std::abs(area(clipped));
      if(surface<.00001)continue;
      ++checked;physical_area+=surface;
      std::vector<Plan> remainder{std::move(clipped)};
      for(const auto& road:roads)subtract(remainder,road);
      double undeclared=0;for(const auto& part:remainder)undeclared+=std::abs(area(part));
      if(undeclared>.02) {
        const auto p=remainder.front().front();
        return "legacy asphalt remains inside the square district cleanup at survey ("+
          std::to_string(p.s)+","+std::to_string(p.t)+"), "+std::to_string(undeclared)+" square metres outside the six intended streets";
      }
    }
  }
  if(std::abs(physical_area-expected_area)>2)
    return "square district asphalt is duplicated or missing: actual area "+std::to_string(physical_area)+
      ", six-street union "+std::to_string(expected_area);
  std::printf("square district streets: %zu actual asphalt triangles, %.2f square metres, no legacy road fragments\n",checked,physical_area);
  return {};
}
std::string arrival_route_metadata() {
  const auto routes=cb::scene_routes(true);
  const cb::SceneRoute* circuit=nullptr;const cb::SceneRoute* civic=nullptr;
  for(const auto& route:routes) {
    if(route.id=="landing_access"||route.id=="landing_internal_stair")
      return "retired landing stairs still appear in the authored district routes";
    if(route.id=="arrival_block_promenade")circuit=&route;
    if(route.id=="civic_public_walk")civic=&route;
  }
  if(!circuit||circuit->stairs||circuit->waypoints.size()<5)
    return "square district requires its continuous ground promenade";
  if(!civic||civic->waypoints.empty()||
     cb::length(civic->waypoints.back().position-circuit->waypoints.front().position)>.01f)
    return "square district promenade is disconnected from the civic public walk";
  if(cb::length(circuit->waypoints.front().position-circuit->waypoints.back().position)>.01f)
    return "square district promenade is not a closed circuit";
  for(const auto& waypoint:circuit->waypoints)
    if(cb::length(waypoint.target-waypoint.position)<.5f||std::abs(waypoint.position.y-3)>.01f)
      return "square district promenade has an invalid ground camera or zero-length view direction";
  return {};
}
std::uint64_t geometry_hash(const cb::Scene &scene) {
  std::uint64_t hash = 1469598103934665603ull;
  auto word = [&](std::uint32_t v) {
    hash ^= v;
    hash *= 1099511628211ull;
  };
  for (const auto *mesh : {&scene.opaque, &scene.foliage}) {
    for (const auto &v : mesh->vertices) {
      for (float f : {v.position.x, v.position.y, v.position.z, v.normal.x,
                      v.normal.y, v.normal.z, v.aux.x, v.aux.y, v.aux.z,
                      v.aux.w})
        word(std::bit_cast<std::uint32_t>(f));
      word(v.material);
    }
    for (auto i : mesh->indices)
      word(i);
  }
  for (const auto &instance : scene.asset_instances) {
    word(instance.resource);
    for (float value : {instance.translation.x, instance.translation.y,
                        instance.translation.z, instance.yaw, instance.scale.x,
                        instance.scale.y, instance.scale.z, instance.tint.x,
                        instance.tint.y, instance.tint.z})
      word(std::bit_cast<std::uint32_t>(value));
  }
  return hash;
}
} // namespace
int main(int argc, char **argv) {
  auto fail = [](const char *message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
  };
  cb::SceneParams params;
  if (argc > 1)
    params.asset_kit = argv[1];
  if(!params.asset_kit.empty()) {
    const auto route_error=arrival_route_metadata();
    if(!route_error.empty())return fail(route_error.c_str());
  }
  std::uint64_t canonical_hash = 0;
  {
    const cb::Scene sc = cb::generate_scene(params);
    if (sc.opaque.vertices.empty())
      return fail("empty city");
    if(!params.asset_kit.empty()) {
      const auto street_error=arrival_street_geometry(sc);
      if(!street_error.empty())return fail(street_error.c_str());
    }
    std::set<int> companion_floors;
    float companion_u_min = 10000, companion_u_max = -10000;
    for (const auto *mesh : {&sc.opaque, &sc.foliage}) {
      if (mesh->indices.size() % 3)
        return fail("incomplete mesh");
      for (const auto &v : mesh->vertices) {
        if (!std::isfinite(v.position.x + v.position.y + v.position.z +
                           v.normal.x + v.normal.y + v.normal.z + v.uv.x +
                           v.uv.y) ||
            v.material >= sc.materials.size())
          return fail("invalid vertex/material");
        // The companion facade must carry its actual metre/floor coordinates,
        // rather than cloning a zero-origin room onto every small pane.
        const float companion_radius = std::hypot(v.position.x + 386.f,
                                                  v.position.z - 300.f);
        if (v.material == cb::M_GLASS_BLUE &&
            std::abs(companion_radius - 17.65f) < .01f &&
            v.position.y > 13.f && v.position.y < 322.f) {
          if (std::abs(v.aux.y - v.position.y) > .005f)
            return fail("companion window coordinates restart per pane");
          companion_floors.insert(static_cast<int>(v.position.y / 4));
          companion_u_min = std::min(companion_u_min, v.aux.x);
          companion_u_max = std::max(companion_u_max, v.aux.x);
        }
      }
      for (auto index : mesh->indices)
        if (index >= mesh->vertices.size())
          return fail("out of bounds index");
    }
    if (companion_floors.size() < 75 || companion_u_max-companion_u_min < 100)
      return fail("companion facade lacks continuous room/floor identities");
    std::size_t end = 0;
    for (const auto &draw : sc.draws) {
      if (draw.first != end || draw.count % 3 ||
          draw.first + draw.count > sc.opaque.indices.size())
        return fail("draw ranges do not partition the index buffer");
      if (draw.radius < 0 || !std::isfinite(draw.radius + draw.centre.x +
                                            draw.centre.y + draw.centre.z))
        return fail("invalid culling bounds");
      end = draw.first + draw.count;
    }
    if (end != sc.opaque.indices.size())
      return fail("unregistered geometry");
    for (const auto &instance : sc.asset_instances) {
      if (instance.resource >= sc.asset_library.resources.size() ||
          !std::isfinite(cb::length(instance.translation) +
                         cb::length(instance.scale) +
                         cb::length(instance.tint) + instance.yaw) ||
          instance.scale.x <= 0 || instance.scale.y <= 0 ||
          instance.scale.z <= 0)
        return fail("invalid authored instance placement");
      if (sc.asset_library.resources[instance.resource].name.ends_with(
              "_mid")) {
        for (const char *shot :
             {"aerial", "galaxy", "civic", "street", "garden", "landing"}) {
          cb::Vec3 camera, target;
          cb::shot_camera(shot, camera, target);
          if (cb::length(instance.translation - camera) < 100.f)
            return fail("distant botanical detail used beside a review camera");
        }
      }
    }
    bool dielectric_water = false;
    for (const auto &material : sc.materials) {
      if (!std::isfinite(material.roughness + material.metallic) ||
          material.roughness < 0 || material.roughness > 1 ||
          material.metallic < 0 || material.metallic > 1)
        return fail("invalid material factors");
      if ((material.flags & 128u) &&
          (!std::isfinite(material.room_w + material.room_h + material.room_d +
                          material.lit_probability) ||
           material.room_w < 1 || material.room_w > 2.5f ||
           material.room_h < 0 || material.room_h > 1 || material.room_d < 0 ||
           material.room_d > 1 || material.lit_probability <= 0))
        return fail("invalid optical factors in scene glazing");
      if (material.name == "water")
        dielectric_water = material.metallic == 0;
    }
    if (!dielectric_water)
      return fail("water must be dielectric");
    if (sc.stats_towers < 100 || sc.stats_standards < 300 ||
        sc.lights.size() <= 64)
      return fail("incomplete city or truncated lighting");
    for (const auto &light : sc.lights)
      if (light.radius <= 0 ||
          !std::isfinite(light.position.x + light.position.y +
                         light.position.z + light.intensity))
        return fail("invalid practical light");
    canonical_hash = geometry_hash(sc);
    std::printf("valid city: %zu opaque triangles, %d towers, %d buildings, "
                "%zu draws, %zu lights\n",
                sc.opaque.triangle_count(), sc.stats_towers, sc.stats_standards,
                sc.draws.size(), sc.lights.size());
  }
  // Camera presets must select a viewpoint, never a different city or backdrop.
  cb::SceneParams night_params = params;
  night_params.shot = "galaxy";
  if (geometry_hash(cb::generate_scene(night_params)) != canonical_hash)
    return fail("camera changes world geometry");
  std::set<std::string> positions;
  for (const char *shot :
       {"aerial", "galaxy", "civic", "street", "garden", "landing"}) {
    cb::Vec3 p, t;
    if (!cb::shot_camera(shot, p, t) || p.y < 1.8f || cb::length(t - p) < 1)
      return fail("invalid capture camera");
    const float fov = cb::shot_fov_degrees(shot);
    if (fov < 15 || fov > 100)
      return fail("invalid camera field of view");
    positions.insert(std::to_string(p.x) + ":" + std::to_string(p.y) + ":" +
                     std::to_string(p.z));
  }
  if (positions.size() != 6)
    return fail("capture cameras must be distinct");
  // The terrace alias now selects the district's ground court while retaining
  // its established narrower lens. Retired stair routes are checked above.
  cb::Vec3 landing_p, landing_t, terrace_p, terrace_t;
  cb::shot_camera("landing", landing_p, landing_t);
  cb::shot_camera("terrace", terrace_p, terrace_t);
  if (std::abs(cb::shot_fov_degrees("landing") - 48.f) > .001f ||
      cb::shot_fov_degrees("terrace") != cb::shot_fov_degrees("landing") ||
      cb::length(landing_p - terrace_p) > .001f ||
      cb::length(landing_t - terrace_t) > .001f)
    return fail("landing camera and terrace alias disagree");
  cb::Vec3 p, t;
  if (cb::shot_camera("missing", p, t))
    return fail("unknown camera accepted");
  if (cb::scene_layout_manifest().find("camera_independent_geometry") ==
      std::string::npos)
    return fail("missing canonical scene manifest");
}
