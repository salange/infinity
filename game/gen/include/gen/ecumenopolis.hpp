#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/buildings.hpp"
#include "gen/civ_types.hpp"
#include "gen/civilization.hpp"
#include "gen/height_modifier.hpp"
#include "gen/settlements.hpp"
#include "gen/sites.hpp"
#include "gen/terrain.hpp"
#include "world/mesher.hpp"

namespace inf::gen {

// T0020 WP7: ecumenopolis/v1 (design section 16). Level 7 is not a
// bigger site list; it is a different surface, and the cube-sphere
// quadtree IS the street plan:
//
//  - PLATES. Every province carries a plate at max(terrain) + 40 m
//    (ocean provinces: sea + 60 m on pylons). Adjacent plates meet over
//    a ramp band across the province border. The modifier returns the
//    plate everywhere; terrain above the plate survives as a preserved
//    peak (dark, unbuilt). The under-city below is later content.
//  - STREETS FROM THE QUADTREE. Face cells at the block level (~120 m)
//    are blocks; their borders are streets; every 8th border is an
//    arterial (~1 km) and every 64th a mega-avenue (~8 km). No layout
//    solve and no site list: a chunk knows it is city from its address.
//  - DISTRICTS. A planet-scale keyed value-noise field (300 / 60 / 12 km
//    wavelengths) gives each block a district type (civic spires,
//    residential canyons, industrial slabs, park domes) and a height
//    budget (200-1500 m). Blocks draw their towers from the block key
//    and run them through buildings/v1 with tier = Ecumenopolis.
//  - LIGHTS. Urban albedo everywhere, an emissive lattice following the
//    arterials with district intensity for the far-view bake.
//  - RUINED (Precursor homes): keyed collapsed plates revealing the
//    terrain, dark, towers truncated at keyed heights.
//
// Everything is a pure function of (body key, block address, state);
// a tile mesh is generated per tile like a terrain chunk, never from a
// planet-wide list. Identity-critical (det path, goldens): plate datums,
// district samples, block towers (footprints, heights, usage). Cosmetic:
// mesh detail.
class EcumenopolisField final : public HeightModifier {
 public:
  static constexpr double kPlateAboveTerrainM = 40.0;
  static constexpr double kPlateAboveSeaM = 60.0;
  static constexpr double kBlockTargetM = 120.0;
  static constexpr int kArterialShift = 3;   // arterial cell = 8 x 8 blocks
  static constexpr int kAvenueShift = 6;     // mega-avenue cell = 64 x 64 blocks
  static constexpr double kStreetHalfM = 6.0;
  static constexpr double kArterialHalfM = 20.0;
  static constexpr double kAvenueHalfM = 60.0;
  static constexpr double kRampBand = 0.15;  // half width of the plate ramp, in province cells
  static constexpr double kHoleFraction = 0.12;  // ruined: collapsed arterial cells

  EcumenopolisField(const core::Key& body_entity_key, const TerrainField& field,
                    const SettlementPlan& plan, const RaceParams& race,
                    const std::vector<FactionParams>& factions, const CivState& state);

  // --- the HeightModifier ------------------------------------------------
  det::Real modify(const Dir3& unit_dir, det::Real base_m, const BaseEval& base_at) const override;
  Urban urban(const Dir3& unit_dir, det::Real surface_m) const override;
  bool near(const Dir3& unit_dir) const override { (void)unit_dir; return true; }

  // --- plates --------------------------------------------------------------
  // The plate datum (metres above the nominal radius) at a direction:
  // the province plate with the ramp blend across borders. Ruined: the
  // hole factor (0 = fully collapsed) is returned separately.
  double plate_m(const Dir3& unit_dir) const;
  double hole_factor(const Dir3& unit_dir) const;  // 1 intact, 0 collapsed (ruined only)
  double province_plate_m(std::uint32_t province_index) const { return plate_[province_index]; }
  double plate_min_m() const { return plate_min_; }
  double plate_max_m() const { return plate_max_; }

  // --- districts -------------------------------------------------------------
  enum class DistrictType : std::uint8_t { Residential = 0, Civic = 1, Industrial = 2, Park = 3 };
  struct District {
    DistrictType type{DistrictType::Residential};
    double height_budget_m{300.0};  // for the tallest tower of a block
    double density{0.85};           // fraction of a block's footprint built
    double light{0.6};              // night-light intensity 0..1
  };
  District district(const Dir3& unit_dir) const;

  // --- the block lattice ------------------------------------------------------
  struct BlockId {
    std::uint8_t face{0};
    std::uint32_t bi{0};
    std::uint32_t bj{0};
  };
  int block_level() const { return block_level_; }
  std::uint32_t blocks_per_face() const { return blocks_per_face_; }
  double block_m() const { return block_m_; }
  BlockId block_of(const Dir3& unit_dir) const;
  Dir3 block_centre(const BlockId& block) const;
  // Centre of the arterial cell (8 x 8 blocks) a block belongs to: the
  // cell decides the district, the preserved-peak test and the collapse.
  Dir3 arterial_cell_centre(const BlockId& block) const;
  // Corner k (0..3, counter-clockwise in face uv) of a block.
  Dir3 block_corner(const BlockId& block, int k) const;
  core::Key block_key(const BlockId& block) const;

  // The towers of a block, as lots in a caller-supplied local frame
  // (site-local metres of `frame`): footprints inset from the streets,
  // usage and height from the district, styles from the race. Pure
  // function of the block; identical from every frame up to the frame's
  // own mapping. Returns the number of towers (0 for preserved peaks and
  // collapsed plates).
  // What an arterial cell decides for all its blocks: whether it is
  // built at all (not a preserved peak, not collapsed) and its district.
  // One terrain read and one district sample per cell; tile builders
  // compute it once per cell and pass it down.
  struct CellInfo {
    bool built{false};
    District district;
  };
  CellInfo cell_info(const BlockId& block) const;
  int towers_in_block(const BlockId& block, const SiteFrame& frame, std::vector<Lot>* out,
                      const CellInfo* info = nullptr) const;
  // The key a tower's building draws from.
  core::Key tower_key(const BlockId& block, int tower) const;

  // --- tiles (the per-chunk unit) ------------------------------------------
  // A tile is a face cell `shift` levels above the block level: the
  // near tile (shift 3, 8 x 8 blocks, ~1 km) and the far tile (shift 6,
  // 64 x 64 blocks, ~8 km).
  struct TileId {
    int shift{3};
    std::uint8_t face{0};
    std::uint32_t ti{0};
    std::uint32_t tj{0};
    friend bool operator==(const TileId&, const TileId&) = default;
  };
  TileId tile_of(const Dir3& unit_dir, int shift) const;
  Dir3 tile_centre(const TileId& tile) const;
  double tile_m(int shift) const { return block_m_ * static_cast<double>(1U << shift); }
  SiteFrame tile_frame(const TileId& tile) const;

  const CivState& state() const { return state_; }
  const StyleVector& style() const { return style_; }
  bool ruined() const { return state_.ruined; }
  int capital_province() const { return capital_; }
  const TerrainField& field() const { return field_; }

 private:
  double province_datum(const Dir3& probe) const;
  double noise(int octave, double x, double y, double z, int word) const;

  core::Key key_;
  const TerrainField& field_;
  CivState state_;
  StyleVector style_;
  std::uint32_t n_{0};            // provinces per face edge
  double radius_m_{0.0};
  double sea_m_{0.0};
  bool has_sea_{false};
  int capital_{-1};
  int block_level_{13};
  std::uint32_t blocks_per_face_{8192};
  double block_m_{120.0};
  std::vector<float> plate_;      // per province (face-major)
  double plate_min_{0.0};
  double plate_max_{0.0};
  core::Key octave_keys_[3];
  double octave_scale_[3]{};      // lattice cells per unit direction
};

// The mesh of one tile (origin at the tile centre on its plate). detail:
// 0 towers with parts, 1 grammar towers, 2 mass towers (one box per
// tower), 3 the far view (one slab per arterial cell plus its two
// tallest towers as boxes). Terrain-mesh vertex format (10 floats) with
// the building palette, so the towers ride the lit terrain pipeline.
struct EcumenopolisMesh {
  world::ChunkMesh mesh;
  std::uint32_t block_count{0};
  std::uint32_t tower_count{0};
  std::uint32_t triangle_count{0};
};
EcumenopolisMesh build_ecumenopolis_tile(const EcumenopolisField& field,
                                         const EcumenopolisField::TileId& tile, int detail,
                                         BuildingMethod method = BuildingMethod::GrammarParts);

}  // namespace inf::gen
