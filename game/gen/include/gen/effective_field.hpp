#pragma once

#include "gen/terrain.hpp"

namespace inf::gen {

// The EFFECTIVE world state (prototype-v0 spec sections 3-4): procedural
// terrain (+) the player-diff overlay. Until M7 lands there is no diff, so
// this is a pure passthrough — but every consumer that must observe player
// edits (landing/grounding queries, digging raycasts) already goes through
// this interface, so the overlay plugs in without touching callers.
class EffectiveField {
 public:
  explicit EffectiveField(const gen::TerrainField& field) : field_(field) {}

  det::Real ground_radius_m(const gen::Dir3& unit_dir) const {
    return field_.ground_radius_m(unit_dir);
  }
  det::Real density(const gen::Dir3& position_m) const { return field_.density(position_m); }
  const gen::PlanetParams& planet() const { return field_.planet(); }
  const gen::TerrainField& terrain() const { return field_; }

 private:
  const gen::TerrainField& field_;
};

}  // namespace inf::gen
