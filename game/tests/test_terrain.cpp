#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/geo.hpp"
#include "gen/geo.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"

using namespace inf;
using det::Real;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

}  // namespace

TEST_CASE("noise: deterministic, bounded, continuous") {
  const std::uint64_t key = 0x1234567890ABCDEFULL;
  CHECK(gen::gradient_noise3(key, Real(1.5), Real(2.5), Real(3.5)) ==
        gen::gradient_noise3(key, Real(1.5), Real(2.5), Real(3.5)));

  double max_abs = 0.0;
  double max_step = 0.0;
  double previous = gen::gradient_noise3(key, Real(0.0), Real(0.7), Real(-0.3)).to_double();
  for (int i = 1; i <= 2000; ++i) {
    const double x = i * 0.01;
    const double value =
        gen::gradient_noise3(key, Real(x), Real(0.7), Real(-0.3)).to_double();
    max_abs = std::max(max_abs, std::abs(value));
    max_step = std::max(max_step, std::abs(value - previous));
    previous = value;
  }
  CHECK(max_abs <= 1.5);
  CHECK(max_abs > 0.05);       // not degenerate
  CHECK(max_step < 0.08);      // continuous at 0.01 sampling
}

TEST_CASE("noise: fbm sharpness and octave damping change the signal") {
  const std::uint64_t key = 42;
  gen::FbmParams smooth;
  gen::FbmParams ridged = smooth;
  ridged.sharpness = Real(1.0);
  int differing = 0;
  for (int i = 0; i < 32; ++i) {
    const Real x(0.37 * i);
    if (gen::fbm3(key, x, Real(0.1), Real(0.2), smooth) !=
        gen::fbm3(key, x, Real(0.1), Real(0.2), ridged)) {
      ++differing;
    }
  }
  CHECK(differing > 24);
}

TEST_CASE("terrain: density signs are physical") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);
  const double radius = planet.radius_m.to_double();

  // Far above any possible elevation: air.
  CHECK(field.density(gen::Dir3{Real(radius + 5000.0), Real(0.0), Real(0.0)}).to_double() < 0.0);
  // Well below the surface (but above the core): solid.
  CHECK(field.density(gen::Dir3{Real(radius - 5000.0), Real(0.0), Real(0.0)}).to_double() > 0.0);
  // Inside the core: the impenetrable clamp.
  CHECK(field.density(gen::Dir3{Real(radius * 0.4), Real(0.0), Real(0.0)}).to_double() ==
        1.0e9);
}

TEST_CASE("terrain+mesher: surface chunk meshes with outward normals") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);

  // A chunk straddling the surface at the +X face center: find the shell
  // from the local elevation.
  core::ChunkAddr addr{0, 11, 1024, 1024, 0};
  {
    const gen::ChunkGrid probe = gen::ChunkGrid::from_addr(addr, planet.radius_m);
    const gen::Dir3 dir = gen::face_uv_to_dir(
        gen::FaceUV{0, det::lerp(probe.u0, probe.u1, Real(0.5)),
                    det::lerp(probe.v0, probe.v1, Real(0.5))});
    const double elevation = field.elevation_m(dir).to_double();
    const double thickness = probe.r1.to_double() - probe.r0.to_double();
    // Aligned shells: shell s spans [radius + s*T, radius + (s+1)*T).
    addr.shell = static_cast<std::int16_t>(std::floor(elevation / thickness));
  }
  const gen::ChunkGrid grid = gen::ChunkGrid::from_addr(addr, planet.radius_m);
  const gen::PaddedDensity padded = gen::sample_chunk_density_padded(field, grid);

  // Padded inner slice must agree bit-exactly with the unpadded sampler
  // (the golden-hash artifact).
  const auto densities = gen::sample_chunk_density(field, grid);
  for (int gz = 0; gz <= 32; gz += 8) {
    for (int gy = 0; gy <= 32; gy += 8) {
      for (int gx = 0; gx <= 32; gx += 8) {
        const std::size_t index =
            (static_cast<std::size_t>(gz) * gen::ChunkGrid::kCorners + gy) *
                gen::ChunkGrid::kCorners +
            gx;
        REQUIRE(padded.at(gx, gy, gz) == densities[index]);
      }
    }
  }

  // Sanity: both signs present (the chunk actually straddles the surface).
  bool has_air = false;
  bool has_solid = false;
  for (const Real d : densities) {
    if (d.to_double() < 0.0) has_air = true;
    if (d.to_double() > 0.0) has_solid = true;
  }
  REQUIRE(has_air);
  REQUIRE(has_solid);

  const gen::ChunkMesh mesh = gen::mesh_chunk(grid, padded);
  REQUIRE(mesh.triangle_count() > 100);

  // No NaNs; unit normals; majority of normals point away from the planet
  // center (outward orientation — pins the winding convention).
  std::size_t outward = 0;
  std::size_t total = 0;
  for (std::size_t v = 0; v + 6 <= mesh.vertices.size(); v += 6) {
    for (int c = 0; c < 6; ++c) {
      REQUIRE(std::isfinite(mesh.vertices[v + c]));
    }
    const double px = mesh.origin[0] + mesh.vertices[v];
    const double py = mesh.origin[1] + mesh.vertices[v + 1];
    const double pz = mesh.origin[2] + mesh.vertices[v + 2];
    const double nx = mesh.vertices[v + 3];
    const double ny = mesh.vertices[v + 4];
    const double nz = mesh.vertices[v + 5];
    const double norm = std::sqrt(nx * nx + ny * ny + nz * nz);
    REQUIRE(norm == doctest::Approx(1.0).epsilon(0.01));
    if (px * nx + py * ny + pz * nz > 0.0) {
      ++outward;
    }
    ++total;
  }
  CHECK(outward > total * 7 / 10);

  // Determinism: same input, same mesh bytes.
  const gen::ChunkMesh mesh2 = gen::mesh_chunk(grid, padded);
  REQUIRE(mesh.vertices.size() == mesh2.vertices.size());
  CHECK(std::memcmp(mesh.vertices.data(), mesh2.vertices.data(),
                    mesh.vertices.size() * sizeof(float)) == 0);
}

TEST_CASE("terrain: elevation gradient matches central difference (WP0)") {
  const gen::BodyHandle body = body_for(0x7E11);
  const gen::PlanetParams planet =
      gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);
  const double radius = planet.radius_m.to_double();

  auto unit = [](double x, double y, double z) {
    const double len = std::sqrt(x * x + y * y + z * z);
    return gen::Dir3{Real(x / len), Real(y / len), Real(z / len)};
  };

  int checked = 0;
  for (int i = 0; i < 200; ++i) {
    // Deterministic pseudo-random directions.
    const double a = 0.618 * i + 0.1;
    const double b = 0.414 * i + 0.7;
    const gen::Dir3 dir = unit(std::cos(a) * std::cos(b), std::sin(a) * std::cos(b),
                               std::sin(b));
    const auto result = field.elevation_and_gradient(dir);

    // The gradient covers the noise term with locally-constant params:
    // difference elevation_from_params with params AND macro frozen at dir.
    const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir));
    gen::BlendedParams params{};
    params.relief_amplitude_m = canonical.relief_amplitude_m;
    params.base_elevation_m = canonical.base_elevation_m;
    params.ruggedness = canonical.ruggedness;
    params.carving = canonical.carving;
    const Real macro_rel = canonical.macro_rel;
    CHECK(std::abs(result.elevation_m.to_double() -
                   field.elevation_from_params(dir, params, macro_rel).to_double()) < 1e-9);
    // The exact-derivative contract applies to the PRE-EROSION base term
    // (WP2 erosion corrections are approximated in the exposed slope).

    // Two tangent directions.
    gen::Dir3 t1 = unit(-dir.y.to_double(), dir.x.to_double(), 0.0);
    if (std::abs(dir.z.to_double()) > 0.98) {
      t1 = unit(1.0, 0.0, 0.0);
    }
    // Finest octave is ~3 m since WP4 (12 octaves): the differencing
    // step must be well below the finest lattice cell in dir units.
    const double eps = 1e-8;
    for (int axis = 0; axis < 2; ++axis) {
      gen::Dir3 t = t1;
      if (axis == 1) {
        // t2 = dir x t1
        t = unit(dir.y.to_double() * t1.z.to_double() - dir.z.to_double() * t1.y.to_double(),
                 dir.z.to_double() * t1.x.to_double() - dir.x.to_double() * t1.z.to_double(),
                 dir.x.to_double() * t1.y.to_double() - dir.y.to_double() * t1.x.to_double());
      }
      auto offset = [&](double sign) {
        return unit(dir.x.to_double() + sign * eps * t.x.to_double(),
                    dir.y.to_double() + sign * eps * t.y.to_double(),
                    dir.z.to_double() + sign * eps * t.z.to_double());
      };
      const double h_hi = field.elevation_base_from_params(offset(1.0), params, macro_rel).to_double();
      const double h_mid = field.elevation_base_from_params(dir, params, macro_rel).to_double();
      const double h_lo = field.elevation_base_from_params(offset(-1.0), params, macro_rel).to_double();
      const double forward = (h_hi - h_mid) / (eps * radius);
      const double backward = (h_mid - h_lo) / (eps * radius);
      if (std::abs(forward - backward) > 1e-2 * (1.0 + std::abs(forward))) {
        continue;  // ridge-blend crease: not differentiable
      }
      const double numeric = (h_hi - h_lo) / (2.0 * eps * radius);
      const double analytic = result.slope.x.to_double() * t.x.to_double() +
                              result.slope.y.to_double() * t.y.to_double() +
                              result.slope.z.to_double() * t.z.to_double();
      CAPTURE(i);
      CAPTURE(axis);
      // 5e-3: the 12-octave field (WP4) leaves more near-crease softness
      // in the ridged blend than the 6-octave original; the strict 1e-3
      // contract is enforced at the noise level (engine tests).
      REQUIRE(std::abs(analytic - numeric) <
              5e-3 * (1.0 > std::abs(numeric) ? 1.0 : std::abs(numeric)));
      ++checked;
    }
    // Slope must be tangent: no radial component.
    const double radial = result.slope.x.to_double() * dir.x.to_double() +
                          result.slope.y.to_double() * dir.y.to_double() +
                          result.slope.z.to_double() * dir.z.to_double();
    CHECK(std::abs(radial) < 1e-9);
  }
  CHECK(checked > 300);
}

TEST_CASE("terrain: measured land fraction tracks the solved sea level (WP1)") {
  // The T0015 WP1 contract: the sea level is SOLVED from the macro
  // quantile, so measured land must track the drawn target. The wide
  // 100-seed sweep lives in `infinity-cli macro-stats`; this is the
  // in-tree canary.
  constexpr double kPi = 3.14159265358979323846;
  for (std::uint64_t seed : {3ULL, 7ULL, 0x83ULL, 0x2fULL, 21ULL, 55ULL}) {
    const gen::BodyHandle body = body_for(seed);
    const gen::PlanetParams planet =
        gen::derive_planet_params(body, gen::PlanetType::EarthLike);
    const gen::TerrainField field(body.entity, planet);
    const double sea = planet.sea_level_m.to_double();
    gen::TerrainField::ParamCache cache;
    double land = 0.0;
    double total = 0.0;
    for (int y = 0; y < 24; ++y) {
      const double lat = kPi * (0.5 - (y + 0.5) / 24.0);
      const double weight = std::cos(lat);
      for (int x = 0; x < 48; ++x) {
        const double lon = 2.0 * kPi * ((x + 0.5) / 48.0) - kPi;
        const double cos_lat = std::cos(lat);
        const gen::Dir3 dir{Real(cos_lat * std::cos(lon)), Real(cos_lat * std::sin(lon)),
                            Real(std::sin(lat))};
        const auto canonical = field.canonical_params(gen::dir_to_face_uv(dir), &cache);
        gen::BlendedParams params{};
        params.relief_amplitude_m = canonical.relief_amplitude_m;
        params.base_elevation_m = canonical.base_elevation_m;
        params.ruggedness = canonical.ruggedness;
        params.carving = canonical.carving;
        if (field.elevation_from_params(dir, params, canonical.macro_rel).to_double() >
            sea) {
          land += weight;
        }
        total += weight;
      }
    }
    CAPTURE(seed);
    CHECK(std::abs(land / total - planet.land_fraction.to_double()) < 0.06);
  }
}
