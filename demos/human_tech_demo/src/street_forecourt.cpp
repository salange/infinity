#include "street_forecourt.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace cb {
namespace {
constexpr float kFloor = 1.2f;
const Vec2 kCentre{-111, 137};
constexpr float kSoil = 1.43f;
using Material = std::uint32_t;

Material named(const Scene &scene, std::string_view name) {
  for (std::size_t i = scene.materials.size(); i > 0; --i)
    if (scene.materials[i - 1].name == name)
      return static_cast<Material>(i - 1);
  throw std::runtime_error("Street forecourt requires material: " +
                           std::string(name));
}

bool within_clear_area(Vec3 p) {
  return p.x >= -114.4f && p.x <= -103.f && p.z >= 133.f && p.z <= 145.f;
}

// The camera route is west of the bed. Check the complete transformed botanical
// geometry so a broad leaf cannot intrude despite an apparently safe root.
bool supported_plant(const Scene &scene, const AssetInstance &instance) {
  const auto &mesh = scene.asset_library.resources[instance.resource].mesh;
  for (const auto &vertex : mesh.vertices)
    if (!within_clear_area(asset_transform_point(instance, vertex.position)))
      return false;
  return true;
}
std::vector<Vec2> intersect(const std::vector<Vec2> &a,
                            const std::vector<Vec2> &b) {
  if (a.size() < 3 || b.size() < 3)
    return {};
  const float orientation = plan_area(b) > 0 ? 1.f : -1.f;
  auto result = a;
  for (std::size_t i = 0; i < b.size() && !result.empty(); ++i) {
    const Vec2 p = b[i], q = b[(i + 1) % b.size()], d = q - p;
    result = clip_halfplane(result, p, {-d.y * orientation, d.x * orientation});
  }
  return result;
}

std::vector<std::vector<Vec2>> subtract(const std::vector<Vec2> &shape,
                                        const std::vector<Vec2> &opening) {
  std::vector<std::vector<Vec2>> pieces;
  if (opening.size() < 3)
    return {shape};
  const float orientation = plan_area(opening) > 0 ? 1.f : -1.f;
  auto inside = shape;
  for (std::size_t edge = 0; edge < opening.size() && !inside.empty(); ++edge) {
    const auto p = opening[edge], q = opening[(edge + 1) % opening.size()],
               d = q - p;
    const Vec2 normal{-d.y * orientation, d.x * orientation};
    auto outside = clip_halfplane(inside, p, normal * -1.f);
    if (outside.size() > 2 && std::abs(plan_area(outside)) > .005f)
      pieces.push_back(std::move(outside));
    inside = clip_halfplane(inside, p, normal);
  }
  return pieces;
}

void paving(Scene &scene, Material stone, Rng rng) {
  const std::vector<Vec2> promenade{
      {-121, 163}, {-84, 163}, {-35, 84}, {-111, 84}};
  const auto patch =
      intersect(plan_rect(14.55f, 20, {-106.55f, 140}), promenade);
  // Bonded topping on the existing1.2m slab;18mm joints remain below the
  // adjacent low-point puddles at1.222m. The real drain grate stays exposed.
  MaterialDesc joint = scene.materials[stone];
  joint.name = "market stone open joints";
  joint.base_color = {.035f, .038f, .034f};
  joint.albedo_set = "";
  joint.roughness = .85f;
  joint.flags = kMatPlanarXZ;
  const auto grout = static_cast<Material>(scene.materials.size());
  scene.materials.push_back(joint);
  const auto drain = plan_rect(.4f, .85f, {-106.5f, 131});
  for (const auto &piece : subtract(patch, drain))
    Emit(&scene.opaque, grout).polygon(piece, 1.2005f, true);
  constexpr float top = 1.218f;
  for (int row = 0; row < 26; ++row) {
    const float z = 120 + row * 1.6f;
    for (int col = 0; col < 16; ++col) {
      const float x = -123 + col * 2.25f + (row % 3) * .75f;
      auto tile = intersect(plan_rect(1.120f, .795f, {x, z}), patch);
      if (tile.size() < 3 || std::abs(plan_area(tile)) < .018f)
        continue;
      for (const auto &piece : subtract(tile, drain)) {
        Emit slab(&scene.opaque, stone);
        slab.element_random = rng.next();
        slab.polygon(piece, top, true);
        slab.wall(piece, kFloor, top, true);
      }
    }
  }
}
} // namespace

std::uint32_t market_paving_material(Scene &scene) {
  constexpr const char *name = "exposed market honed stone";
  for (std::uint32_t i = 0; i < scene.materials.size(); ++i)
    if (scene.materials[i].name == name)
      return i;
  MaterialDesc m = scene.materials[M_MARBLE_WHITE];
  m.name = name;
  m.base_color = scene.reviewed_material_maps ? Vec3{.48f, .48f, .43f}
                                              : Vec3{.30f, .30f, .27f};
  m.roughness = .72f;
  m.normal_strength = .30f;
  m.metallic = 0;
  m.uv_scale = 3.6f;
  m.flags = kMatPlanarXZ | 512u | 2048u;
  scene.materials.push_back(m);
  return static_cast<Material>(scene.materials.size() - 1);
}

void stage_street_forecourt(Scene &scene, Rng rng) {
  if (scene.asset_library.resources.empty())
    return;
  paving(scene, market_paving_material(scene), rng.child(90));
  const Material stone = named(scene, "stone_warm");
  const Material soil = named(scene, "soil_mulch");
  const Material bark = named(scene, "bark_ridged");
  const Material bronze = named(scene, "bronze_satin");
  const Material dark = named(scene, "gasket_charcoal");
  MaterialDesc light;
  light.name = "market planter recessed warm light";
  light.base_color = {1, .73f, .38f};
  light.tint2 = light.base_color;
  light.flags = kMatEmissive;
  light.emissive = 1.7f;
  light.roughness = .5f;
  scene.materials.push_back(light);
  const auto lamp = static_cast<Material>(scene.materials.size() - 1);

  const auto outer = plan_superellipse(3.f, 3.f, 2.6f, 80, kCentre);
  const auto shoulder = plan_superellipse(2.98f, 2.98f, 2.6f, 80, kCentre);
  const auto inner = plan_superellipse(2.79f, 2.79f, 2.6f, 80, kCentre);
  Emit coping(&scene.opaque, stone);
  coping.wall(outer, kFloor, 1.51f, true);
  coping.ring_cap(outer, shoulder, 1.51f);
  // The bevel is a real sloped face; the inner return contains visible soil.
  for (std::size_t i = 0; i < outer.size(); ++i) {
    const auto j = (i + 1) % outer.size();
    const Vec3 a{shoulder[i].x, 1.51f, shoulder[i].y};
    const Vec3 b{shoulder[j].x, 1.51f, shoulder[j].y};
    const Vec3 c{inner[j].x, 1.49f, inner[j].y};
    const Vec3 d{inner[i].x, 1.49f, inner[i].y};
    coping.quad_metric(a, d, c, b);
  }
  coping.wall(inner, kSoil - .05f, 1.49f, true, false);
  Emit(&scene.opaque, soil).polygon(inner, kSoil, true);

  // Fine aggregate, old leaf litter and irrigation remain on the soil surface.
  Emit mulch(&scene.opaque, bark), hose(&scene.opaque, dark);
  const auto root_area = plan_superellipse(2.56f, 2.56f, 2.6f, 80, kCentre);
  for (int i = 0; i < 550; ++i) {
    Vec2 p{kCentre.x + rng.range(-2.7f, 2.7f),
           kCentre.y + rng.range(-2.7f, 2.7f)};
    if (!point_in_polygon(inner, p))
      continue;
    float a = rng.range(-kPi, kPi);
    Vec3 x{std::cos(a), 0, std::sin(a)}, z{-x.z, 0, x.x};
    mulch.box({p.x, kSoil + .011f, p.y},
              {rng.range(.015f, .07f), .010f, rng.range(.009f, .026f)}, x,
              {0, 1, 0}, z);
  }
  const auto irrigation = plan_superellipse(2.65f, 2.65f, 2.6f, 64, kCentre);
  for (std::size_t i = 0; i < irrigation.size(); ++i) {
    const auto a = irrigation[i], b = irrigation[(i + 1) % irrigation.size()];
    hose.tube({a.x, kSoil + .016f, a.y}, {b.x, kSoil + .016f, b.y}, .013f, 6,
              true);
  }

  std::vector<Vec2> roots;
  for (int attempt = 0; attempt < 300 && roots.size() < 57; ++attempt) {
    Vec2 p{kCentre.x + rng.range(-2.48f, 2.48f),
           kCentre.y + rng.range(-2.48f, 2.48f)};
    if (!point_in_polygon(root_area, p))
      continue;
    if (std::any_of(roots.begin(), roots.end(),
                    [&](Vec2 q) { return length(p - q) < .43f; }))
      continue;
    const int kind = static_cast<int>(roots.size()) % 7;
    const char *name = kind < 3    ? "fern_arching"
                       : kind == 3 ? "phormium"
                       : kind == 4 ? "shrub_flowering"
                                   : "groundcover";
    const float scale = kind < 3    ? rng.range(.58f, .79f)
                        : kind == 3 ? .55f
                        : kind == 4 ? .39f
                                    : rng.range(.60f, .88f);
    AssetInstance instance{asset_resource(scene.asset_library, name),
                           {p.x, kSoil, p.y},
                           rng.range(-kPi, kPi),
                           {scale, scale, scale},
                           {1, 1, 1}};
    if (!supported_plant(scene, instance))
      continue;
    scene.asset_instances.push_back(instance);
    roots.push_back(p);
  }

  // Small ground-mounted luminaires graze the leaves without raising a new
  // foreground obstruction. Their cables terminate at the retained soil wall.
  for (Vec2 at : {Vec2{-113.15f, 136.1f}, Vec2{-109.7f, 139.1f}}) {
    Emit metal(&scene.opaque, bronze);
    metal.tube({at.x, kSoil, at.y}, {at.x, kSoil + .09f, at.y}, .12f, 24, true);
    metal.torus({at.x, kSoil + .093f, at.y}, {0, 1, 0}, .107f, .012f, 24, 8);
    Emit(&scene.opaque, lamp)
        .tube({at.x, kSoil + .094f, at.y}, {at.x, kSoil + .097f, at.y}, .092f,
              24, true);
    scene.lights.push_back(
        {{at.x, kSoil + .15f, at.y}, 3.2f, {1, .68f, .34f}, 1.3f});
  }
}
} // namespace cb
