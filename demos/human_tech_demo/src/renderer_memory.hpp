#pragma once
// CPU-only size contracts shared by uploads, visibility compaction and tests.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cb::memory {
inline constexpr std::uint64_t upload_bytes = 16 * 1024 * 1024;
// Main camera, three solar cascades, ocean, street puddles and civic pond.
inline constexpr std::uint32_t visibility_views = 7;

inline std::uint64_t bytes(std::uint64_t count, std::uint64_t stride) {
  if (stride && count > std::numeric_limits<std::uint64_t>::max() / stride)
    throw std::length_error("buffer byte count overflows uint64");
  return count * stride;
}

inline std::uint64_t buffer_size(std::uint64_t requested, std::uint64_t limit) {
  if (requested > std::numeric_limits<std::uint64_t>::max() - 3)
    throw std::length_error("aligned buffer byte count overflows uint64");
  const auto aligned = (requested + 3) & ~std::uint64_t{3};
  if (aligned > limit)
    throw std::length_error("aligned buffer exceeds adapter limit");
  return aligned;
}

inline void buffer_write(std::uint64_t capacity, std::uint64_t offset,
                         std::uint64_t size) {
  if ((offset | size) & 3)
    throw std::length_error("buffer write must be aligned to four bytes");
  if (offset > capacity || size > capacity - offset)
    throw std::length_error("buffer write exceeds allocated capacity");
}

inline std::uint32_t instance_capacity(std::uint64_t source_count) {
  // The identity transform is shared; each authored instance can occur in
  // every visibility list. Keep all firstInstance/count values representable.
  if (!source_count)
    throw std::length_error("instance source requires the identity transform");
  const auto copies = bytes(source_count - 1, visibility_views);
  if (copies >= std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("visibility instance count exceeds uint32");
  return static_cast<std::uint32_t>(copies + 1);
}

inline std::uint64_t chunk_records(std::uint64_t remaining,
                                   std::uint64_t record_bytes) {
  if (!record_bytes || record_bytes > upload_bytes)
    throw std::length_error("upload record exceeds staging budget");
  return std::min(remaining, upload_bytes / record_bytes);
}

struct TextureChunk {
  std::uint32_t y, z, height, depth;
  std::uint64_t offset, size;
};

template <class Emit>
void texture_chunks(std::uint32_t width, std::uint32_t height,
                    std::uint32_t depth, std::uint32_t bpp, Emit emit) {
  if (!width || !height || !depth || !bpp)
    throw std::length_error("texture upload dimensions must be positive");
  const auto row = bytes(width, bpp), slice = bytes(row, height);
  (void)bytes(slice, depth);
  if (row > upload_bytes)
    throw std::length_error("texture row exceeds staging budget");
  if (slice <= upload_bytes) {
    for (std::uint32_t z = 0; z < depth;) {
      const auto count =
          static_cast<std::uint32_t>(chunk_records(depth - z, slice));
      emit(TextureChunk{0, z, height, count, z * slice, count * slice});
      z += count;
    }
  } else {
    for (std::uint32_t z = 0; z < depth; ++z)
      for (std::uint32_t y = 0; y < height;) {
        const auto count =
            static_cast<std::uint32_t>(chunk_records(height - y, row));
        emit(TextureChunk{y, z, count, 1, z * slice + y * row, count * row});
        y += count;
      }
  }
}
} // namespace cb::memory
