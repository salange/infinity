#pragma once
#include "scene.hpp"

namespace cb {
// Street-facing market structure. Same six ground/upper stations and occupied
// building as the original frontage; all geometry uses metres and Y up.
void build_market_structure(Scene &scene, std::uint32_t ceramic);
}
