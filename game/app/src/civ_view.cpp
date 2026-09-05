#include "civ_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace inf::app {

namespace {

// Tile streaming ranges (metres from the player to a tile centre).
constexpr double kEcumFarRangeM = 150000.0;     // far tiles drawn out to here
constexpr double kEcumFarSplitM = 20000.0;      // far -> mid inside this
constexpr double kEcumMidSplitM = 6000.0;       // mid -> near inside this
constexpr double kEcumNearPartsM = 1300.0;      // near detail 0 inside this
constexpr double kEcumNearGrammarM = 4000.0;    // near detail 1 inside this
constexpr double kEcumBakeOnlyAltitudeM = 400000.0;  // above this the bake carries the city
constexpr double kTileBuildBudgetMs = 12.0;  // per frame; at least one tile

void push_item(const CivAnchor::TileEntry& entry, const render::Vec3& camera_pos,
               const render::Mat4& view_projection, std::vector<render::Rhi::DrawItem>* items) {
  render::Rhi::DrawItem item;
  item.mesh = entry.mesh;
  const render::Vec3 origin{entry.origin[0], entry.origin[1], entry.origin[2]};
  const render::Vec3 translation = origin - camera_pos;
  const render::Mat4 model = render::translate(translation);
  const render::Mat4 mvp = render::mul(view_projection, model);
  std::memcpy(item.mvp, mvp.m, sizeof(mvp.m));
  item.aux[0] = static_cast<float>(translation.x);
  item.aux[1] = static_cast<float>(translation.y);
  item.aux[2] = static_cast<float>(translation.z);
  constexpr double kTilePeriod = 256.0;
  item.extra[0] = static_cast<float>(std::fmod(entry.origin[0], kTilePeriod));
  item.extra[1] = static_cast<float>(std::fmod(entry.origin[1], kTilePeriod));
  item.extra[2] = static_cast<float>(std::fmod(entry.origin[2], kTilePeriod));
  std::memcpy(item.material_palette, entry.palette, sizeof(item.material_palette));
  items->push_back(item);
}

}  // namespace

std::unique_ptr<CivAnchor> build_civ_anchor(const core::Seed128& seed,
                                            const gen::RaceRegistry& registry,
                                            const gen::ColonyResolver& resolver,
                                            const gen::SystemCell& cell, int slot, int moon,
                                            const core::Key& body_entity,
                                            gen::TerrainField* field, core::WorldTime now) {
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, cell, true);
  const gen::Owner owner = resolver.owner(context, now);
  if (!owner.owned) {
    return nullptr;
  }
  const auto states = resolver.system_states(context, owner, now);
  int pick = -1;
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    if (context.bodies[i].slot == slot && context.bodies[i].moon == moon) {
      pick = static_cast<int>(i);
    }
  }
  if (pick < 0 || !states[static_cast<std::size_t>(pick)].settled) {
    return nullptr;
  }
  (void)body_entity;
  auto civ = std::make_unique<CivAnchor>();
  civ->settled = true;
  civ->state = states[static_cast<std::size_t>(pick)];
  const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
  civ->race = race.params;
  civ->factions = race.factions;
  const core::Key& entity = context.bodies[static_cast<std::size_t>(pick)].body_entity;
  const gen::SettlementPlanner planner(entity, *field, civ->race, civ->state.domed);
  civ->plan = std::make_unique<gen::SettlementPlan>(planner.plan(civ->state, civ->factions));
  if (civ->state.level >= 7) {
    // ecumenopolis/v1: the plate modifier and the block lattice; no site
    // list at all.
    civ->ecumenopolis = std::make_unique<gen::EcumenopolisField>(entity, *field, *civ->plan, civ->race,
                                                                 civ->factions, civ->state);
    field->set_height_modifier(civ->ecumenopolis.get());
    std::printf("civ: %s L7 %s%s — ecumenopolis: blocks of %.0f m (level %d, %u per face), plates %.0f..%.0f m\n",
                race.params.name.c_str(), gen::to_string(static_cast<gen::DevLevel>(civ->state.level)),
                civ->state.ruined ? " (ruined)" : "", civ->ecumenopolis->block_m(),
                civ->ecumenopolis->block_level(), civ->ecumenopolis->blocks_per_face(),
                civ->ecumenopolis->plate_min_m(), civ->ecumenopolis->plate_max_m());
    return civ;
  }
  civ->sites = std::make_unique<gen::SiteField>(entity, *field, *civ->plan, civ->race, civ->factions,
                                                civ->state);
  civ->civil = std::make_unique<gen::CivilField>(*civ->sites, *field);
  field->set_height_modifier(civ->civil.get());
  int per_tier[9] = {};
  for (const gen::Site& site : civ->sites->sites()) ++per_tier[site.tier];
  std::printf("civ: %s L%d %s — %zu sites (", race.params.name.c_str(), civ->state.level,
              gen::to_string(static_cast<gen::DevLevel>(civ->state.level)),
              civ->sites->sites().size());
  for (int k = 1; k <= 7; ++k) {
    if (per_tier[k] > 0) std::printf(" %s %d", gen::to_string(static_cast<gen::SettlementTier>(k)), per_tier[k]);
  }
  std::printf(" ), %zu roads\n", civ->plan->roads.size());
  return civ;
}

std::vector<CivBodyInputs> gather_civ_bodies(const core::Seed128& seed,
                                             const gen::RaceRegistry& registry,
                                             const gen::ColonyResolver& resolver,
                                             const gen::SystemCell& cell, core::WorldTime now) {
  std::vector<CivBodyInputs> out;
  const gen::SystemCivContext context = gen::gather_system_context(seed, registry, cell, true);
  const gen::Owner owner = resolver.owner(context, now);
  if (!owner.owned) {
    return out;
  }
  const auto states = resolver.system_states(context, owner, now);
  const gen::Race& race = resolver.candidates(context.position_m)[owner.candidate];
  for (std::size_t i = 0; i < context.bodies.size(); ++i) {
    if (!states[i].settled) continue;
    CivBodyInputs inputs;
    inputs.slot = context.bodies[i].slot;
    inputs.moon = context.bodies[i].moon;
    inputs.state = states[i];
    inputs.race = race.params;
    inputs.factions = race.factions;
    out.push_back(std::move(inputs));
  }
  return out;
}

std::unique_ptr<CivModifier> build_civ_modifier(const core::Key& body_entity, gen::TerrainField* field,
                                                const CivBodyInputs& inputs) {
  auto out = std::make_unique<CivModifier>();
  const gen::SettlementPlanner planner(body_entity, *field, inputs.race, inputs.state.domed);
  out->plan = std::make_unique<gen::SettlementPlan>(planner.plan(inputs.state, inputs.factions));
  if (inputs.state.level >= 7) {
    out->ecumenopolis = std::make_unique<gen::EcumenopolisField>(body_entity, *field, *out->plan, inputs.race,
                                                                 inputs.factions, inputs.state);
    field->set_height_modifier(out->ecumenopolis.get());
    return out;
  }
  out->sites = std::make_unique<gen::SiteField>(body_entity, *field, *out->plan, inputs.race, inputs.factions,
                                                inputs.state);
  out->civil = std::make_unique<gen::CivilField>(*out->sites, *field);
  field->set_height_modifier(out->civil.get());
  return out;
}

void release_civ_meshes(CivAnchor* civ, render::Rhi* rhi) {
  if (civ == nullptr) return;
  for (auto& entry : civ->meshes) {
    if (entry.mesh != 0) {
      rhi->destroy_mesh(entry.mesh);
      entry.mesh = 0;
    }
  }
  civ->meshes.clear();
  for (auto& tile : civ->tiles) {
    if (tile.mesh != 0) {
      rhi->destroy_mesh(tile.mesh);
      tile.mesh = 0;
    }
  }
  civ->tiles.clear();
}

namespace {

// The ecumenopolis streamer: a quadtree selection over the tile levels
// from the player's position (far tiles split into mid tiles inside
// kEcumFarSplitM, mid tiles into near tiles inside kEcumMidSplitM; near
// detail by distance), then a bounded number of rebuilds per frame and
// eviction of everything not selected.
void draw_ecumenopolis(CivAnchor* civ, render::Rhi* rhi, const gen::TerrainField& field,
                       const render::Vec3& player_pos, const render::Vec3& camera_pos,
                       const render::Mat4& view_projection,
                       std::vector<render::Rhi::DrawItem>* items) {
  const gen::EcumenopolisField& ecum = *civ->ecumenopolis;
  const double R = field.planet().radius_m.to_double();
  const double player_len = std::sqrt(player_pos.x * player_pos.x + player_pos.y * player_pos.y + player_pos.z * player_pos.z);
  const double altitude = player_len - R;
  for (auto& tile : civ->tiles) tile.wanted = false;
  if (altitude < kEcumBakeOnlyAltitudeM) {
    const gen::Dir3 player_dir{det::Real(player_pos.x / player_len), det::Real(player_pos.y / player_len), det::Real(player_pos.z / player_len)};
    gen::SiteFrame frame;
    frame.up = player_dir;
    gen::tangent_basis(frame.up, &frame.east, &frame.north);
    frame.radius_m = R;
    const auto dist_to = [&](const gen::Dir3& d) {
      const double chord = std::sqrt(gen::chord_sq(player_dir, d).to_double()) * R;
      return std::sqrt(chord * chord + altitude * altitude);
    };
    // Probe directions on a polar grid: every far tile within range is
    // hit by at least one probe (spacing half a far tile).
    const double far_m = ecum.tile_m(gen::EcumenopolisField::kAvenueShift);
    const double range = std::min(kEcumFarRangeM, 0.9 * R);
    std::vector<gen::EcumenopolisField::TileId> far_tiles;
    const auto add_far = [&](const gen::EcumenopolisField::TileId& id) {
      for (const auto& t : far_tiles) if (t == id) return;
      far_tiles.push_back(id);
    };
    add_far(ecum.tile_of(player_dir, gen::EcumenopolisField::kAvenueShift));
    for (double r = 0.5 * far_m; r <= range; r += 0.5 * far_m) {
      const int steps = std::max(8, static_cast<int>(2.0 * 3.14159265 * r / (0.5 * far_m)) + 1);
      for (int k = 0; k < steps; ++k) {
        const double a = 2.0 * 3.14159265 * k / steps;
        add_far(ecum.tile_of(frame.to_dir(r * std::cos(a), r * std::sin(a)), gen::EcumenopolisField::kAvenueShift));
      }
    }
    // Selection.
    std::vector<std::pair<gen::EcumenopolisField::TileId, int>> wanted;
    const auto children = [&](const gen::EcumenopolisField::TileId& id, int shift) {
      std::vector<gen::EcumenopolisField::TileId> out;
      const std::uint32_t k = 1U << (id.shift - shift);
      for (std::uint32_t j = 0; j < k; ++j) {
        for (std::uint32_t i = 0; i < k; ++i) {
          out.push_back(gen::EcumenopolisField::TileId{shift, id.face, id.ti * k + i, id.tj * k + j});
        }
      }
      return out;
    };
    for (const auto& far : far_tiles) {
      const double d_far = dist_to(ecum.tile_centre(far));
      if (d_far > range + 0.7 * far_m) continue;
      if (d_far > kEcumFarSplitM + 0.7 * far_m) {
        wanted.emplace_back(far, 3);
        continue;
      }
      for (const auto& mid : children(far, 5)) {
        const double d_mid = dist_to(ecum.tile_centre(mid));
        if (d_mid > kEcumMidSplitM + 0.7 * ecum.tile_m(5)) {
          wanted.emplace_back(mid, 2);
          continue;
        }
        for (const auto& near : children(mid, 3)) {
          const double d = dist_to(ecum.tile_centre(near));
          wanted.emplace_back(near, d < kEcumNearPartsM ? 0 : (d < kEcumNearGrammarM ? 1 : 2));
        }
      }
    }
    // Mark, build (bounded by a time budget), evict.
    int built = 0;
    const auto frame_start = std::chrono::steady_clock::now();
    for (const auto& want : wanted) {
      CivAnchor::TileEntry* entry = nullptr;
      for (auto& tile : civ->tiles) {
        if (tile.id == want.first) { entry = &tile; break; }
      }
      if (entry == nullptr) {
        civ->tiles.push_back(CivAnchor::TileEntry{});
        entry = &civ->tiles.back();
        entry->id = want.first;
        entry->detail = -1;
      }
      entry->wanted = true;
      if (entry->mesh != 0 && entry->detail == want.second) continue;
      if (entry->mesh == 0 || entry->detail != want.second) {
        if (built > 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count() >
                kTileBuildBudgetMs) {
          continue;
        }
        ++built;
        const gen::EcumenopolisMesh mesh = gen::build_ecumenopolis_tile(ecum, want.first, want.second, civ->method);
        if (entry->mesh != 0) {
          rhi->destroy_mesh(entry->mesh);
          entry->mesh = 0;
        }
        if (!mesh.mesh.vertices.empty()) {
          entry->mesh = rhi->create_mesh_mat(mesh.mesh.vertices.data(), mesh.mesh.vertices.size());
        }
        entry->detail = want.second;
        std::memcpy(entry->origin, mesh.mesh.origin, sizeof(entry->origin));
        std::memcpy(entry->palette, mesh.mesh.palette, sizeof(entry->palette));
      }
    }
  }
  for (std::size_t i = 0; i < civ->tiles.size();) {
    CivAnchor::TileEntry& tile = civ->tiles[i];
    if (!tile.wanted) {
      if (tile.mesh != 0) rhi->destroy_mesh(tile.mesh);
      tile = civ->tiles.back();
      civ->tiles.pop_back();
      continue;
    }
    if (tile.mesh != 0) push_item(tile, camera_pos, view_projection, items);
    ++i;
  }
}

}  // namespace

void draw_civ_sites(CivAnchor* civ, render::Rhi* rhi, const gen::TerrainField& field,
                    const render::Vec3& player_pos, const render::Vec3& camera_pos,
                    const render::Mat4& view_projection,
                    std::vector<render::Rhi::DrawItem>* items) {
  if (civ == nullptr) return;
  if (civ->ecumenopolis != nullptr) {
    draw_ecumenopolis(civ, rhi, field, player_pos, camera_pos, view_projection, items);
    return;
  }
  if (civ->sites == nullptr) return;
  const auto& sites = civ->sites->sites();
  if (civ->meshes.size() != sites.size()) {
    release_civ_meshes(civ, rhi);
    civ->meshes.resize(sites.size());
    for (std::size_t i = 0; i < sites.size(); ++i) civ->meshes[i].site_index = static_cast<std::uint32_t>(i);
  }
  const double R = field.planet().radius_m.to_double();
  const double player_len = std::sqrt(player_pos.x * player_pos.x + player_pos.y * player_pos.y + player_pos.z * player_pos.z);
  const gen::Dir3 player_dir{det::Real(player_pos.x / player_len), det::Real(player_pos.y / player_len), det::Real(player_pos.z / player_len)};
  const double altitude = player_len - R;
  int rebuilt = 0;
  for (std::size_t i = 0; i < sites.size(); ++i) {
    const gen::Site& site = sites[i];
    CivAnchor::SiteMeshEntry& entry = civ->meshes[i];
    // Distance from the player to the site centre (surface arc + altitude).
    const double chord = std::sqrt(gen::chord_sq(player_dir, site.frame.up).to_double()) * R;
    const double dist = std::sqrt(chord * chord + altitude * altitude);
    const double range = 40000.0 + 4.0 * site.radius_m;
    if (dist > range) {
      if (entry.mesh != 0) {
        rhi->destroy_mesh(entry.mesh);
        entry.mesh = 0;
        entry.detail = -1;
      }
      continue;
    }
    int detail = 2;
    if (dist < 2500.0 + site.radius_m) detail = 0;
    else if (dist < 15000.0 + site.radius_m) detail = 1;
    // Near a big site only the blocks around the player are built at full
    // detail (a metropolis holds ~100k lots); the focus follows the player
    // and rebuilds when it moved half a focus radius.
    double fx = 0.0;
    double fy = 0.0;
    site.frame.to_local(player_dir, &fx, &fy);
    const double focus_radius = detail == 0 ? 1200.0 : 0.0;
    const bool refocus = detail == 0 && site.radius_m > focus_radius &&
                         (std::fabs(fx - entry.focus_x) > 0.5 * focus_radius ||
                          std::fabs(fy - entry.focus_y) > 0.5 * focus_radius);
    if (entry.mesh == 0 || entry.detail != detail || refocus) {
      if (rebuilt >= 2) {
        continue;  // spread rebuilds over frames
      }
      ++rebuilt;
      gen::SiteMeshParams params;
      params.detail = detail;
      params.method = civ->method;
      if (detail == 0 && site.radius_m > focus_radius) {
        params.focus_x = fx;
        params.focus_y = fy;
        params.focus_radius_m = focus_radius;
      }
      const gen::SiteMesh mesh = gen::build_site_mesh(*civ->sites, site, field, params);
      if (entry.mesh != 0) {
        rhi->destroy_mesh(entry.mesh);
        entry.mesh = 0;
      }
      if (!mesh.mesh.vertices.empty()) {
        entry.mesh = rhi->create_mesh_mat(mesh.mesh.vertices.data(), mesh.mesh.vertices.size());
      }
      entry.detail = detail;
      entry.focus_x = fx;
      entry.focus_y = fy;
      std::memcpy(entry.origin, mesh.mesh.origin, sizeof(entry.origin));
      std::memcpy(entry.palette, mesh.mesh.palette, sizeof(entry.palette));
    }
    if (entry.mesh == 0) continue;
    CivAnchor::TileEntry as_tile;
    as_tile.mesh = entry.mesh;
    std::memcpy(as_tile.origin, entry.origin, sizeof(as_tile.origin));
    std::memcpy(as_tile.palette, entry.palette, sizeof(as_tile.palette));
    push_item(as_tile, camera_pos, view_projection, items);
  }
}

}  // namespace inf::app
