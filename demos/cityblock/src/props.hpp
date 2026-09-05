#pragma once
// Ground kit: entrances and roofs for every building, basins, fountains,
// hedges, low walls, marble foundations with wide stairs, monuments, the
// unification ring, landing pads, curved pedestrian overpasses, and the
// government building. All parametric, all cheap.
#include <cstdint>
#include <vector>

#include "materials.hpp"
#include "math.hpp"
#include "rng.hpp"
#include "scene.hpp"

namespace cb {

enum class EntranceKind : std::uint8_t { Canopy, Portal, Stairs, Vestibule };
enum class RoofKind : std::uint8_t { Flat, Parapet, Green, Monopitch };
enum class MonumentKind : std::uint8_t { Pillar, Ribbon, Weave, Obelisk };

// Entrance centred at p on a facade whose outward normal is n (xz), ground
// at y; storey_h sizes the opening.
void build_entrance(Scene& sc, EntranceKind kind, Vec2 p, Vec2 n, float y, float storey_h, Rng& rng, int detail);
// Roof on a convex footprint whose walls end at y (top of the last storey).
void build_roof(Scene& sc, RoofKind kind, const std::vector<Vec2>& footprint, float y, Rng& rng, int detail, Mat wall);

void build_basin(Scene& sc, Vec2 c, float rx, float rz, bool round, float y, float rim_h);
void build_fountain(Scene& sc, Vec2 c, float radius, float y, Rng& rng, int detail);
void build_hedge(Scene& sc, Vec2 a, Vec2 b, float width, float height, float y);
// Hedges along the edges of a polygon, leaving a gap every gap_every metres.
void build_hedge_ring(Scene& sc, const std::vector<Vec2>& poly, float inset, float width, float height, float y, float gap_every, Rng& rng);
void build_low_wall(Scene& sc, const std::vector<Vec2>& poly, float inset, float height, float thickness, float y, Mat mat, float gap_every, Rng& rng);
// Marble platform with wide stairs on edge `stair_edge` (index into the
// footprint); returns the platform top polygon.
std::vector<Vec2> build_foundation(Scene& sc, const std::vector<Vec2>& footprint, float y, float height, int stair_edge, int detail);
void build_monument(Scene& sc, MonumentKind kind, Vec2 c, float y, float scale, Rng& rng, int detail);
void build_unification_ring(Scene& sc, Vec2 c, float y, float radius, float facing_rot, int detail);
void build_landing_pad(Scene& sc, Vec2 c, float radius, float y, Rng& rng, int detail);
// Curved pedestrian deck along a spline through ctrl (world xz) at deck_y,
// with stair towers at both ends down to ground_y.
void build_overpass(Scene& sc, const std::vector<Vec2>& ctrl, float deck_y, float ground_y, Rng& rng, int detail);
void build_government(Scene& sc, Vec2 c, float rot, float half, float y, Rng& rng, int detail);

}  // namespace cb
