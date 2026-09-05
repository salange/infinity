#include "civ_view.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace inf::app {

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
  const gen::SettlementPlanner planner(context.bodies[static_cast<std::size_t>(pick)].body_entity,
                                       *field, civ->race, civ->state.domed);
  civ->plan = std::make_unique<gen::SettlementPlan>(planner.plan(civ->state, civ->factions));
  civ->sites = std::make_unique<gen::SiteField>(
      context.bodies[static_cast<std::size_t>(pick)].body_entity, *field, *civ->plan, civ->race,
      civ->factions, civ->state);
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

void release_civ_meshes(CivAnchor* civ, render::Rhi* rhi) {
  if (civ == nullptr) return;
  for (auto& entry : civ->meshes) {
    if (entry.mesh != 0) {
      rhi->destroy_mesh(entry.mesh);
      entry.mesh = 0;
    }
  }
  civ->meshes.clear();
}

void draw_civ_sites(CivAnchor* civ, render::Rhi* rhi, const gen::TerrainField& field,
                    const render::Vec3& player_pos, const render::Vec3& camera_pos,
                    const render::Mat4& view_projection,
                    std::vector<render::Rhi::DrawItem>* items) {
  if (civ == nullptr || civ->sites == nullptr) return;
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
    if (dist < 5000.0 + site.radius_m) detail = 0;
    else if (dist < 15000.0 + site.radius_m) detail = 1;
    // Near a big site only the blocks around the player are built at full
    // detail (a metropolis holds ~100k lots); the focus follows the player
    // and rebuilds when it moved half a focus radius.
    double fx = 0.0;
    double fy = 0.0;
    site.frame.to_local(player_dir, &fx, &fy);
    const double focus_radius = detail == 0 ? 2500.0 : 0.0;
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
}

}  // namespace inf::app
