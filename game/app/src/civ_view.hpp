#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/seed.hpp"
#include "core/time/world_time.hpp"
#include "gen/civil.hpp"
#include "gen/colony.hpp"
#include "gen/ecumenopolis.hpp"
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
//
// WP7: at level 7 the body carries an ecumenopolis instead of sites —
// the plate modifier and a tile cache streamed by distance like terrain
// chunks (far 8 km tiles, mid 4 km tiles, near 1 km tiles).
struct CivAnchor {
  bool settled{false};
  gen::BuildingMethod method{gen::BuildingMethod::GrammarParts};
  gen::CivState state;
  gen::RaceParams race;
  std::vector<gen::FactionParams> factions;
  std::unique_ptr<gen::SettlementPlan> plan;
  std::unique_ptr<gen::SiteField> sites;
  std::unique_ptr<gen::CivilField> civil;
  std::unique_ptr<gen::EcumenopolisField> ecumenopolis;

  struct SiteMeshEntry {
    std::uint32_t site_index{0};
    int detail{-1};
    std::uint32_t mesh{0};  // rhi mesh id, 0 = none
    double origin[3]{0.0, 0.0, 0.0};
    std::uint8_t palette[4]{0, 0, 0, 0};
    double focus_x{0.0}, focus_y{0.0};
  };
  std::vector<SiteMeshEntry> meshes;

  struct TileEntry {
    gen::EcumenopolisField::TileId id;
    int detail{3};
    std::uint32_t mesh{0};
    double origin[3]{0.0, 0.0, 0.0};
    std::uint8_t palette[4]{0, 0, 0, 0};
    bool wanted{false};
  };
  std::vector<TileEntry> tiles;
};

// Builds the civ view for a body of a system (nullptr when the body is
// unsettled at `now`). Sets the modifier on `field`.
std::unique_ptr<CivAnchor> build_civ_anchor(const core::Seed128& seed,
                                            const gen::RaceRegistry& registry,
                                            const gen::ColonyResolver& resolver,
                                            const gen::SystemCell& cell, int slot, int moon,
                                            const core::Key& body_entity,
                                            gen::TerrainField* field, core::WorldTime now);

// Appends draw items for every site (or ecumenopolis tile) within range
// of the player, (re)building meshes at a detail level by distance.
// player_pos and camera_pos are planet-local metres; view_projection is
// the camera's.
void draw_civ_sites(CivAnchor* civ, render::Rhi* rhi, const gen::TerrainField& field,
                    const render::Vec3& player_pos, const render::Vec3& camera_pos,
                    const render::Mat4& view_projection,
                    std::vector<render::Rhi::DrawItem>* items);

// Releases the meshes (on re-anchoring).
void release_civ_meshes(CivAnchor* civ, render::Rhi* rhi);

// --- the far-view bake (WP7) --------------------------------------------------
// The orbit impostor is baked from the live generators on a background
// thread with its own TerrainField per body; the civilization surface
// (urban albedo, night lights, plates) must be on that field too. The
// registry and resolver are not thread-safe, so the main thread gathers
// the per-body civ inputs first and the worker builds the modifier from
// those copies alone.
struct CivBodyInputs {
  int slot{0};
  int moon{-1};
  gen::CivState state;
  gen::RaceParams race;
  std::vector<gen::FactionParams> factions;
};
std::vector<CivBodyInputs> gather_civ_bodies(const core::Seed128& seed,
                                             const gen::RaceRegistry& registry,
                                             const gen::ColonyResolver& resolver,
                                             const gen::SystemCell& cell, core::WorldTime now);

// The modifier for one body on a caller-owned field; sets it on the
// field. Keep it alive for as long as the field is sampled.
struct CivModifier {
  std::unique_ptr<gen::SettlementPlan> plan;
  std::unique_ptr<gen::SiteField> sites;
  std::unique_ptr<gen::CivilField> civil;
  std::unique_ptr<gen::EcumenopolisField> ecumenopolis;
};
std::unique_ptr<CivModifier> build_civ_modifier(const core::Key& body_entity, gen::TerrainField* field,
                                                const CivBodyInputs& inputs);

}  // namespace inf::app
