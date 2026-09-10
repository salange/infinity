// Headless validation of the authored set and renderer-facing scene contract.
#include "scene.hpp"
#include "city_routes.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace {
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
  std::uint64_t canonical_hash = 0;
  {
    const cb::Scene sc = cb::generate_scene(params);
    if (sc.opaque.vertices.empty())
      return fail("empty city");
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
  // The approved terrace composition uses a narrower lens; the interactive
  // alias and the physical access route must arrive at that same camera.
  cb::Vec3 landing_p, landing_t, terrace_p, terrace_t;
  cb::shot_camera("landing", landing_p, landing_t);
  cb::shot_camera("terrace", terrace_p, terrace_t);
  if (std::abs(cb::shot_fov_degrees("landing") - 48.f) > .001f ||
      cb::shot_fov_degrees("terrace") != cb::shot_fov_degrees("landing") ||
      cb::length(landing_p - terrace_p) > .001f ||
      cb::length(landing_t - terrace_t) > .001f)
    return fail("landing camera and terrace alias disagree");
  for (const auto& route : cb::scene_routes())
    if (route.id == "landing_access" &&
        (route.waypoints.empty() ||
         cb::length(route.waypoints.back().position - landing_p) > .001f ||
         cb::length(route.waypoints.back().target - landing_t) > .001f))
      return fail("landing route no longer reaches its canonical camera");
  cb::Vec3 p, t;
  if (cb::shot_camera("missing", p, t))
    return fail("unknown camera accepted");
  if (cb::scene_layout_manifest().find("camera_independent_geometry") ==
      std::string::npos)
    return fail("missing canonical scene manifest");
}
