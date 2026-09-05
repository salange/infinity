#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/civ_types.hpp"

namespace inf::gen {

// T0020 WP6: buildings/v1 — the building executor (design section 15;
// decision in scratchpad/DECISIONS.md 2026-09-05 "WP6"). A split/shape
// grammar over box scopes in site-local metres (x east, y north, z up
// from the lot's ground), producing triangles with a material slot.
// ONE executor, TWO terminal sets: a rule emits generated geometry
// (walls, roofs, mounds, spires) or an instanced PART from the generated
// part library (window bay, door, balcony, conduit, antenna, cap). Every
// draw comes from the lot's key under buildings/v1; the geometry is a
// pure function of (Lot, StyleVector, key, detail).
//
// Output vertices: [x y z nx ny nz slot] per vertex, three per triangle.
// Slots: 0 walls, 1 roof/trim, 2 glass, 3 accent (see building_palette).

enum class BuildingMethod : std::uint8_t {
  Mass = 0,          // the WP5 box (far LOD / comparison baseline)
  Grammar = 1,       // grammar geometry, flat facades (mid LOD)
  GrammarParts = 2,  // grammar geometry + instanced parts (near LOD)
};

struct BuildingParams {
  BuildingMethod method{BuildingMethod::GrammarParts};
  // Ground elevation under the lot relative to the site datum (from the
  // civil-modified terrain) — the executor sinks the base 0.8 m below.
  double ground_z{0.0};
};

struct BuildingMesh {
  std::vector<float> vertices;  // 7 floats per vertex
  std::uint32_t triangle_count{0};
  float top_z{0.0f};            // highest point (metres above the lot ground)
  bool emissive_windows{false};
};

// The four-material palette a lot's building uses (walls, roof, glass,
// accent), by material family, faction, ruin and dome state.
void building_palette(const StyleVector& style, std::uint8_t out[4]);

// Executes the grammar for one lot. lot_key = derive_child(K_buildings,
// kind::Lot, province, lot.id) — the caller derives it so tests can key
// buildings without a site.
BuildingMesh build_building(const Lot& lot, const core::Key& lot_key, const BuildingParams& params);

// The rule family the executor picks for a style (tests, tooling).
enum class BuildingRuleSet : std::uint8_t {
  Stacked = 0,    // Humanoid and default: masses, floors, bays, roofs
  Ziggurat = 1,   // Reptilian
  Mound = 2,      // Insectoid
  Spire = 3,      // Avian
  Stilt = 4,      // Aquatic
  Cap = 5,        // Fungoid
  HexLattice = 6, // Machine
  Crystal = 7,    // Crystalline
  Monolith = 8,   // Precursor
  Dome = 9,       // domed colonies
  Megatower = 10, // ecumenopolis blocks (T0020 WP7)
};
BuildingRuleSet rule_set_for(const StyleVector& style);

}  // namespace inf::gen
