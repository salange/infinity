#pragma once

#include "core/det/real.hpp"
#include "gen/geo.hpp"

namespace inf::gen {

// A post-terrain height modifier (T0020): the ONE place a later layer
// may feed back into geometry. It sits after erosion, drainage and
// features in the elevation chain and never influences the layers it
// reads. The terrain knows only this interface — it never includes the
// civilization headers (ci grep gate).
//
// Thread safety: the terrain is sampled from worker threads; an
// implementation must be immutable after construction.
class HeightModifier {
 public:
  virtual ~HeightModifier() = default;

  // The modified elevation at a direction. `base_m` is the elevation the
  // terrain computed; `base_at` evaluates the unmodified terrain at any
  // other direction (road centrelines, terrace references) — never the
  // modified one, so there is no recursion.
  struct BaseEval {
    virtual ~BaseEval() = default;
    virtual det::Real elevation_m(const Dir3& unit_dir) const = 0;
  };
  virtual det::Real modify(const Dir3& unit_dir, det::Real base_m, const BaseEval& base_at) const = 0;

  // Urban surface hint for the material classifier: paving/plating weight
  // (0..1) and the material family (0 stone paving, 1 metal plating, 2
  // resin, 3 crystal, 4 grown) at a direction; 0 outside every site.
  struct Urban {
    double weight{0.0};
    int family{0};
    double night_light{0.0};  // 0..1 emissive mask for the far-view bake
  };
  virtual Urban urban(const Dir3& unit_dir) const = 0;

  // True when the direction lies inside some site's or road's bound —
  // the cheap reject the sampler asks before anything else.
  virtual bool near(const Dir3& unit_dir) const = 0;
};

}  // namespace inf::gen
