#pragma once
// A small catalog scene of the tech faction's architecture — three tower
// families, the standard types, the government building with its ring
// plaza and a few ground-kit pieces — for pipeline checks (the app's
// --city-showcase, the CLI golden). Pure function of the key.
#include "city/rng.hpp"
#include "city/scene.hpp"

namespace inf::city {

// Fills `sc` (materials must already be set) within a ~260 m square
// around the origin, ground at y = 0.
void generate_showcase_small(Scene& sc, Rng root);

}  // namespace inf::city
