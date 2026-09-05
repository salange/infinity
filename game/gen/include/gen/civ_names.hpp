#pragma once

#include <cstdint>
#include <string>

#include "core/key.hpp"
#include "gen/civ_types.hpp"

namespace inf::gen {

// civ-names/v1 (design section 17): deterministic, cosmetic names from
// per-race-type phonology tables — Insectoid clicks and apostrophes,
// Crystalline hard clusters, Machine designations with numerals,
// Humanoid Latinate/Germanic blends. Every generated syllable sequence
// runs through a blocklist; a hit re-draws with the next draw index, so
// the output stays a pure function of the key.
//
// Draws are DRAW_POINT under derive_named(K, "civ-names/v1") with the
// purpose in x and the attempt in y — never a shared sequential stream.

// A race's proper name ("Vashkari") and its adjective ("Vashkar").
void race_names(const core::Key& race_key, RaceType type, std::string* name,
                std::string* adjective);
// A faction's name from its type pattern ("Vashkar Hegemony", "Free Kessa
// Belt", "Rogue Cluster 7").
std::string faction_name(const core::Key& faction_key, RaceType race, FactionType type,
                         const std::string& race_adjective, bool human);
// Settlement names from the race table; capitals get an honorific pattern.
std::string settlement_name(const core::Key& site_key, RaceType race, bool capital);

// True when the candidate contains a blocked sequence (tests the filter).
bool name_blocked(const std::string& candidate);

}  // namespace inf::gen
