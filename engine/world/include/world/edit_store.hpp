#pragma once

// Player-diff tier of the world state (CLAUDE.md contract 3: persistence
// is a diff, never a save). Edits are CSG operations applied on top of
// the procedural density BEFORE meshing/queries:
//   effective(p) = fold over ops (op_id order):
//     subtract sphere: min(d, |p - c| - r)   (carves air)
//     add sphere:      max(d, r - |p - c|)   (fills solid)
// Deterministic by construction: fixed op order, basic IEEE ops only.
//
// The store is an abstract interface (pluggable — representations are an
// explicit experimentation surface); the CSG op list is the first
// implementation. This tier is DISTINCT from the inception overlay
// (core/tree) — they share address-keyed plumbing, never storage.

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/det/fixed64.hpp"

namespace inf::world {

struct SphereEdit {
  std::uint64_t op_id{0};       // global application order
  // Planet-local position/radius in det::fixed64 raw (bit-exact identity;
  // serialization round-trips exactly).
  std::int64_t center_raw[3]{0, 0, 0};
  std::int64_t radius_raw{0};
  bool subtract{true};          // true = dig, false = add material
  std::uint32_t material{0};

  det::Fixed64 center(int axis) const { return det::Fixed64::from_raw(center_raw[axis]); }
  det::Fixed64 radius() const { return det::Fixed64::from_raw(radius_raw); }

  friend bool operator==(const SphereEdit&, const SphereEdit&) = default;
};

class EditStore {
 public:
  virtual ~EditStore() = default;

  virtual std::uint64_t append(const SphereEdit& edit) = 0;  // returns op_id
  virtual std::size_t size() const = 0;

  // All ops whose sphere intersects the ball (center, radius), in op_id
  // order. Meshing/queries fold exactly these.
  virtual std::vector<SphereEdit> overlapping(const det::Fixed64 center[3],
                                              det::Fixed64 radius) const = 0;

  // Trivial v0 persistence (binary, versioned header).
  virtual bool save(const std::string& path) const = 0;
  virtual bool load(const std::string& path) = 0;
};

// First implementation: flat CSG op list with linear overlap queries
// (compact, replayable, mergeable). Chunk-bucketed variants are the known
// alternates behind the same interface. Thread-safe: meshing workers query
// while the main thread appends.
class CsgEditStore final : public EditStore {
 public:
  std::uint64_t append(const SphereEdit& edit) override;
  std::size_t size() const override;
  std::vector<SphereEdit> overlapping(const det::Fixed64 center[3],
                                      det::Fixed64 radius) const override;
  bool save(const std::string& path) const override;
  bool load(const std::string& path) override;

  // Unsynchronized view (tests / single-threaded tools only).
  const std::vector<SphereEdit>& edits() const { return edits_; }

 private:
  mutable std::shared_mutex mutex_;
  std::vector<SphereEdit> edits_;
  std::uint64_t next_op_id_ = 1;
};

// Folds the overlapping ops into a base density (meters-ish signed field,
// positive = solid) at a point given in planet-local meters (doubles fed
// from the deterministic sampling paths).
double apply_edits(double base_density, const std::vector<SphereEdit>& edits, double px,
                   double py, double pz);

}  // namespace inf::world
