#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/civ_types.hpp"
#include "gen/provinces.hpp"
#include "gen/terrain.hpp"

namespace inf::gen {

// T0020 WP4: the planet plan — settlements/v1 (design section 13).
//
// A bounded coarse global pass (seeding spec section 5.2): at most
// 6 * 24^2 = 3 456 provinces, computed once per body and cached like the
// climate percentile bands. Every province gets a suitability from cheap
// layer reads (macro land/sea lattice, province relief, drainage order,
// climate means) reweighted by the race's habitat, and a keyed score.
// The RANKING never depends on time; the level and the progress inside
// it only move the cut-off, so every province settled at t is settled at
// every later t while the race lives (additive growth at planet scale).
// Tiers, regions, the capital, the home-world faction map and the road
// graph (Kruskal in canonical edge order, owner-cell edges) follow.

struct ProvinceSite {
  CellId cell;
  std::uint32_t index{0};        // face-major province index
  Dir3 centre;                   // representative direction (unit)
  float suitability{0.0f};       // 0..1 before the keyed jitter
  float score{0.0f};             // suitability * (0.6 + 0.4 u)
  std::uint32_t rank{0};         // 0 = best; ranks over suitable provinces only
  bool suitable{false};          // suitability > 0 (ocean = 0 below level 7)
  bool ocean{false};
  bool coastal{false};
  bool river{false};
  float flatness{0.0f};
  float altitude_m{0.0f};        // representative elevation above the sea datum
  // Time-dependent (the moving cut-off):
  bool settled{false};
  SettlementTier tier{SettlementTier::None};
  SettlementTier max_tier{SettlementTier::Hamlet};
  float growth{1.0f};            // site growth factor exp(N(0, 0.2))
  float site_progress{0.0f};     // progress inside the current tier [0, 1)
  int region{-1};
  bool region_capital{false};
  bool capital{false};
  int faction{-1};               // home worlds: the province faction map
  float radius_m{0.0f};          // site radius by tier
};

struct Road {
  std::uint32_t a{0};  // province indices, a < b (owner = a)
  std::uint32_t b{0};
  bool trunk{false};   // inter-region trunk road (level >= 4)
  float width_m{8.0f};
  // Great arc from a to b with keyed lateral wobble and a slope-avoiding
  // pull, three midpoint refinements: 9 unit directions.
  Dir3 points[9];
};

struct SettlementPlan {
  int level{0};
  double progress{0.0};
  bool ruined{false};
  bool domed{false};
  bool is_home{false};
  std::uint32_t cells_per_face{0};
  std::uint32_t suitable_count{0};
  std::uint32_t settled_count{0};
  int capital{-1};                       // province index, -1 none
  std::vector<ProvinceSite> provinces;   // all cells, face-major
  std::vector<std::uint32_t> by_rank;    // suitable province indices, best first
  std::vector<std::uint32_t> region_capitals;
  std::vector<Road> roads;

  const ProvinceSite& at(const CellId& cell) const {
    return provinces[(static_cast<std::size_t>(cell.face) * cells_per_face + cell.ci) *
                         cells_per_face +
                     cell.cj];
  }
  std::string to_json() const;
};

// Site radius by tier (design section 13.3), metres.
double tier_radius_m(SettlementTier tier);
// The settled fraction of suitable provinces at a level (design 13.2
// table), continuous in progress.
double settled_fraction(int level, double progress);

class SettlementPlanner {
 public:
  // The time-independent part: suitability, scores, ranking. Reads the
  // body's layers through the TerrainField (macro lattice, provinces,
  // drainage, climate); all draws under settlements/v1 off the body key.
  SettlementPlanner(const core::Key& body_entity_key, const TerrainField& field,
                    const RaceParams& race, bool domed);

  // The full plan at a civilization state (level, progress, ruined,
  // home): the moving cut-off, tiers, regions, capital, roads, and on a
  // home world the province faction map from the race's factions.
  SettlementPlan plan(const CivState& state, const std::vector<FactionParams>& factions) const;

  // Cheap re-cut of an existing plan for a new (level, progress) — the
  // per-frame path; identical to plan() for the same inputs.
  void update(SettlementPlan* plan, const CivState& state,
              const std::vector<FactionParams>& factions) const;

  const std::vector<ProvinceSite>& base() const { return base_; }
  std::uint32_t cells_per_face() const { return n_; }
  const core::Key& key() const { return key_; }

  // Road geometry between two provinces — symmetric in its arguments and
  // a pure function of the pair, so either endpoint's cell recomputes the
  // identical road.
  Road road_between(std::uint32_t a, std::uint32_t b, bool trunk) const;

 private:
  double arc(std::uint32_t a, std::uint32_t b) const;
  void assign_tiers(SettlementPlan* plan) const;
  double settled_level_of(std::uint32_t rank) const;
  void assign_regions(SettlementPlan* plan) const;
  void assign_factions(SettlementPlan* plan, const std::vector<FactionParams>& factions) const;
  void build_roads(SettlementPlan* plan) const;

  core::Key key_;
  const TerrainField& field_;
  RaceParams race_;
  bool domed_;
  std::uint32_t n_{0};
  double radius_m_{0.0};
  std::vector<ProvinceSite> base_;
  std::vector<std::uint32_t> by_rank_;
  std::uint32_t suitable_count_{0};
};

}  // namespace inf::gen
