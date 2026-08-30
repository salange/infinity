#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "core/key.hpp"
#include "gen/mesher.hpp"
#include "gen/noise.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"

using namespace inf;
using det::Real;

namespace {

core::Key body_for(std::uint64_t lo) {
  const core::Key universe = core::universe_key(core::Seed128{0, lo});
  const core::Key galaxy = core::derive_child(universe, core::Kind::Galaxy, 0, 0, 0);
  const core::Key system = core::derive_child(galaxy, core::Kind::System, 0);
  return core::derive_child(system, core::Kind::Body, 0);
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
  const core::Key body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body, planet);
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
  const core::Key body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body, planet);

  // A chunk straddling the surface at the +X face center: find the shell
  // from the local elevation.
  core::ChunkAddr addr{0, 11, 1024, 1024, 0};
  {
    const gen::ChunkGrid probe = gen::ChunkGrid::from_addr(addr, planet);
    const gen::Dir3 dir = gen::face_uv_to_dir(
        gen::FaceUV{0, det::lerp(probe.u0, probe.u1, Real(0.5)),
                    det::lerp(probe.v0, probe.v1, Real(0.5))});
    const double elevation = field.elevation_m(dir).to_double();
    const double thickness = probe.r1.to_double() - probe.r0.to_double();
    addr.shell = static_cast<std::int16_t>(std::floor(elevation / thickness + 0.5));
  }
  const gen::ChunkGrid grid = gen::ChunkGrid::from_addr(addr, planet);
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
