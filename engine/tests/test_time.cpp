#include <doctest/doctest.h>

#include "core/time/world_clock.hpp"

using inf::core::LocalClock;
using inf::core::ManualClock;
using inf::core::SyncedClock;
using inf::core::WorldTime;

TEST_CASE("world time: arithmetic, seconds, ticks") {
  const WorldTime t = WorldTime::from_ns(1'500'000'000);
  CHECK(t.seconds().to_double() == doctest::Approx(1.5));
  CHECK((t + 500'000'000).ns_since_epoch == 2'000'000'000);
  CHECK(t - WorldTime::epoch() == 1'500'000'000);

  // Tick grid (1/60 s), including negative times (pre-epoch).
  const std::int64_t tick_ns = 1'000'000'000 / 60;
  CHECK(WorldTime::epoch().tick(tick_ns) == 0);
  CHECK(WorldTime::from_ns(tick_ns).tick(tick_ns) == 1);
  CHECK(WorldTime::from_ns(-1).tick(tick_ns) == -1);
  CHECK(WorldTime::from_ns(-tick_ns - 1).tick(tick_ns) == -2);
}

TEST_CASE("clocks: manual is scriptable, synced applies an offset") {
  ManualClock manual(WorldTime::from_ns(100));
  CHECK(manual.now().ns_since_epoch == 100);
  manual.advance_ns(50);
  CHECK(manual.now().ns_since_epoch == 150);
  manual.set(WorldTime::from_seconds(2.0));
  CHECK(manual.now().ns_since_epoch == 2'000'000'000);

  auto base = std::make_shared<ManualClock>(WorldTime::from_ns(1000));
  SyncedClock synced(base, 25);
  CHECK(synced.now().ns_since_epoch == 1025);
}

TEST_CASE("clocks: local clock is sane (after 2000, before 2300)") {
  const WorldTime now = LocalClock{}.now();
  CHECK(now.ns_since_epoch > 0);                            // after Epoch Zero
  CHECK(now.seconds().to_double() < 300.0 * 365.25 * 86400.0);  // sanity bound
}
