#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/seed.hpp"
#include "core/time/world_time.hpp"
#include "gen/colony.hpp"

namespace inf::gen {

// T0020 WP3: the pacing census (design section 12.5) run against the
// REAL generators: sample occupied systems in the human sphere by a
// keyed, deterministic point set, resolve owners and body states at t,
// t + 1 week and t + 3 real years, and report the numbers the design
// promises. Shared by the CLI (`civ census`) and the acceptance test.

struct CivCensus {
  core::WorldTime t;
  int systems_sampled{0};
  int systems_owned{0};
  int systems_human{0};
  int systems_alien{0};
  // Owner fraction by distance from the human home, 10 bins to the
  // galaxy radius.
  int bin_systems[10]{};
  int bin_human[10]{};
  int bin_alien[10]{};
  // Bodies of human-owned systems.
  int bodies_settled{0};
  int level_hist[8]{};
  int faction_hist[static_cast<int>(FactionType::Count)]{};
  int faction_far[static_cast<int>(FactionType::Count)]{};   // outer half of the disc
  int faction_near[static_cast<int>(FactionType::Count)]{};  // inner half
  int domed{0};
  int ruined{0};
  int at_cap{0};
  int active{0};             // below cap and growth >= 0.7
  int advance1_all{0};       // >= 1 level within +3 yr
  int advance2_all{0};
  int advance3_all{0};
  int advance1_active{0};
  int advance2_active{0};
  int advance3_active{0};
  int open_air{0};           // living, not domed (the design sim's population)
  int advance1_open{0};
  int advance2_open{0};
  int advance3_open{0};
  int flip_week{0};          // level flips within +1 week
  int new_claims_week{0};    // systems unowned at t, owned at t + 1 week
  int level7{0};
  double front_ly{0.0};

  std::string report() const;
};

// max_systems bounds the work (each human-owned system costs ~1 ms for
// its bodies' climate means); the sampler stops there. radius_ly = 0
// means the galaxy radius.
CivCensus run_civ_census(const core::Seed128& seed, core::WorldTime t, int max_systems,
                         double radius_ly = 0.0);

}  // namespace inf::gen
