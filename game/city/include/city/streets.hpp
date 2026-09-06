#pragma once
// The street kit: lane paint, crosswalks, asphalt ribbons, raised medians
// with hedges, and the plaza kinds (fountain, formal, terraced park,
// monument, garden, landing) that fill a courtyard or a block. Shared by
// the site bridge and the cityblock demo's own layout.
#include <cstdint>
#include <vector>

#include "city/math.hpp"
#include "city/rng.hpp"
#include "city/scene.hpp"

namespace inf::city {

// Marks on a road segment: centre line(s) and dashed lane lines; arteries
// get lanes either side of a median the caller places.
void road_paint(Scene& sc, Vec2 a, Vec2 b, float width, bool artery, float y);
// Zebra crossing centred at `centre`, `across` the road of width road_w.
void crosswalk(Scene& sc, Vec2 centre, Vec2 across, float road_w, float y);
// Asphalt ribbon along a polyline.
void road_surface(Scene& sc, const std::vector<Vec2>& line, float width, float y);
// Raised median strip between a and b (metres in, both ends) with a hedge.
void build_median(Scene& sc, Vec2 a, Vec2 b, float width, float curb_h, float y);

enum class PlazaKind : std::uint8_t { Fountain, Formal, Terraced, Monument, Garden, Landing };

// Fills the polygon (ccw) with a plaza; `trees` is a budget decremented
// by every tree placed.
void build_plaza(Scene& sc, PlazaKind kind, const std::vector<Vec2>& poly, float y, Rng rng, int detail, int* trees);

}  // namespace inf::city
