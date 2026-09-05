#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/key.hpp"
#include "core/time/world_time.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"

namespace inf::gen {

// Civilization vocabulary (T0020, design/civilization-and-settlements.md
// sections 2, 7, 11-15). Everything here is plain serializable data — the
// inter-stage payloads of the civ layers. Enums are gameplay-visible facts
// and therefore append-only.

// Race archetypes from lore (design section 7.1). The order is the
// registry order; never reorder.
enum class RaceType : std::uint8_t {
  Humanoid = 0,
  Insectoid = 1,
  Reptilian = 2,
  Avian = 3,
  Aquatic = 4,
  Fungoid = 5,
  Machine = 6,
  Crystalline = 7,
  Precursor = 8,  // extinct: ruins only
  Count = 9,
};

// The five universal faction behaviour classes (design section 11.1).
enum class FactionType : std::uint8_t {
  Government = 0,
  Independent = 1,
  Outlaw = 2,
  AlignedMachine = 3,
  RenegadeMachine = 4,
  Count = 5,
};

// Development levels (design section 12.1). Plain integers 0-7 in the
// data; the enum names the ladder.
enum class DevLevel : std::uint8_t {
  Uninhabited = 0,
  Founding = 1,
  IndependentSettlements = 2,
  FormingStates = 3,
  NationStates = 4,
  PlanetaryGovernment = 5,
  Metropolises = 6,
  Trantorian = 7,
};

// Settlement size classes (design section 13.3).
enum class SettlementTier : std::uint8_t {
  None = 0,
  Outpost = 1,
  Hamlet = 2,
  Village = 3,
  Town = 4,
  City = 5,
  Metropolis = 6,
  Capital = 7,
  Ecumenopolis = 8,
};

// Layout pattern families (design section 14.2).
enum class LayoutFamily : std::uint8_t {
  Organic = 0,
  Grid = 1,
  Radial = 2,
  Linear = 3,
  Hive = 4,
  Crystal = 5,
  Lattice = 6,
  Terraced = 7,
  Domed = 8,
  Count = 9,
};

const char* to_string(RaceType type);
const char* to_string(FactionType type);
const char* to_string(DevLevel level);
const char* to_string(SettlementTier tier);
const char* to_string(LayoutFamily family);
// Machine races rename their three faction types (Collective / Splinter /
// Rogue); humans use the lore names (Empire, Free settlers, ...).
const char* faction_type_label(FactionType type, RaceType race, bool human);

// Habitat preference of a race: which surface types it settles unaided,
// the temperature band, the gravity band, whether it needs air.
struct Habitat {
  PlanetType preferred[2]{PlanetType::EarthLike, PlanetType::EarthLike};
  int preferred_count{1};
  double temp_lo_k{270.0};
  double temp_hi_k{305.0};
  double gravity_lo{0.6 * 9.81};  // m/s^2
  double gravity_hi{1.4 * 9.81};
  bool needs_atmosphere{true};
  bool wants_ocean{false};   // Aquatic: coasts required, ocean worlds preferred
  bool ignores_climate{false};  // Machine: anything solid
  bool cryogenic{false};        // Crystalline: cold OR hot-dry, never temperate
};

// A point + time a race expands from: its home world, later wormhole
// exits and stranded enclave beachheads (design section 7.2 b, 9).
struct Source {
  Dir3 position_m;               // galactocentric
  core::WorldTime t_source;      // expansion starts here at this time
  double speed_scale{1.0};       // multipliers on the race values
  double settle_scale{1.0};
  double reproduction_scale{1.0};
  double r_max_ly{0.0};          // 0 = the race's r_max
  int level_cap{7};              // enclaves: stranded caps
};

struct RaceParams {
  RaceType type{RaceType::Humanoid};
  RaceType parent_type{RaceType::Humanoid};  // Machine races: their creators
  std::string name;
  std::string adjective;
  std::uint32_t variant{0};        // morphology sub-variant
  float palette[2][3]{{0.8f, 0.7f, 0.5f}, {0.3f, 0.4f, 0.6f}};
  std::uint8_t material_family{0};  // 0 stone, 1 metal, 2 resin, 3 crystal, 4 grown
  Habitat habitat;
  int tech_tier{3};      // 1-5
  int peak_level{5};     // highest level any of its planets may reach
  int home_level{5};     // the home world's fixed level
  double dome_affinity{0.4};
  int faction_count{2};
  float disposition[3]{0.5f, 0.5f, 0.5f};  // order, openness, aggression
  // Spread model (real seconds / light-years / real years).
  core::WorldTime t_0;             // founding: first interstellar colony
  double speed_ly_per_year{500.0}; // frontier velocity
  double reproduction{0.7};        // scales the level-up rate
  double settle_prob{0.5};
  double r_max_ly{1000.0};         // never larger than k_reach cell widths
  double falloff_ly{600.0};
  double anisotropy{0.3};
  bool extinct_ever{false};
  core::WorldTime t_end;           // valid when extinct_ever
  std::vector<Source> sources;     // [0] = the home world
  bool is_human{false};

  bool extinct_at(core::WorldTime t) const { return extinct_ever && t >= t_end; }
  std::string to_json() const;
};

struct FactionCentre {
  Dir3 position_m;
  double weight{1.0};
};

struct FactionParams {
  FactionType type{FactionType::Government};
  std::string name;
  std::uint32_t emblem_seed{0};
  float accent[3]{0.8f, 0.8f, 0.8f};
  core::WorldTime t_start;
  bool hostile{false};
  double speed_mul{1.0};
  double reproduction_mul{1.0};
  double settle_mul{1.0};
  double dome_mul{1.0};
  std::vector<FactionCentre> centres;

  std::string to_json() const;
};

// The per-body civilization state (design section 10.3).
struct CivState {
  bool settled{false};
  std::uint32_t race_index{0};   // index into the candidate block; see colony.hpp
  core::Key race_key;
  int faction_index{-1};
  FactionType faction_type{FactionType::Government};
  int level{0};
  int max_level{1};
  bool ruined{false};
  bool domed{false};
  bool is_home{false};
  core::WorldTime settled_at;
  double age_s{0.0};
  double growth{1.0};
  double suitability{0.0};
  double progress{0.0};  // position inside the current level, [0, 1)

  std::string to_json() const;
};

// Everything a building's geometry is a function of (design section 15).
struct StyleVector {
  RaceType race_type{RaceType::Humanoid};
  std::uint32_t race_variant{0};
  float palette[2][3]{{0.8f, 0.7f, 0.5f}, {0.3f, 0.4f, 0.6f}};
  std::uint8_t material_family{0};
  FactionType faction_type{FactionType::Government};
  int tech_tier{3};
  SettlementTier tier{SettlementTier::Hamlet};
  int level{1};
  bool ruined{false};
  bool domed{false};
  float regularity{0.5f};
  float wear{0.2f};
  float ornament{0.3f};
  float light_density{0.5f};
  float construction{1.0f};  // 1 = finished
};

enum class LotUsage : std::uint8_t {
  Residential = 0,
  Civic = 1,
  Industrial = 2,
  Agricultural = 3,
  Pad = 4,      // landing pad
  Monument = 5,
  Count = 6,
};

const char* to_string(LotUsage usage);

// One buildable lot in a site (site-local metres, convex footprint).
struct Lot {
  std::uint32_t id{0};
  std::uint32_t order{0};       // reveal order inside the tier ring
  std::uint8_t tier{1};         // the ring that created it
  std::uint8_t vertex_count{4};
  float footprint[8][2]{};      // convex polygon, <= 8 vertices, metres
  float datum_m{0.0f};          // elevation relative to the site datum
  float height_budget_m{8.0f};
  LotUsage usage{LotUsage::Residential};
  StyleVector style;
};

}  // namespace inf::gen
