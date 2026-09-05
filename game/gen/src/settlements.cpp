#include "gen/settlements.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/det/trig.hpp"
#include "gen/civilization.hpp"
#include "gen/names.hpp"

namespace inf::gen {

using civ::u01;
using det::Real;

namespace {

double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double arc_between(const Dir3& a, const Dir3& b) {
  const double c = clamp01(0.5 * std::sqrt(chord_sq(a, b).to_double()));
  // arc = 2 asin(chord/2); asin via atan2 on the deterministic kernels.
  const double s = c;
  const double co = std::sqrt(1.0 - s * s);
  return 2.0 * det::atan2(Real(s), Real(co)).to_double();
}

// Fraction table of suitable provinces settled at each level (design
// 13.2). Level 1 is "1-3 sites", handled by count; level 7 is everything.
constexpr double kFractionTable[9] = {0.0, 0.0, 0.05, 0.22, 0.80, 0.90, 0.95, 1.0, 1.0};

bool is_organic(RaceType type) {
  return type != RaceType::Machine && type != RaceType::Crystalline &&
         type != RaceType::Precursor;
}

}  // namespace

double tier_radius_m(SettlementTier tier) {
  switch (tier) {
    case SettlementTier::None: return 0.0;
    case SettlementTier::Outpost: return 120.0;
    case SettlementTier::Hamlet: return 200.0;
    case SettlementTier::Village: return 400.0;
    case SettlementTier::Town: return 1000.0;
    case SettlementTier::City: return 3000.0;
    case SettlementTier::Metropolis: return 8000.0;
    case SettlementTier::Capital: return 12000.0;
    case SettlementTier::Ecumenopolis: return 0.0;
  }
  return 0.0;
}

double settled_fraction(int level, double progress) {
  if (level <= 0) return 0.0;
  if (level >= 8) return 1.0;
  const double a = kFractionTable[level];
  const double b = kFractionTable[level + 1];
  return a + (b - a) * clamp01(progress);
}

SettlementPlanner::SettlementPlanner(const core::Key& body_entity_key, const TerrainField& field,
                                     const RaceParams& race, bool domed)
    : key_(core::derive_named(body_entity_key, name::SettlementsV1)),
      field_(field),
      race_(race),
      domed_(domed),
      n_(field.provinces().cells_per_face()),
      radius_m_(field.planet().radius_m.to_double()) {
  const PlanetParams& planet = field.planet();
  const ProvinceField& provinces = field.provinces();
  const MacroField& macro = field.macro();
  const ClimateField& climate = field.climate();
  const DrainageField& drainage = field.drainage();
  const bool has_sea = planet.land_fraction.to_double() < 0.999;
  const double sea = planet.sea_level_m.to_double();
  const double macro_amp = planet.macro_amplitude_m.to_double();
  const Habitat& habitat = race.habitat;
  const double cell_angle = 2.0 / static_cast<double>(n_);  // uv extent of a cell
  base_.resize(static_cast<std::size_t>(6) * n_ * n_);
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t ci = 0; ci < n_; ++ci) {
      for (std::uint32_t cj = 0; cj < n_; ++cj) {
        const CellId cell{face, ci, cj};
        const std::uint32_t index = (static_cast<std::uint32_t>(face) * n_ + ci) * n_ + cj;
        ProvinceSite& site = base_[index];
        site.cell = cell;
        site.index = index;
        site.centre = provinces.representative(cell);
        const ProvinceParams params = provinces.cell_params(cell);
        // Land/sea from a 5-point stencil of the macro lattice (centre +
        // 4 offsets at +-0.3 cell) plus the province base elevation.
        int above = 0;
        int samples = 0;
        double elevation_centre = 0.0;
        const FaceUV centre_uv = dir_to_face_uv(site.centre);
        for (int k = 0; k < 5; ++k) {
          const double du = k == 1 ? 0.3 : (k == 2 ? -0.3 : 0.0);
          const double dv = k == 3 ? 0.3 : (k == 4 ? -0.3 : 0.0);
          FaceUV uv = centre_uv;
          uv.u = Real(std::clamp(uv.u.to_double() + du * cell_angle, -1.0, 1.0));
          uv.v = Real(std::clamp(uv.v.to_double() + dv * cell_angle, -1.0, 1.0));
          const double elevation =
              macro.canonical_value(uv).to_double() * macro_amp + params.base_elevation_m.to_double();
          if (k == 0) elevation_centre = elevation;
          above += elevation > sea ? 1 : 0;
          ++samples;
        }
        const double land_fraction = static_cast<double>(above) / samples;
        site.ocean = has_sea && above == 0;
        site.coastal = has_sea && above > 0 && above < samples;
        site.altitude_m = static_cast<float>(elevation_centre - sea);
        // Flatness from the province's relief and ruggedness: a 3 km
        // Alpine province is not town country.
        const double relief = params.relief_amplitude_m.to_double() * (0.5 + params.ruggedness.to_double());
        site.flatness = static_cast<float>(1.0 / (1.0 + relief / 500.0));
        // Rivers: the drainage forest is built on the province
        // representatives, so the province's own vertex says it all.
        if (drainage.enabled() && index < drainage.vertices().size()) {
          const DrainageField::Vertex& v = drainage.vertices()[index];
          site.river = !v.sea && v.parent >= 0 && v.order >= 2;
        }
        // Climate at the representative.
        const Climate c = climate.sample(site.centre, elevation_centre > 0.0 ? elevation_centre : 0.0,
                                         elevation_centre - sea);
        // --- suitability -------------------------------------------------
        double s = 0.0;
        if (site.ocean) {
          s = 0.0;  // below level 7 nothing lives at sea
        } else if (domed_ || habitat.ignores_climate) {
          // Domed colonies and machines: flat ground, subsurface ice for
          // organics (cold provinces), climate otherwise ignored.
          s = 0.3 + 0.7 * site.flatness;
          if (domed_ && is_organic(race.type) && c.temperature_k < 273.0) s *= 1.15;
        } else {
          double temp;
          if (habitat.cryogenic) {
            temp = c.temperature_k < 200.0 ? 1.0 : clamp01((320.0 - c.temperature_k) / 120.0) * 0.3;
          } else {
            const double lo = habitat.temp_lo_k - 25.0;
            const double hi = habitat.temp_hi_k + 25.0;
            temp = c.temperature_k >= lo && c.temperature_k <= hi
                       ? 1.0
                       : clamp01(1.0 - (c.temperature_k < lo ? lo - c.temperature_k : c.temperature_k - hi) / 45.0);
          }
          const double wet = 0.35 + 0.65 * clamp01(c.humidity * 1.5);
          const double flat = 0.25 + 0.75 * site.flatness;
          const double low = 1.0 / (1.0 + std::max(0.0, elevation_centre - sea) / 2500.0);
          s = temp * wet * flat * low * (0.6 + 0.4 * land_fraction);
          if (site.coastal) s *= 1.25;
          if (site.river) s *= 1.3;
          switch (race.type) {
            case RaceType::Aquatic: s *= site.coastal ? 1.5 : 0.25; break;
            case RaceType::Avian: s *= 0.5 + 0.8 * (1.0 - site.flatness); break;
            case RaceType::Fungoid: s *= 0.7 + 0.5 * clamp01(c.humidity * 1.5); break;
            case RaceType::Reptilian:
            case RaceType::Insectoid: s *= 0.8 + 0.4 * clamp01((c.temperature_k - 280.0) / 40.0); break;
            default: break;
          }
        }
        site.suitability = static_cast<float>(clamp01(s));
        site.suitable = site.suitability > 0.02f;
        const auto d = core::draw_point(key_, channel::Params, face, ci, cj);
        site.score = static_cast<float>(site.suitability * (0.6 + 0.4 * u01(d[0])));
        site.growth = static_cast<float>(det::fast_exp(Real(0.2 * civ::normal01(d[1], d[2]))).to_double());
      }
    }
  }
  // Ranking over suitable provinces: score descending, index ascending
  // (a total order — never changes with t).
  for (const ProvinceSite& site : base_) {
    if (site.suitable) by_rank_.push_back(site.index);
  }
  std::sort(by_rank_.begin(), by_rank_.end(), [&](std::uint32_t a, std::uint32_t b) {
    if (base_[a].score != base_[b].score) return base_[a].score > base_[b].score;
    return a < b;
  });
  for (std::uint32_t r = 0; r < by_rank_.size(); ++r) {
    base_[by_rank_[r]].rank = r;
  }
  suitable_count_ = static_cast<std::uint32_t>(by_rank_.size());
  // Max tier per site: tier by rank one level below the cap, plus keyed
  // scatter — local wealth (design 13.3).
  for (std::uint32_t r = 0; r < by_rank_.size(); ++r) {
    ProvinceSite& site = base_[by_rank_[r]];
    const auto d = core::draw_point(key_, channel::Params, site.cell.face, site.cell.ci, site.cell.cj);
    const double frac = static_cast<double>(r) / std::max<std::size_t>(1, by_rank_.size());
    double t = frac < 0.02 ? 6.0 : (frac < 0.08 ? 5.0 : (frac < 0.25 ? 4.0 : (frac < 0.6 ? 3.0 : 2.0)));
    t += 0.7 * civ::normal01(d[3], d[0]);
    int tier = static_cast<int>(std::floor(t + 0.5));
    tier = std::clamp(tier, 1, 7);
    site.max_tier = static_cast<SettlementTier>(tier);
  }
}

double SettlementPlanner::arc(std::uint32_t a, std::uint32_t b) const {
  return arc_between(base_[a].centre, base_[b].centre);
}

SettlementPlan SettlementPlanner::plan(const CivState& state,
                                       const std::vector<FactionParams>& factions) const {
  SettlementPlan plan;
  plan.cells_per_face = n_;
  plan.provinces = base_;
  plan.by_rank = by_rank_;
  plan.suitable_count = suitable_count_;
  update(&plan, state, factions);
  return plan;
}

void SettlementPlanner::update(SettlementPlan* plan, const CivState& state,
                               const std::vector<FactionParams>& factions) const {
  plan->level = state.settled ? state.level : 0;
  plan->progress = state.progress;
  plan->ruined = state.ruined;
  plan->domed = state.domed;
  plan->is_home = state.is_home;
  plan->capital = -1;
  plan->region_capitals.clear();
  plan->roads.clear();
  for (ProvinceSite& site : plan->provinces) {
    site.settled = false;
    site.tier = SettlementTier::None;
    site.site_progress = 0.0f;
    site.region = -1;
    site.region_capital = false;
    site.capital = false;
    site.faction = -1;
    site.radius_m = 0.0f;
  }
  plan->settled_count = 0;
  const int level = plan->level;
  if (level <= 0 || suitable_count_ == 0) {
    return;
  }
  // The moving cut-off (design 13.2). Level 1: 1-3 sites; level 7: every
  // province including oceans (the ecumenopolis layer takes over).
  std::uint32_t count;
  if (level >= 7) {
    count = static_cast<std::uint32_t>(plan->provinces.size());
  } else if (level == 1) {
    count = 1 + static_cast<std::uint32_t>(std::floor(2.0 * clamp01(plan->progress) + 0.001));
    if (count > suitable_count_) count = suitable_count_;
  } else {
    const double fraction = settled_fraction(level, plan->progress);
    count = static_cast<std::uint32_t>(std::ceil(fraction * suitable_count_ - 1e-9));
    if (count < 3) count = std::min<std::uint32_t>(3, suitable_count_);
    if (count > suitable_count_) count = suitable_count_;
  }
  if (level >= 7) {
    for (ProvinceSite& site : plan->provinces) site.settled = true;
    plan->settled_count = static_cast<std::uint32_t>(plan->provinces.size());
  } else {
    for (std::uint32_t r = 0; r < count; ++r) {
      plan->provinces[plan->by_rank[r]].settled = true;
    }
    plan->settled_count = count;
  }
  assign_tiers(plan);
  assign_regions(plan);
  if (plan->is_home) {
    assign_factions(plan, factions);
  }
  build_roads(plan);
}

void SettlementPlanner::assign_tiers(SettlementPlan* plan) const {
  const int level = plan->level;
  const std::uint32_t settled = plan->level >= 7 ? suitable_count_ : plan->settled_count;
  if (level >= 7) {
    // The ecumenopolis: every province (oceans included) is one
    // continuous city; ecumenopolis/v1 takes over from the site list. The
    // best-ranked province keeps the capital flag for tooling.
    for (ProvinceSite& site : plan->provinces) {
      site.tier = SettlementTier::Ecumenopolis;
      site.capital = false;
      site.radius_m = 0.0f;
      site.site_progress = 0.999999f;
    }
    if (!by_rank_.empty()) {
      plan->provinces[by_rank_[0]].capital = true;
      plan->capital = static_cast<int>(by_rank_[0]);
    }
    return;
  }
  // Tier by rank among the SETTLED provinces (design 13.3): top 20 %
  // towns from level 3, top 8 % cities from level 4, 3-8 metropolises at
  // level 6, exactly one capital from level 5.
  for (std::uint32_t r = 0; r < settled && r < by_rank_.size(); ++r) {
    ProvinceSite& site = plan->provinces[by_rank_[r]];
    if (!site.settled) continue;
    const double frac = static_cast<double>(r) / std::max<std::uint32_t>(1, settled);
    SettlementTier by_rank;
    if (level == 1) {
      by_rank = r == 0 ? SettlementTier::Hamlet : SettlementTier::Outpost;
    } else if (level == 2) {
      by_rank = frac < 0.4 ? SettlementTier::Village : SettlementTier::Hamlet;
    } else if (level == 3) {
      by_rank = frac < 0.2 ? SettlementTier::Town : (frac < 0.6 ? SettlementTier::Village : SettlementTier::Hamlet);
    } else if (level == 4) {
      by_rank = frac < 0.08 ? SettlementTier::City
                            : (frac < 0.25 ? SettlementTier::Town
                                           : (frac < 0.65 ? SettlementTier::Village : SettlementTier::Hamlet));
    } else if (level == 5) {
      by_rank = r == 0 ? SettlementTier::Capital
                       : (frac < 0.08 ? SettlementTier::City
                                      : (frac < 0.3 ? SettlementTier::Town
                                                    : (frac < 0.7 ? SettlementTier::Village : SettlementTier::Hamlet)));
    } else {
      // Level 6+: 3-8 metropolises after the capital.
      const std::uint32_t metros = std::min<std::uint32_t>(8, 3 + settled / 400);
      by_rank = r == 0 ? SettlementTier::Capital
                       : (r <= metros ? SettlementTier::Metropolis
                                      : (frac < 0.1 ? SettlementTier::City
                                                    : (frac < 0.35 ? SettlementTier::Town
                                                                   : (frac < 0.75 ? SettlementTier::Village
                                                                                  : SettlementTier::Hamlet))));
    }
    // Local wealth caps the tier; the capital/metropolis picks are the
    // planet's decision and exempt.
    SettlementTier tier = by_rank;
    if (tier != SettlementTier::Capital && tier != SettlementTier::Metropolis &&
        static_cast<int>(tier) > static_cast<int>(site.max_tier)) {
      tier = site.max_tier;
    }
    site.tier = tier;
    site.capital = tier == SettlementTier::Capital;
    if (site.capital) plan->capital = static_cast<int>(site.index);
    site.radius_m = static_cast<float>(tier_radius_m(tier));
    // Progress inside the tier (design 14.4), MONOTONE in time: measured
    // from the continuous level (level + progress, which never decreases
    // while the race lives) at which the site earned its current tier —
    // the later of the level it was settled at and the level the tier
    // first becomes available — scaled by the site's own growth. A level
    // flip therefore never resets a site; it only adds a ring.
    const double continuous = static_cast<double>(level) + clamp01(plan->progress);
    double tier_available = 1.0;
    switch (tier) {
      case SettlementTier::Outpost:
      case SettlementTier::Hamlet: tier_available = 1.0; break;
      case SettlementTier::Village: tier_available = 2.0; break;
      case SettlementTier::Town: tier_available = 3.0; break;
      case SettlementTier::City: tier_available = 3.0; break;
      case SettlementTier::Capital: tier_available = 5.0; break;
      case SettlementTier::Metropolis: tier_available = 6.0; break;
      default: break;
    }
    const double granted = std::max(settled_level_of(r), tier_available);
    const double p = clamp01((continuous - granted) * site.growth);
    site.site_progress = static_cast<float>(p >= 1.0 ? 0.999999 : p);
  }
}

double SettlementPlanner::settled_level_of(std::uint32_t rank) const {
  // The continuous level at which the province of this rank crosses the
  // moving cut-off (inverse of the count rule in update()).
  const auto count_at = [&](int level, double progress) -> double {
    if (level <= 0) return 0.0;
    if (level >= 7) return static_cast<double>(base_.size());
    if (level == 1) return 1.0 + 2.0 * clamp01(progress);
    double count = std::ceil(settled_fraction(level, progress) * suitable_count_ - 1e-9);
    if (count < 3.0) count = std::min(3.0, static_cast<double>(suitable_count_));
    return count;
  };
  const double needed = static_cast<double>(rank) + 1.0;
  for (int level = 1; level <= 6; ++level) {
    const double lo = count_at(level, 0.0);
    const double hi = count_at(level + 1, 0.0);
    if (needed <= lo) return static_cast<double>(level);
    if (needed <= hi) {
      return static_cast<double>(level) + (hi > lo ? clamp01((needed - lo) / (hi - lo)) : 0.0);
    }
  }
  return 7.0;
}

void SettlementPlanner::assign_regions(SettlementPlan* plan) const {
  const int level = plan->level;
  if (level < 3) {
    return;
  }
  // Regions: the top-k settled provinces by score are regional capitals
  // (k grows with level and settled count); every settled province joins
  // its nearest capital by great arc. One pass, canonical order.
  std::uint32_t k = level == 3 ? 2 + std::min<std::uint32_t>(3, plan->settled_count / 12)
                               : 3 + std::min<std::uint32_t>(9, plan->settled_count / 40);
  std::vector<std::uint32_t> capitals;
  for (std::uint32_t r = 0; r < by_rank_.size() && capitals.size() < k; ++r) {
    const std::uint32_t index = by_rank_[r];
    if (!plan->provinces[index].settled) continue;
    // Spread them: skip a candidate within 25 degrees of an existing one.
    bool too_close = false;
    for (const std::uint32_t c : capitals) {
      if (arc(index, c) < 0.44) { too_close = true; break; }
    }
    if (too_close && capitals.size() >= 2) continue;
    capitals.push_back(index);
  }
  for (std::size_t i = 0; i < capitals.size(); ++i) {
    plan->provinces[capitals[i]].region_capital = true;
    plan->provinces[capitals[i]].region = static_cast<int>(i);
    if (level >= 3 && plan->provinces[capitals[i]].tier < SettlementTier::City &&
        plan->provinces[capitals[i]].tier != SettlementTier::Capital) {
      // Regional capitals are cities from level 3 (design 13.3).
      plan->provinces[capitals[i]].tier = SettlementTier::City;
      plan->provinces[capitals[i]].radius_m = static_cast<float>(tier_radius_m(SettlementTier::City));
    }
  }
  plan->region_capitals = capitals;
  for (ProvinceSite& site : plan->provinces) {
    if (!site.settled || site.region_capital) continue;
    int best = -1;
    double best_arc = 1.0e9;
    for (std::size_t i = 0; i < capitals.size(); ++i) {
      const double a = arc(site.index, capitals[i]);
      if (a < best_arc) {
        best_arc = a;
        best = static_cast<int>(i);
      }
    }
    site.region = best;
  }
}

void SettlementPlanner::assign_factions(SettlementPlan* plan,
                                        const std::vector<FactionParams>& factions) const {
  if (factions.empty()) {
    return;
  }
  // Each faction gets a capital province (governments first, spaced by a
  // minimum arc) and a weight; every settled province takes the faction
  // with the highest w * exp(-arc / lambda) * (1 + noise) — a few large
  // states with enclaves (design 11.4).
  struct Seat {
    std::uint32_t province;
    double weight;
    double lambda;
  };
  std::vector<Seat> seats(factions.size(), Seat{0, 0.0, 0.0});
  std::vector<bool> taken(plan->provinces.size(), false);
  // Governments first so they take the best provinces.
  std::vector<std::size_t> order;
  for (std::size_t j = 0; j < factions.size(); ++j) if (factions[j].type == FactionType::Government) order.push_back(j);
  for (std::size_t j = 0; j < factions.size(); ++j) if (factions[j].type != FactionType::Government) order.push_back(j);
  const double min_arc = 0.35;  // ~20 degrees between seats
  for (const std::size_t j : order) {
    const FactionParams& f = factions[j];
    // Walk the ranking for the first settled province far enough from
    // every seat taken so far; fall back to the best free one.
    std::uint32_t pick = by_rank_.empty() ? 0 : by_rank_[0];
    bool found = false;
    for (std::uint32_t r = 0; r < by_rank_.size(); ++r) {
      const std::uint32_t index = by_rank_[r];
      if (!plan->provinces[index].settled || taken[index]) continue;
      bool ok = true;
      for (std::size_t k = 0; k < factions.size(); ++k) {
        if (seats[k].lambda > 0.0 && arc(index, seats[k].province) < min_arc) { ok = false; break; }
      }
      if (ok) { pick = index; found = true; break; }
    }
    if (!found) {
      for (std::uint32_t r = 0; r < by_rank_.size(); ++r) {
        const std::uint32_t index = by_rank_[r];
        if (plan->provinces[index].settled && !taken[index]) { pick = index; break; }
      }
    }
    taken[pick] = true;
    double weight = 1.0;
    switch (f.type) {
      case FactionType::Government: weight = 3.0; break;
      case FactionType::Independent: weight = 1.2; break;
      case FactionType::Outlaw: weight = 0.6; break;
      case FactionType::AlignedMachine: weight = 0.8; break;
      case FactionType::RenegadeMachine: weight = 0.4; break;
      case FactionType::Count: break;
    }
    seats[j] = Seat{pick, weight, f.type == FactionType::Government ? 0.9 : 0.45};
  }
  for (ProvinceSite& site : plan->provinces) {
    if (!site.settled) continue;
    const auto d = core::draw_point(key_, channel::Layout, site.cell.face, site.cell.ci, site.cell.cj);
    int best = -1;
    double best_score = -1.0;
    for (std::size_t j = 0; j < factions.size(); ++j) {
      const Seat& seat = seats[j];
      const double a = arc(site.index, seat.province);
      const double noise = u01(d[j % 4]) * 0.6 - 0.3;
      const double score = seat.weight * det::fast_exp(Real(-a / seat.lambda)).to_double() * (1.0 + noise);
      if (score > best_score) {
        best_score = score;
        best = static_cast<int>(j);
      }
    }
    site.faction = best;
  }
  for (std::size_t j = 0; j < factions.size(); ++j) {
    plan->provinces[seats[j].province].faction = static_cast<int>(j);  // the seat is its own
  }
}

Road SettlementPlanner::road_between(std::uint32_t a, std::uint32_t b, bool trunk) const {
  if (a > b) std::swap(a, b);
  Road road;
  road.a = a;
  road.b = b;
  road.trunk = trunk;
  road.width_m = trunk ? 14.0f : 8.0f;
  const Dir3 pa = base_[a].centre;
  const Dir3 pb = base_[b].centre;
  // Owner-cell draws: keyed by the smaller index — either endpoint
  // recomputes the same road.
  const auto d = core::draw_point(key_, channel::Layout, static_cast<std::int64_t>(a),
                                  static_cast<std::int64_t>(b), 1);
  // Three midpoint refinements of the great arc with a keyed lateral
  // wobble that shrinks with each level and a slope-avoiding pull toward
  // the flatter of the two provinces' sides. Fixed count, canonical.
  Dir3 pts[9];
  pts[0] = pa;
  pts[8] = pb;
  const double pull = 0.15 * (base_[a].flatness > base_[b].flatness ? 1.0 : -1.0);
  const auto refine = [&](int lo, int hi, int depth) {
    const int mid = (lo + hi) / 2;
    const Dir3& p = pts[lo];
    const Dir3& q = pts[hi];
    Dir3 m{p.x + q.x, p.y + q.y, p.z + q.z};
    m = normalize(m);
    // Lateral direction: cross(m, q - p).
    const Dir3 dir{q.x - p.x, q.y - p.y, q.z - p.z};
    Dir3 lat{m.y * dir.z - m.z * dir.y, m.z * dir.x - m.x * dir.z, m.x * dir.y - m.y * dir.x};
    const double len = std::sqrt(dot(lat, lat).to_double());
    if (len > 1e-12) {
      lat = Dir3{lat.x / Real(len), lat.y / Real(len), lat.z / Real(len)};
      const double span = std::sqrt(dot(dir, dir).to_double());
      const double amount = (u01(d[static_cast<std::size_t>(depth) % 4]) * 2.0 - 1.0 + pull) * 0.18 * span /
                            static_cast<double>(1 << depth);
      m = normalize(Dir3{m.x + lat.x * Real(amount), m.y + lat.y * Real(amount), m.z + lat.z * Real(amount)});
    }
    pts[mid] = m;
  };
  refine(0, 8, 0);
  refine(0, 4, 1);
  refine(4, 8, 1);
  refine(0, 2, 2);
  refine(2, 4, 2);
  refine(4, 6, 2);
  refine(6, 8, 2);
  for (int i = 0; i < 9; ++i) road.points[i] = pts[i];
  return road;
}

void SettlementPlanner::build_roads(SettlementPlan* plan) const {
  const int level = plan->level;
  if (level < 3 || plan->region_capitals.empty()) {
    return;
  }
  // Province adjacency: representatives closer than 1.7 cell angles.
  const double cell_arc = 2.0 / static_cast<double>(n_) * 0.8;  // ~ chord of a cell edge on the unit sphere
  const double adjacency = 1.7 * cell_arc;
  // Per region: Kruskal over the settled provinces in canonical edge
  // order (weight = arc * (1 + slope penalty), ties by index pair), plus
  // 0.2 N extra edges between the closest non-tree pairs.
  struct Edge {
    double weight;
    std::uint32_t a, b;
  };
  std::vector<std::uint32_t> members;
  std::vector<std::uint32_t> parent(plan->provinces.size());
  const auto find = [&](std::uint32_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  for (std::size_t region = 0; region < plan->region_capitals.size(); ++region) {
    members.clear();
    for (const ProvinceSite& site : plan->provinces) {
      if (site.settled && site.region == static_cast<int>(region)) members.push_back(site.index);
    }
    if (members.size() < 2) continue;
    std::vector<Edge> edges;
    for (std::size_t i = 0; i < members.size(); ++i) {
      for (std::size_t j = i + 1; j < members.size(); ++j) {
        const double a = arc(members[i], members[j]);
        if (a > adjacency * 3.0) continue;
        const double slope = 1.0 - 0.5 * (base_[members[i]].flatness + base_[members[j]].flatness);
        edges.push_back(Edge{a * (1.0 + slope), std::min(members[i], members[j]), std::max(members[i], members[j])});
      }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
      if (x.weight != y.weight) return x.weight < y.weight;
      if (x.a != y.a) return x.a < y.a;
      return x.b < y.b;
    });
    for (const std::uint32_t m : members) parent[m] = m;
    std::vector<Edge> extra;
    for (const Edge& e : edges) {
      const std::uint32_t ra = find(e.a);
      const std::uint32_t rb = find(e.b);
      if (ra == rb) {
        extra.push_back(e);
        continue;
      }
      parent[ra] = rb;
      plan->roads.push_back(road_between(e.a, e.b, false));
    }
    const std::size_t extra_count = static_cast<std::size_t>(0.2 * members.size());
    for (std::size_t i = 0; i < extra.size() && i < extra_count; ++i) {
      plan->roads.push_back(road_between(extra[i].a, extra[i].b, false));
    }
  }
  // Level 4+: trunk roads between region capitals (a spanning tree over
  // the capitals in canonical order).
  if (level >= 4 && plan->region_capitals.size() >= 2) {
    std::vector<Edge> edges;
    for (std::size_t i = 0; i < plan->region_capitals.size(); ++i) {
      for (std::size_t j = i + 1; j < plan->region_capitals.size(); ++j) {
        const std::uint32_t a = std::min(plan->region_capitals[i], plan->region_capitals[j]);
        const std::uint32_t b = std::max(plan->region_capitals[i], plan->region_capitals[j]);
        edges.push_back(Edge{arc(a, b), a, b});
      }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
      if (x.weight != y.weight) return x.weight < y.weight;
      if (x.a != y.a) return x.a < y.a;
      return x.b < y.b;
    });
    for (const std::uint32_t c : plan->region_capitals) parent[c] = c;
    for (const Edge& e : edges) {
      const std::uint32_t ra = find(e.a);
      const std::uint32_t rb = find(e.b);
      if (ra == rb) continue;
      parent[ra] = rb;
      plan->roads.push_back(road_between(e.a, e.b, true));
    }
  }
}

std::string SettlementPlan::to_json() const {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "{\"level\": %d, \"progress\": %.4f, \"suitable\": %u, \"settled\": %u, "
                "\"capital\": %d, \"regions\": %zu, \"roads\": %zu, \"ruined\": %s, \"home\": %s}",
                level, progress, suitable_count, settled_count, capital, region_capitals.size(),
                roads.size(), ruined ? "true" : "false", is_home ? "true" : "false");
  return buffer;
}

}  // namespace inf::gen
