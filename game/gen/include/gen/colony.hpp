#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/ephem/elements.hpp"
#include "core/key.hpp"
#include "core/seed.hpp"
#include "core/time/world_time.hpp"
#include "gen/civ_types.hpp"
#include "gen/civilization.hpp"
#include "gen/life.hpp"
#include "gen/system.hpp"

namespace inf::gen {

// T0020 WP3: system ownership and the per-body civilization state
// (design sections 8, 10, 11.3, 12).
//
//   race-claims/v1 (K_race, system cell)  the wave/infill claim draws
//   colony/v1      (K_body)               settle draw, dome, delay, flip,
//                                         abandonment, growth
//   max-level/v1   (K_body)               local wealth cap (versioned so
//                                         neighbours/trade can enter later)
//
// Time enters ONLY as closed-form functions of WorldTime: arrival
// instants, infill instants, step thresholds. No tick-keyed draws, no
// accumulated state, no hysteresis. Two clients agree bit-exactly given
// synced clocks; near a flip they disagree for the clock error.

// Level thresholds in REAL years (design section 12.2): the age at which
// a colony with rate 1 reaches level k. Index = level.
inline constexpr double kLevelThresholdYears[8] = {0.0, 0.0, 0.4, 1.2, 2.5, 4.5, 7.5, 12.0};
inline constexpr double kInfillTauYears = 10.0;
inline constexpr double kDomedRateAboveThree = 0.1;

// --- the spread model (design section 8) ------------------------------------

struct Claim {
  bool reached{false};
  bool claimed{false};
  bool home{false};          // the race's own cradle: beats every other claim
  core::WorldTime t_arrive;  // the front's arrival (jittered)
  core::WorldTime t_claim;   // the settlement instant (wave or infill)
  int source_index{0};       // which source the claim came through
  double p_wave{0.0};
  double d_eff_ly{0.0};
};

// star_factor reads stellar/v1 only — claim resolution never generates
// planets (design section 8).
double star_factor(const core::StarPhys& star);

// A race's claim on a system at time t. The race's home system is always
// claimed at t_0 (a race owns its cradle by rule).
Claim race_claim(const Race& race, const SystemCell& cell, const Dir3& system_pos_m,
                 const core::StarPhys& star, core::WorldTime t);

struct Owner {
  bool owned{false};
  std::size_t candidate{0};  // index into the candidate block
  core::Key race_key;
  core::WorldTime t_claim;
  int source_index{0};
};

// --- system context ---------------------------------------------------------

// Everything the body layer needs about one body, gathered once by the
// caller from planets/v1 + climate/v1 (means) + life/v1 + stellar/v1.
struct BodyCivInputs {
  int slot{0};
  int moon{-1};                 // -1: the planet itself
  core::Key body_entity;
  PlanetType type{PlanetType::Barren};
  bool solid{true};             // false for giants and sub-Neptunes
  bool is_race_home{false};     // planets/v1 marked this slot a race home
  double radius_m{0.0};
  double gravity{9.81};
  bool has_atmosphere{false};
  double mean_temperature_k{250.0};
  double land_fraction{1.0};
  LifeStage life_stage{LifeStage::Sterile};
  bool life_occupied{false};
};

struct SystemCivContext {
  SystemCell cell;
  core::Key system_key;
  Dir3 position_m;
  core::StarPhys star;
  double metallicity{0.0};
  std::vector<BodyCivInputs> bodies;
};

// Gather a system's civ inputs from the generators (the expensive part:
// planet params + climate means per body). generate_system is called
// with the race-home override when the registry says so.
SystemCivContext gather_system_context(const core::Seed128& seed, const RaceRegistry& registry,
                                       const SystemCell& cell, bool include_moons = true);

// --- resolution ---------------------------------------------------------------

class ColonyResolver {
 public:
  explicit ColonyResolver(const RaceRegistry& registry) : registry_(registry) {}

  // The earliest-claiming candidate owns the system (ties by race key).
  Owner owner(const SystemCell& cell, const Dir3& system_pos_m, const core::StarPhys& star,
              core::WorldTime t) const;
  Owner owner(const SystemCivContext& context, core::WorldTime t) const {
    return owner(context.cell, context.position_m, context.star, t);
  }

  // Per-body states of an owned system at t; unowned systems give every
  // body settled = false. Applies the best-body rule (the most suitable
  // solid body of an owned system is always settled, domed if needed).
  std::vector<CivState> system_states(const SystemCivContext& context, const Owner& owner,
                                      core::WorldTime t) const;
  // One body (the same function; the best-body rule needs the system, so
  // pass is_best explicitly).
  CivState body_state(const SystemCivContext& context, const BodyCivInputs& body,
                      const Owner& owner, bool is_best, core::WorldTime t) const;

  // The next instant at which this body's state changes (settlement,
  // a level flip, extinction, abandonment); t itself when nothing is
  // pending (unowned, at cap with nothing scheduled).
  core::WorldTime next_change(const SystemCivContext& context, const BodyCivInputs& body,
                              const Owner& owner, bool is_best, core::WorldTime t) const;

  const RaceRegistry& registry() const { return registry_; }
  // The candidate block the owner resolution used (aliens + humans).
  const std::vector<Race>& candidates(const Dir3& system_pos_m) const {
    return registry_.candidates_around(system_pos_m);
  }

 private:
  const RaceRegistry& registry_;
};

// Suitability of a body for a race (0..1, design section 10.3) — the
// organic value; the dome option is applied by the resolver.
double suitability(const RaceParams& race, const BodyCivInputs& body);
// The dome-path suitability of a body (0 for giants, > 700 K, no solid
// surface).
double dome_suitability(const RaceParams& race, const BodyCivInputs& body);

// The level ladder as a function of age (real seconds), rate and domed
// flag; capped by the caller.
int level_for_age(double age_s, double rate, bool domed);
// Time (real seconds) at which level k is reached under rate/domed.
double time_to_level(int level, double rate, bool domed);

}  // namespace inf::gen
