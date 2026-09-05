#pragma once

#include <cstdint>

#include "core/time/world_time.hpp"

namespace inf::gen {

// The civilization clock (design/civilization-and-settlements.md section 3).
// No new clock: everything takes a WorldTime. Two units —
//  - REAL time: seconds of WorldTime. Every pacing constant in the
//    civilization layers is specified in real seconds, because the
//    requirements are about what a player sees in a real week or year.
//  - GAME-years: the lore/display unit, an Earth-analogue orbital period
//    under the accepted 1:80 period scale (design/scales-and-distances.md
//    section 2). Display only; nothing is paced in game-years.
//
// These live in the game (not engine core) because the 1:80 rate is a
// game scale, and the engine may not know game scales.
inline constexpr double kRealYearS = 365.25 * 86400.0;
inline constexpr double kRealWeekS = 7.0 * 86400.0;
inline constexpr double kGameYearS = kRealYearS / 80.0;  // 394 470 s = 4.566 real days

// Humanity's expansion start ("t_0_human"): ONE dated constant, 8 real
// years before launch — provisionally 2018-09-01T00:00:00 UTC, so that on
// the reference launch date the expansion is 8.0 years old. Set once at
// launch; every alien race's founding time is drawn relative to the same
// launch reference. WorldTime counts SI seconds from Epoch Zero
// (2000-01-01T00:00:00 UTC); 2018-09-01 is 589 075 200 s later.
inline constexpr core::WorldTime kHumanExpansionStart =
    core::WorldTime::from_ns(589'075'200LL * 1'000'000'000LL);
inline constexpr double kHumanExpansionAgeAtLaunchYears = 8.0;
// The launch reference: the "now" the design's numbers are calibrated
// against (2026-08-31). Tests use ManualClock offsets from this.
inline constexpr core::WorldTime kLaunchReference = core::WorldTime::from_ns(
    kHumanExpansionStart.ns_since_epoch +
    static_cast<std::int64_t>(kHumanExpansionAgeAtLaunchYears * kRealYearS) * 1'000'000'000LL);

inline constexpr std::int64_t real_years_to_ns(double years) {
  return static_cast<std::int64_t>(years * kRealYearS * 1e9);
}
inline constexpr double ns_to_real_years(std::int64_t ns) {
  return static_cast<double>(ns) / 1e9 / kRealYearS;
}
inline constexpr double ns_to_real_seconds(std::int64_t ns) {
  return static_cast<double>(ns) / 1e9;
}

// Lore time: game-years since Epoch Zero and back. Cosmetic conversions;
// the pacing math never goes through them.
inline double game_years_since_epoch(core::WorldTime t) {
  return static_cast<double>(t.ns_since_epoch) / 1e9 / kGameYearS;
}
inline core::WorldTime world_time_from_game_years(double game_years) {
  return core::WorldTime::from_ns(static_cast<std::int64_t>(game_years * kGameYearS * 1e9));
}
inline double game_years_between(core::WorldTime from, core::WorldTime to) {
  return static_cast<double>(to - from) / 1e9 / kGameYearS;
}

}  // namespace inf::gen
