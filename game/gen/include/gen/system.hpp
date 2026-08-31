#pragma once

#include <string>
#include <vector>

#include "core/ephem/elements.hpp"
#include "core/key.hpp"
#include "core/time/world_time.hpp"
#include "gen/planet.hpp"
#include "gen/universe.hpp"

namespace inf::gen {

// Planetary systems (T0012, design/planetary-systems.md): the Epoch-Zero
// forever-state drawn under the StarSystem node's layer keys. All values
// at global 1:10 scale (lengths /10, mu /10 => periods /10, orbital
// speeds real). Everything below is a pure function of the system entity
// key; positions at any time come from core::Ephemeris.

enum class SystemArchetype : std::uint8_t {
  SolarLike = 0,       // overweighted per Sascha's rule
  CompactMulti = 1,
  GiantDominated = 2,
  SparseBarren = 3,
  HotJupiter = 4,
  Exotic = 5,
};

const char* to_string(SystemArchetype archetype);

struct SystemMoon {
  core::PlanetPhys phys;
  core::OrbitalElements orbit;  // parent = the planet
  core::SpinState spin;
};

struct SystemPlanet {
  bool occupied{false};
  core::PlanetPhys phys;
  core::OrbitalElements orbit;  // parent = the star
  core::SpinState spin;
  // Mapping to the surface generator; valid when landable.
  bool landable{false};
  PlanetType surface_type{PlanetType::Barren};
  std::vector<SystemMoon> moons;
};

struct SystemBelt {
  det::Real inner_m, outer_m, thickness_m;
};

// Companion star (multistar/v1): a wide S-type companion on a Keplerian
// orbit around the primary, placed far beyond the outer planet so the
// planet architecture stays untouched (extension-safe layer).
struct SystemStar {
  core::StarPhys phys;
  core::OrbitalElements orbit;  // parent = the primary star
};

struct StarSystemParams {
  core::StarPhys star;          // the primary (system frame origin)
  // 0 companions = single, 1 = binary, 2 = trinary. Multiplicity is drawn
  // with roughly galactic frequencies (~2/3 single) under multistar/v1.
  std::vector<SystemStar> companions;
  SystemArchetype archetype{SystemArchetype::SolarLike};
  det::Real frost_line_m;       // disk/v1 scaffold output
  std::vector<SystemPlanet> planets;  // slot-indexed (kMaxPlanetSlots)
  std::vector<SystemBelt> belts;
};

inline constexpr int kMaxPlanetSlots = 16;

// Generates the whole system forever-state from the system ENTITY key
// (layers stellar/v1, disk/v1, architecture/v1, planets/v1, moons/v1,
// belts/v1 hang off it per the seeding contract).
StarSystemParams generate_system(const core::Key& system_entity_key);

// First landable slot (guaranteed to exist by construction), for the
// default spawn body.
int default_landable_slot(const StarSystemParams& system);

// Surface-generator parameters for one slot: type/radius/gravity/
// atmosphere come from the system layer; the remaining cosmetic draws
// come from the planet node's params key (two-seed rule).
PlanetParams planet_params_for_slot(const StarSystemParams& system, int slot,
                                    const BodyHandle& body);

// Surface-generator parameters for a MOON (T0016: moons are landable
// bodies like planets): radius/gravity/surface type from moons/v1, no
// atmosphere; cosmetic draws from the moon's own params key.
PlanetParams planet_params_for_moon(const StarSystemParams& system, int slot,
                                    int moon_index, const BodyHandle& body);

// Procedural display name for a body, pure function of its entity key
// (map-mode info card; design/map-mode.md section 3).
std::string body_display_name(const core::Key& entity_key);

// Serializable payloads (spec section 8) + the headless dump.
std::string system_to_json(const StarSystemParams& system);
std::string ephemeris_table_json(const StarSystemParams& system, core::WorldTime start,
                                 std::int64_t step_ns, int steps);

// Golden report: system params + ephemeris samples at fixed (seed, t)
// tuples via ManualClock semantics.
std::string hash_system_report();

}  // namespace inf::gen
