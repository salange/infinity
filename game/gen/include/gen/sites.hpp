#pragma once

#include <cstdint>
#include <vector>

#include "core/key.hpp"
#include "gen/civ_types.hpp"
#include "gen/civilization.hpp"
#include "gen/settlements.hpp"
#include "gen/terrain.hpp"

namespace inf::gen {

// T0020 WP5: sites/v1 (design section 14). One settlement per settled
// province: a centre and datum picked from 16 keyed candidates, a
// site-local east/north frame, a layout family, arterials, and LOTS —
// which are never stored: a lot is a pure function of (site key, block
// coordinates, lot index). Growth is additive by tier (a tier-n site is
// tier n-1 plus a ring; every lot inside the inner rings is bit-identical
// whatever the outer tier) and continuous inside a tier (each lot has a
// keyed reveal value; it exists once the site's progress passes it, and
// is under construction for the last 3 % before).
//
// Sites are bounded feature entities (centre + radius); a query outside
// every bound pays one province lookup.

struct SiteFrame {
  Dir3 up;     // unit direction of the centre
  Dir3 east;
  Dir3 north;
  double radius_m{0.0};  // planet radius (the frame's sphere)
  // Orthographic site-local metres <-> directions (exact inverses on the
  // sphere for the small angles a site spans).
  void to_local(const Dir3& dir, double* x, double* y) const;
  Dir3 to_dir(double x, double y) const;
};

struct Arterial {
  std::vector<float> xy;  // polyline, site-local metres, pairs
  float width_m{10.0f};
};

struct Site {
  bool valid{false};
  std::uint32_t province{0};
  CellId cell;
  core::Key key;                 // derive_child(K_sites, kind::Site, face, ci, cj)
  SiteFrame frame;
  double datum_m{0.0};           // elevation of the plateau above the nominal radius
  double sea_m{0.0};
  int tier{0};                   // current tier index (1..7)
  int max_tier{7};
  double radius_m{0.0};          // radius of the current tier
  float progress{0.0f};          // inside the current tier, [0, 1)
  LayoutFamily family{LayoutFamily::Organic};
  double axis_rad{0.0};          // layout rotation (grid/linear axis)
  bool coastal{false};
  bool river{false};
  bool ruined{false};
  bool domed{false};
  bool capital{false};
  double block_m{80.0};          // block pitch of the square lattice
  double street_m{10.0};
  double lot_m{18.0};            // typical lot edge
  double density{0.8};           // fraction of lattice cells that host a lot
  StyleVector style;             // race/faction/tier style base
  std::vector<Arterial> arterials;
  float light_density{0.5f};

  // Ring index (1..7) of a site-local point, by distance from the
  // centre: which tier created it.
  static int ring_of(double dist_m);
};

// Radius of ring t (= tier radius, design 13.3).
double ring_radius_m(int tier);

class SiteField {
 public:
  // Sites for every settled province of the plan, computed once (the
  // centre search costs 16 elevation evaluations per site). Immutable
  // afterwards — safe to read from every worker thread.
  SiteField(const core::Key& body_entity_key, const TerrainField& field,
            const SettlementPlan& plan, const RaceParams& race,
            const std::vector<FactionParams>& factions, const CivState& state);

  const std::vector<Site>& sites() const { return sites_; }
  // The site of a province (nullptr when unsettled), by cell.
  const Site* site_of(const CellId& cell) const;
  std::uint32_t cells_per_face() const { return n_; }
  const SettlementPlan& plan() const { return plan_; }

  // Lots of one block of the site's square lattice, at the site's
  // current tier and progress: every lot inside the current radius, with
  // style.construction < 1 for the newest 3 %. Blocks are B x B metres,
  // block (bx, by) spanning [bx B, (bx+1) B) x [by B, (by+1) B) in the
  // site frame. Pure function; identical from every caller.
  void lots_in_block(const Site& site, int bx, int by, std::vector<Lot>* out) const;
  // All lots inside the current radius (the whole site; tests, dumps,
  // small sites). Ordered by block then lot index.
  void all_lots(const Site& site, std::vector<Lot>* out) const;
  // Number of lots that would be visible at another progress value —
  // the reveal is a per-lot threshold, so this is the count of lots with
  // reveal < progress in the current ring plus every inner-ring lot.
  std::uint32_t visible_count(const Site& site, float progress) const;

  // Plateau/terrace target elevation for a site-local point (the
  // civil/v1 datum): the site datum, or a terrace step for the Terraced
  // family (steps of kTerraceStepM along the site's downhill axis).
  double plateau_m(const Site& site, double x, double y) const;
  static constexpr double kTerraceStepM = 6.0;

 private:
  Site build_site(const ProvinceSite& province, const SettlementPlan& plan,
                  const RaceParams& race, const std::vector<FactionParams>& factions,
                  const CivState& state) const;
  void build_arterials(Site* site) const;
  // Whether a lattice cell centre lies on a street of the family (masked).
  bool on_street(const Site& site, double x, double y) const;

  core::Key sites_key_;
  const TerrainField& field_;
  SettlementPlan plan_;
  std::uint32_t n_{0};
  double radius_m_{0.0};
  std::vector<Site> sites_;
  std::vector<std::int32_t> site_index_;  // per province, -1 = none
};

}  // namespace inf::gen
