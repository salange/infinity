#pragma once

#include <cstdint>

namespace inf::core {

// Chunk address (prototype-v0 spec section 4): not an entity with a key —
// an address. Cube-sphere face + quadtree cell + radial shell. Feeds
// Philox DRAW_CHUNK counters, names meshes, keys diff buckets.
struct ChunkAddr {
  std::uint8_t face{0};   // 0..5
  std::uint8_t lod{0};    // quadtree depth
  std::uint32_t i{0};     // cell coords, valid range [0, 2^lod)
  std::uint32_t j{0};
  std::int16_t shell{0};  // radial shell index relative to surface shell 0

  friend bool operator==(const ChunkAddr&, const ChunkAddr&) = default;
};

}  // namespace inf::core
