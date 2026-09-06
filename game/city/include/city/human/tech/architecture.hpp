#pragma once
// Human, tech faction: the parametric towers, the standard buildings and
// the ground kit of the cityblock demo, driven by the lot's StyleVector.
// Height budget and footprint decide tower against standard; the faction
// type picks the materials (Government marble and white, Independent
// warm panels, Outlaw dark panels, machine factions chrome and lattice);
// usage picks the type (civic, industrial, agricultural, pads,
// monuments); construction shortens an unfinished building.
#include "city/architecture.hpp"
#include "city/materials.hpp"

namespace inf::city::human_tech {

// Materials the faction type picks for walls, glazing, frames and members.
struct FactionMaterials {
  Mat wall;
  Mat wall_alt;
  Mat glass;
  Mat glass_tower;
  Mat frame;
  Mat member;
  Mat floor;
};
FactionMaterials faction_materials(const gen::StyleVector& style);

class TechArchitecture final : public Architecture {
 public:
  const char* name() const override { return "human/tech"; }
  std::vector<MaterialDesc> materials() const override;
  LotBuildResult build_lot(Scene& sc, const LotInput& lot, Rng rng, int detail) const override;
  void build_key(Scene& sc, KeyRole role, Vec2 centre, float rot, float half, float y, Rng rng,
                 int detail, int stage) const override;
  void build_tower_block(Scene& sc, const TowerBlockInput& in, Rng rng, int detail) const override;
};

const TechArchitecture& instance();

}  // namespace inf::city::human_tech
