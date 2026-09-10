#include "tower_floor_identity.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace inf::city;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
auto fields(const MaterialDesc &m) {
  return std::tie(m.name, m.base_color.x, m.base_color.y, m.base_color.z,
                  m.roughness, m.metallic, m.emissive, m.normal_strength,
                  m.albedo_set, m.uv_scale, m.flags, m.tint2.x, m.tint2.y,
                  m.tint2.z, m.room_w, m.room_h, m.room_d, m.lit_probability);
}
void palette_unchanged(const Scene &before, const Scene &after) {
  require(after.materials.size() >= before.materials.size(), "palette shrank");
  for (std::size_t i = 0; i < before.materials.size(); ++i)
    require(fields(before.materials[i]) == fields(after.materials[i]),
            "original material record changed");
}
std::size_t compare_geometry(const Scene &before, const Scene &after,
                             const cb::TowerFloorSpan &span, float datum) {
  require(before.opaque.indices == after.opaque.indices &&
              before.opaque.vertices.size() == after.opaque.vertices.size(),
          "tower index order, count or topology changed");
  require(!std::memcmp(&before.opaque.bounds_min, &after.opaque.bounds_min,
                       sizeof(Vec3)) &&
              !std::memcmp(&before.opaque.bounds_max, &after.opaque.bounds_max,
                           sizeof(Vec3)),
          "physical tower bounds changed");
  require(before.foliage.indices == after.foliage.indices &&
              before.foliage.vertices.size() == after.foliage.vertices.size() &&
              before.lights.size() == after.lights.size(),
          "non-shaft scene contents changed");
  std::size_t adjusted = 0;
  for (std::size_t i = 0; i < before.opaque.vertices.size(); ++i) {
    auto expected = before.opaque.vertices[i];
    const auto &actual = after.opaque.vertices[i];
    if (i >= span.first_vertex && i < span.end_vertex &&
        actual.material == span.shaft && span.original != span.shaft) {
      require(expected.material == span.original, "non-shaft vertex remapped");
      expected.material = span.shaft;
      expected.aux.y -= datum;
      require(std::abs(actual.aux.y - (actual.position.y - datum)) < .0001f,
              "shaft coordinates do not use its physical base datum");
      ++adjusted;
    }
    // Includes all positions, normals, tangent handedness, UV pattern inputs,
    // element seeds and occlusion. Only the two stated fields may differ.
    require(!std::memcmp(&expected, &actual, sizeof(Vertex)),
            "geometry, optics reference, pattern parameters or seed changed");
  }
  require(adjusted == span.adjusted_vertices,
          "incorrect adjusted vertex count");
  return adjusted;
}
} // namespace

int main() {
  try {
    std::size_t cases = 0, shaft_vertices = 0, integer_boundaries = 0;
    set_far_patterns(true);
    for (auto glass : {M_GLASS_BLUE, M_GLASS_STD})
      for (float height : {4.f, 4.7f})
        for (float datum : {1.2f, -2.3f})
          for (int detail : {-1, 0, 2})
            for (auto facade : {FacadeKind::Curtain, FacadeKind::Diagrid})
              for (auto base : {BaseKind::Lobby, BaseKind::Podium}) {
                Scene before, after;
                before.materials = make_materials();
                before.materials[glass].base_color = {.62f, .76f, .82f};
                before.materials[glass].metallic = .35f;
                before.materials[glass].lit_probability = .19f;
                // Existing vertices and IDs must remain unchanged.
                Emit(&before.opaque, M_GLASS_CLEAR)
                    .box({-80, 2, -80}, {1, 1, 1});
                after = before;
                const auto original_palette = before;
                cb::TowerFloorMaterials materials;
                TowerSpec spec;
                spec.plan = PlanKind::Circle;
                spec.a = spec.b = 8;
                spec.floors = 6;
                spec.floor_h = height;
                spec.glass = glass;
                spec.base = base;
                spec.facade = facade;
                spec.crown = CrownKind::Lantern;
                spec.random = .413f;
                build_tower(before, spec, {11, -17}, datum, root_rng("83"),
                            detail);
                const auto span = cb::build_floor_aligned_tower(
                    after, materials, spec, {11, -17}, datum, root_rng("83"),
                    detail);
                require(span.original == glass && span.shaft != glass &&
                            span.shaft == original_palette.materials.size(),
                        "shaft copy did not preserve existing material IDs");
                palette_unchanged(original_palette, after);
                auto optical_reference = original_palette.materials[glass];
                optical_reference.room_h = height;
                optical_reference.name = after.materials[span.shaft].name;
                require(fields(optical_reference) ==
                            fields(after.materials[span.shaft]),
                        "shaft copy changed optical fields or occupancy");
                shaft_vertices += compare_geometry(before, after, span, datum);
                require(span.adjusted_vertices > 0,
                        "actual tower shaft was not exercised");
                for (std::size_t i = span.first_vertex; i < span.end_vertex;
                     ++i) {
                  const auto &v = after.opaque.vertices[i];
                  if (v.material != span.shaft)
                    continue;
                  const float floor = (v.position.y - datum) / height;
                  // All generated pane corners here lie on physical storey
                  // boundaries because the fixture has no inset spandrels.
                  require(std::abs(floor - std::round(floor)) < .0001f &&
                              std::abs(v.aux.y / height - std::round(floor)) <
                                  .0001f,
                          "window row boundary differs from the physical slab");
                  ++integer_boundaries;
                }
                const auto count = after.materials.size();
                require(materials.material(after, glass, height) ==
                                span.shaft &&
                            after.materials.size() == count,
                        "identical shaft definition was not cached locally");
                const auto other =
                    materials.material(after, glass, height + .25f);
                require(other != span.shaft &&
                            after.materials[other].room_h == height + .25f,
                        "different physical storeys shared a room period");
                ++cases;
              }
    // Shared-ID collisions are why even matching periods need separate shaft
    // IDs: CLEAR lobbies and SILVER lantern crowns must keep their own UVs.
    for (auto glass : {M_GLASS_CLEAR, M_GLASS_SILVER}) {
      Scene before, after;
      before.materials = make_materials();
      after = before;
      cb::TowerFloorMaterials materials;
      TowerSpec spec;
      spec.floors = 4;
      spec.glass = glass;
      spec.crown = CrownKind::Lantern;
      build_tower(before, spec, {}, 1.2f, root_rng("91"), 2);
      const auto span = cb::build_floor_aligned_tower(
          after, materials, spec, {}, 1.2f, root_rng("91"), 2);
      require(compare_geometry(before, after, span, 1.2f) > 0,
              "collision fixture has no shaft");
      std::size_t retained = 0;
      for (auto &v : after.opaque.vertices)
        retained += v.material == glass;
      require(retained > 0,
              "lobby/crown was incorrectly remapped with the shaft");
    }
    // Optical flag128 fields are IOR/transmission, never room dimensions.
    for (auto flags : {128u, 129u, 0u}) {
      Scene before, after;
      before.materials = make_materials();
      before.materials[M_GLASS_BLUE].flags = flags;
      after = before;
      cb::TowerFloorMaterials materials;
      TowerSpec spec;
      spec.floors = 3;
      build_tower(before, spec, {}, 1.2f, root_rng("12"), 0);
      const auto span = cb::build_floor_aligned_tower(
          after, materials, spec, {}, 1.2f, root_rng("12"), 0);
      require(span.shaft == span.original && !span.adjusted_vertices &&
                  before.materials.size() == after.materials.size(),
              "true glass or non-room material was assigned a room period");
      compare_geometry(before, after, span, 1.2f);
      palette_unchanged(before, after);
    }
    {
      Scene scene;
      scene.materials = make_materials();
      cb::TowerFloorMaterials materials;
      const auto first = materials.material(scene, M_GLASS_STD, 4.f);
      scene.materials[M_GLASS_STD].roughness = .123f;
      const auto updated = materials.material(scene, M_GLASS_STD, 4.f);
      require(first != updated && scene.materials[updated].roughness == .123f &&
                  scene.materials[first].roughness != .123f,
              "cache silently reused a different optical material definition");
      for (float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
        bool rejected = false;
        try {
          materials.material(scene, M_GLASS_BLUE, invalid);
        } catch (const std::invalid_argument &) {
          rejected = true;
        }
        require(rejected, "invalid room period was accepted");
      }
      bool rejected = false;
      try {
        materials.material(scene, static_cast<Mat>(scene.materials.size()),
                           4.f);
      } catch (const std::out_of_range &) {
        rejected = true;
      }
      require(rejected, "invalid source material was accepted");
    }
    std::cout << "PASS " << cases << " actual tower controls, "
              << shaft_vertices << " shaft vertices and " << integer_boundaries
              << " physical floor boundaries; BLUE/STD, raised datums, "
                 "detailed/analytic/far paths, "
                 "exact geometry and seeds, local cache, lobby/crown and "
                 "optical-field controls\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
