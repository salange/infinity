#pragma once
// The architecture registry: one Architecture per (race, faction) turns
// a lot of sites/v1 into geometry. The city system is laid out to grow
// per race and faction — its own logic, assets and, later, renderers —
// so everything above the shared generators goes through this
// interface. Today one architecture exists (human, tech) and stands in
// for every other race and faction; a key building (government,
// unification ring, landing pad, monument) is a slot a loaded asset can
// fill later without touching the lot pipeline.
#include <cstdint>
#include <vector>

#include "city/math.hpp"
#include "city/rng.hpp"
#include "city/scene.hpp"
#include "gen/civ_types.hpp"

namespace inf::city {

// A lot in the scene frame (x east, y up, z south; metres from the site
// centre on the datum), ready for an architecture.
struct LotInput {
  std::uint32_t id{0};
  std::vector<Vec2> footprint;  // convex, counter-clockwise (plan_area > 0)
  Vec2 centre;
  float rotation{0.0f};         // of the footprint's first edge
  float ground_y{0.0f};         // civil-modified ground under the lot
  float height_budget{8.0f};
  float t{0.5f};                // 0 centre … 1 edge of the site
  std::uint8_t tier{1};         // the ring that created the lot
  gen::LotUsage usage{gen::LotUsage::Residential};
  gen::StyleVector style;
};

enum class KeyRole : std::uint8_t { Government, UnificationRing, LandingPad, Monument };

struct LotBuildResult {
  bool tower{false};
  bool built{false};
};

class Architecture {
 public:
  virtual ~Architecture() = default;
  virtual const char* name() const = 0;
  // The material table this architecture's geometry indexes.
  virtual std::vector<MaterialDesc> materials() const = 0;
  // One lot. detail: 2 full, 1 near context, 0 far context.
  virtual LotBuildResult build_lot(Scene& sc, const LotInput& lot, Rng rng, int detail) const = 0;
  // A key building at `centre` facing `rot`, `half` metres of half extent,
  // ground at `y`.
  virtual void build_key(Scene& sc, KeyRole role, Vec2 centre, float rot, float half, float y, Rng rng,
                         int detail) const = 0;
};

// The architecture for a style: by race type and faction type, with the
// human tech architecture as the placeholder for every pair without one.
const Architecture& architecture_for(const gen::StyleVector& style);

}  // namespace inf::city
