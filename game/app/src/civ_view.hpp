#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/seed.hpp"
#include "core/time/world_time.hpp"
#include "gen/civil.hpp"
#include "gen/colony.hpp"
#include "gen/settlements.hpp"
#include "gen/site_mesh.hpp"
#include "gen/sites.hpp"
#include "gen/terrain.hpp"
#include "render/math.hpp"
#include "render/rhi.hpp"

namespace inf::app {

// T0020 WP5: the civilization view of the anchored body — its plan, its
// sites, the civil terrain modifier (set on the terrain field before any
// worker samples it), and the site mass meshes drawn through the lit
// terrain material pipeline. Everything here is a view of pure
// functions; nothing is stored beyond mesh caches.
struct CivAnchor {
  bool settled{false};
  gen::BuildingMethod method{gen::BuildingMethod::GrammarParts};
  gen::CivState state;
  gen::RaceParams race;
  std::vector<gen::FactionParams> factions;
  std::unique_ptr<gen::SettlementPlan> plan;
  std::unique_ptr<gen::SiteField> sites;
  std::unique_ptr<gen::CivilField> civil;

  struct SiteMeshEntry {
    std::uint32_t site_index{0};
    int detail{-1};
    std::uint32_t mesh{0};  // rhi mesh id, 0 = none
    double origin[3]{0.0, 0.0, 0.0};
    std::uint8_t palette[4]{0, 0, 0, 0};
    double focus_x{0.0}, focus_y{0.0};
  };
  std::vector<SiteMeshEntry> meshes;
};

// Builds the civ view for a body of a system (nullptr when the body is
// unsettled at `now`). Sets the modifier on `field`.
std::unique_ptr<CivAnchor> build_civ_anchor(const core::Seed128& seed,
                                            const gen::RaceRegistry& registry,
                                            const gen::ColonyResolver& resolver,
                                            const gen::SystemCell& cell, int slot, int moon,
                                            const core::Key& body_entity,
                                            gen::TerrainField* field, core::WorldTime now);

// Appends draw items for every site within range of the player,
// (re)building meshes at a detail level by distance. player_pos and
// camera_pos are planet-local metres; view_projection is the camera's.
void draw_civ_sites(CivAnchor* civ, render::Rhi* rhi, const gen::TerrainField& field,
                    const render::Vec3& player_pos, const render::Vec3& camera_pos,
                    const render::Mat4& view_projection,
                    std::vector<render::Rhi::DrawItem>* items);

// Releases the meshes (on re-anchoring).
void release_civ_meshes(CivAnchor* civ, render::Rhi* rhi);

}  // namespace inf::app
