#pragma once
#include "scene.hpp"

namespace cb {
// A continuous ceramic outrigger tied to the existing three structural floors.
// All coordinates are metres in the city's Y-up frame.
void build_sculpted_garden_frame(Scene &scene, Vec3 joint, Vec3 upper,
                                 Vec3 upper_anchor, Vec3 lower,
                                 std::uint32_t ceramic,
                                 Vec3 front = {.6442177f, 0, .7648422f});
} // namespace cb
