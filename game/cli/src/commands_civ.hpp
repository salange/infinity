#pragma once

#include "core/seed.hpp"

namespace inf::cli {

// T0020: civilization inspection. All commands take the home galaxy of
// the seed; positions are galactocentric light-years, "here" = the home
// system.
int cmd_civ_races(const core::Seed128& seed, const double* at_ly, bool all = false);
int cmd_civ_enclaves(const core::Seed128& seed);
// Owner + per-body state of one system at a time (--time: ISO date
// "YYYY-MM-DD", or "+N" real years after the launch reference).
int cmd_civ_state(const core::Seed128& seed, const long long* cell_xyzl, const char* time_text);
// The pacing census over the human sphere.
// min_level > 0 lists only the human systems whose best body reached it
// (--level; finds ecumenopolis worlds).
int cmd_civ_census(const core::Seed128& seed, int max_systems, const char* time_text, int min_level = 0);
int cmd_civ_map(const core::Seed128& seed, const long long* cell_xyzl, int slot, int moon,
                const char* time_text, const char* out_path);
// at_m: planet-local metres of a point; picks the site under it (or the
// nearest) and reports what the app would build around that point.
int cmd_civ_site(const core::Seed128& seed, const long long* cell_xyzl, int slot, int moon,
                 const char* tier_text, int site_index, const char* time_text, const char* out_path,
                 const double* at_m = nullptr);
int cmd_hash_civ();

}  // namespace inf::cli
