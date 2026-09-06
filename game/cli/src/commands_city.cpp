#include "commands_city.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>

#include "city/materials.hpp"
#include "city/rng.hpp"
#include "city/showcase.hpp"
#include "city/site_build.hpp"
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

namespace inf::cli {

namespace {

// FNV-1a over centimetre-quantised geometry: the city generators are
// cosmetic float code (libm trig), so the golden tolerates last-bit
// differences but catches any change of shape, material or count.
struct Fnv {
  std::uint64_t h{0xcbf29ce484222325ULL};
  void feed(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xffU;
      h *= 0x100000001b3ULL;
    }
  }
  void feed_cm(float v) { feed(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(v * 100.0f)))); }
};

std::uint64_t hash_scene(const city::Scene& sc) {
  Fnv f;
  for (const city::Mesh* m : {&sc.opaque, &sc.foliage}) {
    f.feed(m->vertices.size());
    f.feed(m->indices.size());
    for (const city::Vertex& v : m->vertices) {
      f.feed_cm(v.position.x);
      f.feed_cm(v.position.y);
      f.feed_cm(v.position.z);
      f.feed(v.material);
    }
    for (std::size_t i = 0; i < m->indices.size(); i += 97) f.feed(m->indices[i]);
  }
  f.feed(sc.lights.size());
  f.feed(sc.draws.size());
  return f.h;
}

// The seed-83 home world's first Town through sites/v1 and the city
// system (T0021 WP3): the whole site at full detail.
int hash_home_town(const core::Seed128& seed) {
  const core::Key galaxy_key = gen::home_galaxy_key(seed);
  const gen::GalaxyParams galaxy = gen::home_galaxy_params(seed);
  const gen::CivilizationParams civ = gen::derive_civilization(galaxy_key, galaxy, true);
  gen::RaceRegistry registry(galaxy_key, galaxy, civ);
  registry.set_human(gen::human_race(galaxy_key, galaxy));
  const gen::ColonyResolver resolver(registry);
  const gen::SystemCell cell{};
  const core::WorldTime t = gen::kLaunchReference;
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, cell, true);
  const gen::Owner owner = resolver.owner(context, t);
  if (!owner.owned) {
    std::printf("city-site seed=%s: home system unowned\n", core::to_hex(seed).c_str());
    return 1;
  }
  const auto states = resolver.system_states(context, owner, t);
  int pick = -1;
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    if (states[i].is_home) pick = static_cast<int>(i);
  }
  if (pick < 0) {
    std::printf("city-site seed=%s: no home body\n", core::to_hex(seed).c_str());
    return 1;
  }
  const gen::BodyCivInputs& body = context.bodies[static_cast<std::size_t>(pick)];
  const gen::CivState& state = states[static_cast<std::size_t>(pick)];
  const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
  gen::HomeSlotOverride slot_override;
  const auto over = registry.home_override(cell);
  if (over.has_value()) {
    slot_override.habitat = over->habitat;
    slot_override.preferred_flux = over->preferred_flux;
    slot_override.force_biosphere = over->force_biosphere;
  }
  const gen::StarSystemParams system = gen::generate_system(context.system_key, over.has_value() ? &slot_override : nullptr);
  const gen::BodyKeys keys = gen::body_keys_in_system(context.system_key, body.slot);
  const gen::PlanetParams params = gen::planet_params_for_slot(system, body.slot, gen::BodyHandle{keys.entity, keys.params});
  gen::TerrainField field(keys.entity, params);
  const gen::SettlementPlanner planner(keys.entity, field, race.params, state.domed);
  const gen::SettlementPlan plan = planner.plan(state, race.factions);
  const gen::SiteField sites(keys.entity, field, plan, race.params, race.factions, state);
  const gen::CivilField civil(sites, field);
  field.set_height_modifier(&civil);
  const gen::Site* town = nullptr;
  for (const gen::Site& site : sites.sites()) {
    if (site.tier == static_cast<int>(gen::SettlementTier::Town)) {
      town = &site;
      break;
    }
  }
  if (town == nullptr) {
    std::printf("city-site seed=%s: no town\n", core::to_hex(seed).c_str());
    return 1;
  }
  city::Scene sc;
  city::SiteBuildStats stats;
  city::SiteBuildParams bp;
  bp.detail = 2;
  city::build_site_scene(sites, *town, field, bp, &sc, &stats);
  std::printf("city-site seed=%s province=%u tier=%s fnv=%016llx lots=%u towers=%u standards=%u triangles=%u\n",
              core::to_hex(seed).c_str(), town->province, gen::to_string(static_cast<gen::SettlementTier>(town->tier)),
              static_cast<unsigned long long>(hash_scene(sc)), stats.lots, stats.towers, stats.standards, stats.triangles);
  return 0;
}

}  // namespace

int cmd_hash_city() {
  const core::Seed128 seeds[2] = {core::Seed128{0, 1}, core::Seed128{0, 0x83}};
  for (const core::Seed128& seed : seeds) {
    city::Scene sc;
    sc.materials = city::make_materials();
    city::generate_showcase_small(sc, city::Rng(core::universe_key(seed)).child(0x51));
    std::printf("city-showcase seed=%s fnv=%016llx triangles=%zu\n", core::to_hex(seed).c_str(),
                static_cast<unsigned long long>(hash_scene(sc)),
                (sc.opaque.indices.size() + sc.foliage.indices.size()) / 3);
  }
  return hash_home_town(core::Seed128{0, 0x83});
}

}  // namespace inf::cli
