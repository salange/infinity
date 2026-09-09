#pragma once
// The asset catalog mode (`cityblock --asset SPEC`): one building or prop
// of the city library on a plaza plate, framed automatically, so every
// type and variant can be rendered on its own with a reproducible command
// line. The reference document (design/city-reference.md in the research
// repository) is illustrated from this mode.
//
// SPEC is `kind:name[;key=value...]`; `--asset list` prints every kind,
// name and key. Examples: `tower:diagrid`, `tower:curtain;base=podium;
// crown=mast`, `standard:residential;entrance=stairs;roof=green`,
// `government:3`, `monument:weave`, `plaza:formal`.
#include <string>

#include "rng.hpp"
#include "scene.hpp"

namespace cb {

// Builds the asset into `sc` (materials must be set), places the plate
// under it and sets the scene camera. Returns false with a message when
// the spec is unknown.
bool generate_asset(Scene& sc, const std::string& spec, Rng root, int detail, std::string* error);

// The catalog as text (kinds, names, keys), for `--asset list`.
std::string asset_catalog_text();

}  // namespace cb
