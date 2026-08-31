#pragma once

#include "gen/terrain.hpp"
#include "world/edit_store.hpp"

namespace inf::gen {

// The EFFECTIVE world state (prototype-v0 spec sections 3-4): procedural
// terrain (+) the player-diff overlay (M7). Every consumer that must
// observe player edits (landing/grounding queries, digging raycasts) goes
// through this interface; with no store attached it is a pure passthrough.
class EffectiveField {
 public:
  explicit EffectiveField(const gen::TerrainField& field,
                          const world::EditStore* edits = nullptr)
      : field_(field), edits_(edits) {}

  // Topmost solid/air crossing along the radial through unit_dir — the
  // walkable ground, craters and built-up material included.
  det::Real ground_radius_m(const gen::Dir3& unit_dir) const;
  // Walking floor: first crossing at or below from_r (cave interiors),
  // with the edit overlay applied — see TerrainField::ground_radius_below_m.
  det::Real ground_radius_below_m(const gen::Dir3& unit_dir, det::Real from_r) const;

  det::Real density(const gen::Dir3& position_m) const;

  const gen::PlanetParams& planet() const { return field_.planet(); }
  const gen::TerrainField& terrain() const { return field_; }
  const world::EditStore* edits() const { return edits_; }

 private:
  const gen::TerrainField& field_;
  const world::EditStore* edits_;
};

}  // namespace inf::gen
