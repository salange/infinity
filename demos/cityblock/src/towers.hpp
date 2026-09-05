#pragma once
// Parametric towers for the tech faction: plan × vertical profile × facade
// system × base × crown, each an independent axis, so one generator yields
// the hero buildings of the centre block and endless variants for the rest
// of the city. Everything is geometry computed from the spec and a key.
#include <cstdint>
#include <vector>

#include "materials.hpp"
#include "math.hpp"
#include "mesh.hpp"
#include "rng.hpp"
#include "scene.hpp"

namespace cb {

enum class PlanKind : std::uint8_t { Superellipse, Lens, Circle, RoundedRect, Polygon };
enum class FacadeKind : std::uint8_t { Curtain, Sail, Ribbon, FinWeave, Louvre, Diagrid, XFrame, HexLattice };
enum class BaseKind : std::uint8_t { Lobby, Podium, Colonnade, Plinth, Legs };
enum class CrownKind : std::uint8_t { Parapet, Lattice, Mast, Lantern, Louvres };

struct TowerSpec {
  // plan (half extents in metres)
  PlanKind plan{PlanKind::Superellipse};
  float a{16.0f}, b{16.0f};
  float exponent{3.0f};   // superellipse
  int sides{5};           // polygon
  float rot{0.0f};
  // vertical profile
  int floors{30};
  float floor_h{4.0f};
  float taper{0.0f};      // 0..0.4, quadratic shrink toward the top
  float tip{0.0f};        // 0..0.6, pinch in the top 15 %
  float twist{0.0f};      // total rotation over the height (radians)
  int setback_floor{-1};  // -1 = none
  float setback_scale{0.78f};
  // facade
  FacadeKind facade{FacadeKind::Curtain};
  Mat glass{M_GLASS_BLUE};
  Mat frame{M_DARK_METAL};   // mullions / transoms
  Mat member{M_WHITE_METAL}; // lattice members, fins
  float module_w{3.0f};
  float spandrel_h{0.0f};
  float fin_depth{0.6f};
  float member_r{0.42f};
  int lattice_rows{2};       // floors per lattice cell
  bool floor_bands{true};    // dark slab rings behind the glass
  // base
  BaseKind base{BaseKind::Lobby};
  int base_floors{2};
  float base_scale{1.7f};    // podium plan scale
  // crown
  CrownKind crown{CrownKind::Parapet};
  float random{0.0f};
};

// Builds the tower at `centre` with its ground at `base_y`. detail: 2 full,
// 1 near context (no mullion boxes, coarser members), 0 far context.
void build_tower(Scene& sc, const TowerSpec& spec, Vec2 centre, float base_y, Rng rng, int detail);

// Named families (the hero buildings), with their parameters exposed.
TowerSpec spec_diagrid(float half, int floors);
TowerSpec spec_lens(float half_w, float half_t, int floors, float rot);
TowerSpec spec_finweave(float radius, int floors);
TowerSpec spec_xframe(float hx, float hz, int floors);
TowerSpec spec_hex(float half, int floors);
TowerSpec spec_sail(float half_w, float half_t, int floors, float rot);

// A random variant: picks a family, then jitters every axis within the
// family's plausible ranges.
TowerSpec random_tower(Rng& rng, float footprint_half, int max_floors);
// Cheaper variant for the far ring.
TowerSpec random_context_tower(Rng& rng, float footprint_half, int max_floors);

// Shared podium with two or three towers of one family.
void build_tower_group(Scene& sc, Rng rng, Vec2 centre, float rot, int detail);

// Helpers shared with the site generator.
Vec3 P3(Vec2 xz, float y);
void slab(Mesh& mesh, const std::vector<Vec2>& plan, float y, float thickness, Mat mat);
void parapet(Mesh& mesh, const std::vector<Vec2>& plan, float y, float height, float thickness, Mat mat);
void roof_equipment(Mesh& mesh, Rng& rng, const std::vector<Vec2>& plan, float y, int count);
void gen_tree(Scene& sc, Rng rng, Vec3 base, float height);
void gen_planter(Scene& sc, Rng rng, Vec2 centre, float hx, float hz, float y);

}  // namespace cb
