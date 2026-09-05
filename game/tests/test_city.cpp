#include <doctest/doctest.h>

#include <cmath>

#include "city/materials.hpp"
#include "city/rng.hpp"
#include "city/showcase.hpp"
#include "city/standards.hpp"
#include "city/towers.hpp"
#include "core/seed.hpp"

using namespace inf;

TEST_CASE("city: generators are deterministic, finite and budgeted (T0021 WP0)") {
  const core::Key key = core::universe_key(core::Seed128{0, 0x83});
  // A tower twice from the same key is bit-identical; every vertex is
  // finite; the vertex layout is the 68-byte city format.
  city::Scene a;
  a.materials = city::make_materials();
  city::Scene b = a;
  city::build_tower(a, city::spec_diagrid(15.0f, 32), city::Vec2{0, 0}, 0.0f, city::Rng(key).child(1), 2);
  city::build_tower(b, city::spec_diagrid(15.0f, 32), city::Vec2{0, 0}, 0.0f, city::Rng(key).child(1), 2);
  REQUIRE(a.opaque.vertices.size() == b.opaque.vertices.size());
  CHECK(a.opaque.indices == b.opaque.indices);
  for (std::size_t i = 0; i < a.opaque.vertices.size(); ++i) {
    const city::Vertex& va = a.opaque.vertices[i];
    const city::Vertex& vb = b.opaque.vertices[i];
    CHECK(va.position.x == vb.position.x);
    CHECK(va.position.y == vb.position.y);
    CHECK(va.position.z == vb.position.z);
    CHECK(std::isfinite(va.position.x + va.position.y + va.position.z));
    CHECK(std::isfinite(va.normal.x + va.normal.y + va.normal.z));
    CHECK(va.material < a.materials.size());
  }
  CHECK(sizeof(city::Vertex) == 68);
  CHECK(a.opaque.triangle_count() > 1000);
  CHECK(a.opaque.triangle_count() < 250000);  // full detail; the WP5 LOD budget is 60k at detail 2
  // Standard buildings stay cheap.
  city::Scene s;
  s.materials = city::make_materials();
  city::Rng r(key);
  city::StandardSpec spec = city::random_standard(r, 600.0f, 0.5f);
  city::build_standard(s, spec, city::plan_rect(14.0f, 11.0f), 0.0f, r.child(2), 2);
  CHECK(s.opaque.triangle_count() > 50);
  CHECK(s.opaque.triangle_count() < 1500);
  // Every material's texture set has a name the library knows or "flat".
  for (const city::MaterialDesc& m : a.materials) {
    bool known = m.albedo_set.empty();
    for (const city::TextureSetSpec& t : city::texture_sets()) known = known || t.name == m.albedo_set;
    CAPTURE(m.name);
    CHECK(known);
  }
  // The showcase scene exists and is bounded.
  city::Scene show;
  show.materials = city::make_materials();
  city::generate_showcase_small(show, city::Rng(key).child(0x51));
  CHECK(show.opaque.triangle_count() > 50000);
  CHECK(show.opaque.triangle_count() < 800000);
  CHECK(!show.lights.empty());
}
