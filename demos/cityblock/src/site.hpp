#pragma once
// Landscape and street furniture generators shared by the city layout.
#include <vector>

#include "math.hpp"
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
void gen_tree(Scene& sc, Rng rng, Vec3 base, float height);
void gen_lamp(Scene& sc, Vec3 base, float yaw);
void gen_bench(Scene& sc, Vec3 pos, float yaw);
void gen_planter(Scene& sc, Rng rng, Vec2 centre, float hx, float hz, float y);
void gen_park(Scene& sc, Rng rng, Vec2 centre, float y);
void gen_pavilion(Scene& sc, Rng rng, Vec2 centre, float radius, float height, float y);
std::vector<Vec2> ribbon(const std::vector<Vec2>& line, float half_width);
}  // namespace cb
