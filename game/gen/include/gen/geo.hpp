#pragma once

// Engine geometry/meshing vocabulary imported into the game's gen
// namespace (these types moved to the engine in the T0011 split; game
// code keeps using them unqualified).

#include "world/chunk_grid.hpp"
#include "world/cubesphere.hpp"
#include "world/mesher.hpp"
#include "world/noise.hpp"

namespace inf::gen {

using world::ChunkGrid;
using world::ChunkMesh;
using world::Dir3;
using world::FaceUV;
using world::FbmParams;
using world::PaddedDensity;
using world::TransitionMask;
using world::kTransitionUMinus;
using world::kTransitionUPlus;
using world::kTransitionVMinus;
using world::kTransitionVPlus;

using world::chord_sq;
using world::dir_to_face_uv;
using world::dot;
using world::face_uv_to_dir;
using world::fbm3;
using world::gradient_noise3;
using world::mesh_chunk;
using world::normalize;
using world::tangent_basis;
using world::warped_fbm3;

}  // namespace inf::gen
