#include <doctest/doctest.h>

#include <cmath>

#include "core/seed.hpp"
#include "gen/galaxy.hpp"
#include "gen/galaxy_octree.hpp"
#include "gen/names.hpp"
#include "gen/universe.hpp"

using namespace inf;
using det::Real;

namespace {

core::Key home_galaxy_key() {
  const auto seed = core::parse_seed("83");
  const auto tree = gen::make_tree(*seed);
  const auto address =
      core::tree::Address{}
          .child(core::tree::Step{gen::name::ClustersAxis, core::tree::Cell::grid(0, 0, 0)})
          .child(core::tree::Step{gen::name::GalaxiesAxis, core::tree::Cell::index(0)});
  return tree->get(address)->key();
}

// Deterministic scan for the first galaxy variant of a wanted type.
gen::GalaxyParams params_of_type(gen::GalaxyType type) {
  const core::Key base = home_galaxy_key();
  for (int i = 0; i < 64; ++i) {
    const auto key = core::derive_child(base, gen::kind::Galaxy, i);
    const auto params = gen::derive_galaxy_params(key);
    if (params.type == type) {
      return params;
    }
  }
  FAIL("no galaxy of the requested type in 64 variants");
  return gen::GalaxyParams{};
}

}  // namespace

TEST_CASE("galaxy: type drives geometry, not just amplitude (WP1)") {
  const auto elliptical = params_of_type(gen::GalaxyType::Elliptical);
  CHECK(elliptical.arm_amplitude.to_double() == 0.0);   // no arms
  CHECK(elliptical.bulge_frac.to_double() == 1.0);      // no disc
  CHECK(elliptical.dust_opacity.to_double() < 0.05);    // almost no dust
  CHECK(elliptical.ellipticity.to_double() < 1.0);
  CHECK(elliptical.ellipticity.to_double() >= 0.3);

  const auto lenticular = params_of_type(gen::GalaxyType::Lenticular);
  CHECK(lenticular.arm_amplitude.to_double() == 0.0);   // disc without arms
  CHECK(lenticular.bulge_frac.to_double() < 1.0);       // but a disc exists

  const auto barred = params_of_type(gen::GalaxyType::Barred);
  CHECK(barred.arm_count >= 2);
  CHECK(barred.arm_count <= 6);
  CHECK(barred.bar_fraction.to_double() >= 0.15);
  CHECK(barred.pitch_deg.to_double() >= 11.0);
  CHECK(barred.pitch_deg.to_double() <= 32.0);

  const auto irregular = params_of_type(gen::GalaxyType::Irregular);
  CHECK(irregular.arm_amplitude.to_double() == 0.0);
  CHECK(irregular.clumpiness.to_double() >= 0.8);
}

TEST_CASE("galaxy: density model morphology (WP2)") {
  const auto spiral = params_of_type(gen::GalaxyType::Barred);
  const gen::GalaxyDensity density(spiral);
  const double radius = density.radius_m().to_double();

  SUBCASE("spiral arms modulate a mid-disc ring; the mean survives") {
    const double ring = radius * 0.45;
    double lo = 1e300;
    double hi = 0.0;
    for (int i = 0; i < 256; ++i) {
      const double angle = 6.283185307179586 * i / 256.0;
      const gen::Dir3 p{Real(ring * std::cos(angle)), Real(ring * std::sin(angle)),
                        Real(0.0)};
      const double value = density.stars(p).to_double();
      lo = std::min(lo, value);
      hi = std::max(hi, value);
    }
    CHECK(hi / lo > 1.5);  // real arm contrast
  }

  SUBCASE("dust is thinner than the stars and hugs the arms harder") {
    const double ring = radius * 0.45;
    const double h_thin = spiral.thin_scale_height_ly.to_double() * gen::kLightYearM;
    const gen::Dir3 mid{Real(ring), Real(0.0), Real(0.0)};
    const gen::Dir3 up{Real(ring), Real(0.0), Real(2.0 * h_thin)};
    const double star_falloff =
        density.stars(up).to_double() / density.stars(mid).to_double();
    const double dust_falloff =
        density.dust(up).to_double() / (density.dust(mid).to_double() + 1e-300);
    CHECK(dust_falloff < star_falloff);  // the dark rift is thinner
    double dust_lo = 1e300;
    double dust_hi = 0.0;
    for (int i = 0; i < 256; ++i) {
      const double angle = 6.283185307179586 * i / 256.0;
      const gen::Dir3 p{Real(ring * std::cos(angle)), Real(ring * std::sin(angle)),
                        Real(0.0)};
      const double value = density.dust(p).to_double();
      dust_lo = std::min(dust_lo, value);
      dust_hi = std::max(dust_hi, value);
    }
    double star_lo = 1e300;
    double star_hi = 0.0;
    for (int i = 0; i < 256; ++i) {
      const double angle = 6.283185307179586 * i / 256.0;
      const gen::Dir3 p{Real(ring * std::cos(angle)), Real(ring * std::sin(angle)),
                        Real(0.0)};
      const double value = density.stars(p).to_double();
      star_lo = std::min(star_lo, value);
      star_hi = std::max(star_hi, value);
    }
    CHECK(dust_hi / dust_lo > star_hi / star_lo);  // stronger arm contrast
  }

  SUBCASE("elliptical is smooth: no arm modulation, no disc plane") {
    const auto elliptical = params_of_type(gen::GalaxyType::Elliptical);
    const gen::GalaxyDensity ell(elliptical);
    const double ring = ell.radius_m().to_double() * 0.4;
    double lo = 1e300;
    double hi = 0.0;
    for (int i = 0; i < 128; ++i) {
      const double angle = 6.283185307179586 * i / 128.0;
      const gen::Dir3 p{Real(ring * std::cos(angle)), Real(ring * std::sin(angle)),
                        Real(0.0)};
      const double value = ell.stars(p).to_double();
      lo = std::min(lo, value);
      hi = std::max(hi, value);
    }
    CHECK(hi / lo < 1.0001);  // isotropic in the plane
    CHECK(ell.dust(gen::Dir3{Real(ring), Real(0.0), Real(0.0)}).to_double() == 0.0);
  }

  SUBCASE("population: old red bulge, younger disc, metallicity falls outward") {
    const auto center = density.population(gen::Dir3{Real(0.0), Real(0.0), Real(0.0)});
    const auto outer =
        density.population(gen::Dir3{Real(radius * 0.8), Real(0.0), Real(0.0)});
    CHECK(center.temperature_k.to_double() < outer.temperature_k.to_double());
    CHECK(center.metallicity.to_double() > outer.metallicity.to_double());
  }
}

TEST_CASE("galaxy octree: counts, determinism, magnitude-limited API (WP3)") {
  const core::Key key = home_galaxy_key();
  const auto params = gen::derive_galaxy_params(key);
  const gen::GalaxyOctree octree(key, params);

  SUBCASE("cell counts integrate to the density model's galaxy total") {
    const int level = 4;
    double total = 0.0;
    for (int x = 0; x < (1 << level); ++x) {
      for (int y = 0; y < (1 << level); ++y) {
        for (int z = 0; z < (1 << level); ++z) {
          total += octree.expected_systems({x, y, z, level}).to_double();
        }
      }
    }
    const double expected = params.total_mass_suns.to_double() /
                            gen::GalaxyOctree::kMeanSystemMassSuns;
    CHECK(total > expected * 0.5);
    CHECK(total < expected * 2.0);
  }

  SUBCASE("home leaf is valid and the neighbourhood is deterministic") {
    const gen::Dir3 home = gen::home_system_position_m(params);
    const auto leaf = octree.leaf_at(home);
    CHECK(octree.is_leaf(leaf));
    CHECK(leaf.level > 0);
    std::vector<gen::GalaxyOctree::CellId> found_a;
    std::vector<gen::GalaxyOctree::CellId> found_b;
    const Real range(20.0 * gen::kLightYearM);
    octree.systems_in_ball(home, range, 512, &found_a);
    const gen::GalaxyOctree octree_b(key, params);  // independent instance
    octree_b.systems_in_ball(home, range, 512, &found_b);
    REQUIRE(found_a.size() == found_b.size());
    CHECK(!found_a.empty());
    for (std::size_t i = 0; i < found_a.size(); ++i) {
      CHECK(found_a[i].x == found_b[i].x);
      CHECK(found_a[i].level == found_b[i].level);
      CHECK(octree.occupied(found_a[i]));
      CHECK(octree.is_leaf(found_a[i]));
    }
  }

  SUBCASE("luminous_count is closed-form on any cell — even the root") {
    // The ROOT holds the entire galaxy (~1e8-1e9 systems); a count that
    // instantiated stars could never return. It must answer instantly
    // and scale with the quantised luminosity fraction.
    const gen::GalaxyOctree::CellId root{0, 0, 0, 0};
    const auto bright = octree.luminous_count(root, Real(-8.0));
    const auto medium = octree.luminous_count(root, Real(0.0));
    const auto faint = octree.luminous_count(root, Real(8.0));
    CHECK(bright < medium);
    CHECK(medium < faint);
    const double lambda = octree.expected_systems(root).to_double();
    const double expected_medium =
        lambda * gen::GalaxyOctree::luminous_fraction(Real(0.0)).to_double();
    // Poisson around the closed-form expectation.
    CHECK(medium > expected_medium * 0.8 - 10.0);
    CHECK(medium < expected_medium * 1.2 + 10.0);
  }

  SUBCASE("luminous_star samples the truncated bright tail only") {
    const gen::GalaxyOctree::CellId root{0, 0, 0, 0};
    const Real limit(2.0);
    const auto count = octree.luminous_count(root, limit);
    REQUIRE(count > 0);
    for (std::uint32_t i = 0; i < 16 && i < count; ++i) {
      const auto star = octree.luminous_star(root, limit, i);
      CHECK(star.abs_mag.to_double() <= 2.26);  // quantised limit + epsilon
      CHECK(star.temperature_k.to_double() >= 2600.0);
      CHECK(star.temperature_k.to_double() <= 40000.0);
      // Position inside the cell.
      CHECK(std::abs(star.position_m.x.to_double()) <= octree.root_size_m() * 0.5);
    }
  }
}
