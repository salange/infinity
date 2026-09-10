#pragma once
#include "scene.hpp"

namespace cb {
// Replaces landing()'s three front annular overhangs and three independent
// elliptical shades. Keeps their world anchors, support columns and lights.
void stage_landing_canopies(Scene& scene);
}  // namespace cb
