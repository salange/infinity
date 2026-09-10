#pragma once
#include "rng.hpp"
#include "scene.hpp"

namespace cb {
// Reviewed honed stone for exposed market floors; ground wetness stays
// explicit.
std::uint32_t market_paving_material(Scene &scene);
// Low planted island on the existing western market promenade, in metres.
void stage_street_forecourt(Scene &scene, Rng rng);
} // namespace cb
