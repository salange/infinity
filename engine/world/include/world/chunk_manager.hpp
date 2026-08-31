#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/chunk_addr.hpp"
#include "world/chunk_sampler.hpp"
#include "world/mesher.hpp"

// Streaming (M4 complete): per-face quadtrees balanced to max one lod
// level between lateral neighbors; per-column Transvoxel transition masks;
// chunks re-mesh automatically when a neighbor's lod changes.

namespace inf::world {

// Planet-wide chunk streaming (M4): per-face quadtrees selected by
// distance, radial shells around the local surface, background meshing
// workers, cache with eviction. The manager owns CPU-side chunk data only;
// GPU upload is the renderer's business (world never depends on render).
//
// Determinism: every chunk's content is a pure function of (keys, addr) —
// worker count and scheduling order cannot affect any produced mesh
// (verified by the thread-invariance test).

struct ChunkData {
  core::ChunkAddr addr;
  ChunkMesh mesh;               // may be empty (all-air / all-solid chunk)
  TransitionMask transitions = 0;  // mask this mesh was built with
  std::uint64_t density_hash = 0;
};

struct ChunkEvent {
  enum class Kind { Ready, Evicted };
  Kind kind;
  core::ChunkAddr addr;
  // Valid for Ready events only.
  std::shared_ptr<const ChunkData> data;
};

struct ChunkManagerConfig {
  std::uint32_t worker_count = 4;
  // Split a quadtree node while (lateral size / distance) exceeds this.
  double split_factor = 2.0;
  std::uint8_t max_lod = 16;         // ~1-2 m voxels at 40-100 km radii
  std::size_t resident_budget = 3072;  // max cached chunks (headroom for
                                       // cave columns' extra shells)
  std::size_t uploads_per_update = 8;  // max Ready events delivered per tick
};

class ChunkManager {
 public:
  // The sampler is the game's terrain (density + surface elevation for a
  // body); it must be thread-safe for concurrent const calls and outlive
  // the manager.
  ChunkManager(const ChunkSampler& sampler, const ChunkManagerConfig& config);
  ~ChunkManager();

  ChunkManager(const ChunkManager&) = delete;
  ChunkManager& operator=(const ChunkManager&) = delete;

  // Recomputes the desired chunk set for a camera position (planet-local
  // meters), schedules generation, collects finished chunks, evicts
  // over-budget ones. Returns the events since the last update.
  std::vector<ChunkEvent> update(double camera_x, double camera_y, double camera_z);

  // Currently desired + ready chunk set (stable order: sorted by address).
  std::vector<std::shared_ptr<const ChunkData>> resident_chunks() const;

  // Drops every resident/in-flight chunk that intersects the ball
  // (planet-local meters) so the next update() re-samples and re-meshes it
  // through the sampler's CURRENT state. Call after mutating what the
  // sampler reads (M7: appending a player edit). Old meshes stay valid to
  // draw until their Ready replacement arrives (no hole flash). Main
  // thread only, like update().
  void invalidate_sphere(double center_x, double center_y, double center_z,
                         double radius_m);

  // Deterministic fingerprint of a set of chunk addresses' density grids —
  // used by tests and the M8 harness (independent of workers/scheduling).
  std::uint64_t scene_hash(const std::vector<core::ChunkAddr>& addrs) const;

  // Blocks until every currently scheduled chunk has been generated.
  void drain();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace inf::world
