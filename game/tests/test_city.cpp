#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include "city/architecture.hpp"
#include "city/materials.hpp"
#include "city/rng.hpp"
#include "city/showcase.hpp"
#include "city/site_build.hpp"
#include "city/standards.hpp"
#include "city/towers.hpp"
#include "core/seed.hpp"
#include "gen/civ_time.hpp"
#include "gen/civil.hpp"
#include "gen/civilization.hpp"
#include "gen/colony.hpp"
#include "gen/human.hpp"
#include "gen/settlements.hpp"
#include "gen/sites.hpp"
#include "gen/terrain.hpp"
#include "gen/universe.hpp"

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

namespace {

// The seed-83 home world's site field (the CLI's hash-city builds the
// same): registry, resolver, the home body's terrain and its sites.
struct HomeSites {
  std::unique_ptr<gen::TerrainField> field;
  std::unique_ptr<gen::SiteField> sites;
  std::unique_ptr<gen::CivilField> civil;
};

HomeSites home_sites() {
  const core::Seed128 seed{0, 0x83};
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  const gen::ColonyResolver resolver(registry);
  const gen::SystemCell cell{};
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, cell, true);
  const gen::Owner owner = resolver.owner(context, gen::kLaunchReference);
  REQUIRE(owner.owned);
  const auto states = resolver.system_states(context, owner, gen::kLaunchReference);
  std::size_t pick = 0;
  for (std::size_t i = 0; i < states.size(); ++i) {
    if (states[i].is_home) pick = i;
  }
  const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
  gen::HomeSlotOverride slot_override;
  const auto over = registry.home_override(cell);
  if (over.has_value()) {
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
  }
  const gen::StarSystemParams system = gen::generate_system(context.system_key, over.has_value() ? &slot_override : nullptr);
  const gen::BodyKeys keys = gen::body_keys_in_system(context.system_key, context.bodies[pick].slot);
  const gen::PlanetParams params =
      gen::planet_params_for_slot(system, context.bodies[pick].slot, gen::BodyHandle{keys.entity, keys.params});
  HomeSites h;
  h.field = std::make_unique<gen::TerrainField>(keys.entity, params);
  const gen::SettlementPlanner planner(keys.entity, *h.field, race.params, states[pick].domed);
  const gen::SettlementPlan plan = planner.plan(states[pick], race.factions);
  h.sites = std::make_unique<gen::SiteField>(keys.entity, *h.field, plan, race.params, race.factions, states[pick]);
  h.civil = std::make_unique<gen::CivilField>(*h.sites, *h.field);
  h.field->set_height_modifier(h.civil.get());
  return h;
}

}  // namespace

TEST_CASE("city: the home town through sites/v1 and the architecture registry (T0021 WP3)") {
  const HomeSites h = home_sites();
  const gen::Site* town = nullptr;
  const gen::Site* capital = nullptr;
  for (const gen::Site& site : h.sites->sites()) {
    if (town == nullptr && site.tier == static_cast<int>(gen::SettlementTier::Town)) town = &site;
    if (site.capital) capital = &site;
  }
  REQUIRE(town != nullptr);
  // Every lot is a pure function of its key: two builds are identical.
  city::SiteBuildParams bp;
  bp.detail = 2;
  city::Scene a;
  city::Scene b;
  city::SiteBuildStats sa;
  city::SiteBuildStats sb;
  city::build_site_scene(*h.sites, *town, *h.field, bp, &a, &sa);
  city::build_site_scene(*h.sites, *town, *h.field, bp, &b, &sb);
  MESSAGE("home town: " << sa.lots << " lots, " << sa.towers << " towers, " << sa.standards << " standards, "
                        << sa.triangles << " triangles; max standard " << sa.max_standard_triangles
                        << ", max tower " << sa.max_tower_triangles);
  REQUIRE(a.opaque.vertices.size() == b.opaque.vertices.size());
  CHECK(a.opaque.indices == b.opaque.indices);
  bool same = true;
  for (std::size_t i = 0; i < a.opaque.vertices.size() && same; ++i) {
    same = a.opaque.vertices[i].position.x == b.opaque.vertices[i].position.x &&
           a.opaque.vertices[i].position.y == b.opaque.vertices[i].position.y &&
           a.opaque.vertices[i].position.z == b.opaque.vertices[i].position.z &&
           a.opaque.vertices[i].material == b.opaque.vertices[i].material;
  }
  CHECK(same);
  CHECK(sa.lots > 50);
  CHECK(sa.standards > 20);
  CHECK(sa.towers == 0);  // a town's height budgets stay under the tower threshold
  // Budgets: the full level carries the demo's bays and balconies; the
  // context levels are the cheap fabric (WP5 switches by distance).
  CHECK(sa.max_standard_triangles <= 3000);
  for (int detail = 1; detail >= 0; --detail) {
    city::SiteBuildParams cp = bp;
    cp.detail = detail;
    city::Scene c;
    city::SiteBuildStats sc;
    city::build_site_scene(*h.sites, *town, *h.field, cp, &c, &sc);
    MESSAGE("home town detail " << detail << ": " << sc.triangles << " triangles; max standard "
                                << sc.max_standard_triangles);
    CHECK(sc.max_standard_triangles <= (detail == 1 ? 2000u : 900u));
    CHECK(sc.lots == sa.lots);
  }
  for (const city::Vertex& v : a.opaque.vertices) {
    CHECK(std::isfinite(v.position.x + v.position.y + v.position.z));
    CHECK(v.material < a.materials.size());
  }
  // The architecture registry answers for every race and faction.
  for (int r = 0; r < static_cast<int>(gen::RaceType::Count); ++r) {
    for (int f = 0; f < static_cast<int>(gen::FactionType::Count); ++f) {
      gen::StyleVector style;
      style.race_type = static_cast<gen::RaceType>(r);
      style.faction_type = static_cast<gen::FactionType>(f);
      CHECK(city::architecture_for(style).name() != nullptr);
    }
  }
  // The capital: its civic centre (government building and ring) sits in
  // the old-town core; towers stand where the city rings' height budgets
  // rise, at the inner edge of ring 6.
  if (capital != nullptr) {
    city::SiteBuildParams cp;
    cp.detail = 0;
    cp.focus_radius_m = 400.0;
    city::Scene c;
    city::SiteBuildStats sc;
    city::build_site_scene(*h.sites, *capital, *h.field, cp, &c, &sc);
    MESSAGE("capital centre: " << sc.lots << " lots, " << sc.towers << " towers, " << sc.key_buildings
                               << " key buildings, " << sc.triangles << " triangles");
    CHECK(sc.key_buildings == 2);
    // Nothing built reaches outside the focus by more than a building.
    float far_x = 0.0f, far_y = 0.0f, far_z = 0.0f, far_d = 0.0f;
    std::uint32_t far_mat = 0;
    for (const city::Vertex& v : c.opaque.vertices) {
      const float d = std::sqrt(v.position.x * v.position.x + v.position.z * v.position.z);
      if (d > far_d) { far_d = d; far_x = v.position.x; far_y = v.position.y; far_z = v.position.z; far_mat = v.material; }
    }
    MESSAGE("capital centre: farthest vertex " << far_d << " m at (" << far_x << ", " << far_y << ", " << far_z
                                                << ") material " << far_mat);
    CHECK(far_d < 700.0f);
    city::SiteBuildParams rp;
    rp.detail = 0;
    rp.focus_x = gen::ring_radius_m(5) + 60.0;
    rp.focus_radius_m = 600.0;
    city::Scene ring;
    city::SiteBuildStats sr;
    city::build_site_scene(*h.sites, *capital, *h.field, rp, &ring, &sr);
    MESSAGE("capital ring 6: " << sr.lots << " lots, " << sr.towers << " towers, " << sr.triangles
                               << " triangles; max tower " << sr.max_tower_triangles);
    CHECK(sr.towers > 0);
    CHECK(sr.max_tower_triangles <= 60000);  // the coarse level of a tower
    CHECK(sr.dropped == 0);
    // Full detail on the same region: finite throughout.
    rp.detail = 2;
    city::Scene full;
    city::SiteBuildStats sf;
    city::build_site_scene(*h.sites, *capital, *h.field, rp, &full, &sf);
    MESSAGE("capital ring 6 full detail: " << sf.lots << " lots, " << sf.towers << " towers, " << sf.triangles
                                           << " triangles; max tower " << sf.max_tower_triangles << ", dropped "
                                           << sf.dropped);
    CHECK(sf.dropped == 0);
    // And the centre at full detail (the app's near level).
    cp.detail = 2;
    cp.focus_radius_m = 1200.0;
    city::Scene near;
    city::SiteBuildStats sn;
    city::build_site_scene(*h.sites, *capital, *h.field, cp, &near, &sn);
    MESSAGE("capital centre full detail: " << sn.lots << " lots, " << sn.towers << " towers, " << sn.triangles
                                           << " triangles, dropped " << sn.dropped);
    CHECK(sn.dropped == 0);
  }
  CHECK(sa.dropped == 0);
}
