#include "core/time/world_clock.hpp"

#include <chrono>

namespace inf::core {

WorldTime LocalClock::now() const {
  // The single sanctioned OS-clock read in the codebase (ci-enforced).
  // C++20: system_clock counts Unix time (no leap seconds) — matching
  // WorldTime's uniform-timescale convention.
  const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  return WorldTime{unix_ns - kEpochZeroUnixSeconds * 1'000'000'000LL};
}

WorldTime SyncedClock::now() const { return base_->now() + offset_ns_; }

}  // namespace inf::core
