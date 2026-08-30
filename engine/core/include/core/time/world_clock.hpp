#pragma once

#include <cstdint>
#include <memory>

#include "core/time/world_time.hpp"

namespace inf::core {

// The ONLY source of "now" (planetary-systems spec section 5). Everything
// else receives time, never asks the OS — a ci gate forbids OS-clock
// reads outside the clock module.
class WorldClock {
 public:
  virtual ~WorldClock() = default;
  virtual WorldTime now() const = 0;
};

// OS UTC -> WorldTime. The one place that touches the system clock.
class LocalClock final : public WorldClock {
 public:
  WorldTime now() const override;
};

// Fixed/scripted time for tests, replays and golden runs.
class ManualClock final : public WorldClock {
 public:
  explicit ManualClock(WorldTime start = WorldTime::epoch()) : now_(start) {}
  WorldTime now() const override { return now_; }
  void set(WorldTime t) { now_ = t; }
  void advance_ns(std::int64_t ns) { now_ = now_ + ns; }

 private:
  WorldTime now_;
};

// Server-disciplined clock (stub — full NTP-style offset estimation with
// slewing lands with the net layer). Applies a fixed offset to a base
// clock for now; the interface is what matters.
class SyncedClock final : public WorldClock {
 public:
  explicit SyncedClock(std::shared_ptr<const WorldClock> base, std::int64_t offset_ns = 0)
      : base_(std::move(base)), offset_ns_(offset_ns) {}
  WorldTime now() const override;
  void set_offset_ns(std::int64_t offset_ns) { offset_ns_ = offset_ns; }

 private:
  std::shared_ptr<const WorldClock> base_;
  std::int64_t offset_ns_;
};

}  // namespace inf::core
