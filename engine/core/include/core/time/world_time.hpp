#pragma once

#include <cstdint>

#include "core/det/real.hpp"

namespace inf::core {

// WorldTime (planetary-systems spec section 5): int64 nanoseconds since
// Epoch Zero = 2000-01-01T00:00:00 UTC, on a UNIFORM timescale — a
// straight count of SI seconds, no leap seconds, no calendar. Civil/UTC
// display is a UI-layer conversion only. Spans roughly +-292 years.
// Everything that could be time-dependent takes a WorldTime, needed or
// not; nothing outside the clock module reads the OS clock.
struct WorldTime {
  std::int64_t ns_since_epoch{0};

  friend bool operator==(const WorldTime&, const WorldTime&) = default;
  friend auto operator<=>(const WorldTime&, const WorldTime&) = default;

  static constexpr WorldTime epoch() { return WorldTime{0}; }
  static constexpr WorldTime from_ns(std::int64_t ns) { return WorldTime{ns}; }
  static constexpr WorldTime from_seconds(double s) {
    return WorldTime{static_cast<std::int64_t>(s * 1e9)};
  }

  // Seconds since Epoch Zero as a controlled real (deterministic; ~us
  // resolution at century range — fine for ephemerides, same rounding on
  // every machine).
  det::Real seconds() const {
    return det::Real(static_cast<double>(ns_since_epoch) * 1e-9);
  }

  constexpr WorldTime operator+(std::int64_t ns) const {
    return WorldTime{ns_since_epoch + ns};
  }
  constexpr std::int64_t operator-(const WorldTime& other) const {
    return ns_since_epoch - other.ns_since_epoch;
  }

  // Gameplay tick index on a fixed grid (tick-keyed draws, seeding spec
  // section 3). tick_ns must be positive.
  constexpr std::int64_t tick(std::int64_t tick_ns) const {
    const std::int64_t q = ns_since_epoch / tick_ns;
    return (ns_since_epoch % tick_ns < 0) ? q - 1 : q;
  }
};

// Unix epoch offset: 2000-01-01T00:00:00 UTC in Unix seconds.
inline constexpr std::int64_t kEpochZeroUnixSeconds = 946'684'800;

}  // namespace inf::core
