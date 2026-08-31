#include <bit>
#include <cmath>
#include <cstdio>

#include "core/golden.hpp"
#include "core/key.hpp"
#include "gen/effective_field.hpp"
#include "gen/golden.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "gen/terrain_sampler.hpp"
#include "gen/universe.hpp"
#include "world/chunk_grid.hpp"
#include "world/edit_store.hpp"

namespace inf::gen {

namespace {

world::SphereEdit sphere_at(const Dir3& dir, double r, double offset_r, double radius,
                            bool subtract) {
  world::SphereEdit edit;
  edit.center_raw[0] = det::Fixed64::from_double(dir.x.to_double() * (r + offset_r)).raw();
  edit.center_raw[1] = det::Fixed64::from_double(dir.y.to_double() * (r + offset_r)).raw();
  edit.center_raw[2] = det::Fixed64::from_double(dir.z.to_double() * (r + offset_r)).raw();
  edit.radius_raw = det::Fixed64::from_double(radius).raw();
  edit.subtract = subtract;
  return edit;
}

}  // namespace

std::string hash_edits_report() {
  std::string report = "# effective-density goldens (M7 scripted edit sequence)\n";
  const core::Seed128 seed{0, 7};
  const BodyHandle body = default_body(seed);
  const PlanetParams planet = derive_planet_params(body, PlanetType::Barren);
  const TerrainField field(body.entity, planet);

  // Fixed unit direction (normalized in doubles — reproducible bits).
  const double raw[3] = {0.45, 0.65, 0.61};
  const double len = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
  const Dir3 dir{det::Real(raw[0] / len), det::Real(raw[1] / len), det::Real(raw[2] / len)};
  const double r0 = field.ground_radius_m(dir).to_double();

  // Scripted sequence: dig, overlapping dig, partial refill, deep carve.
  world::CsgEditStore store;
  store.append(sphere_at(dir, r0, 0.0, 3.0, true));
  store.append(sphere_at(dir, r0, -2.0, 2.5, true));
  store.append(sphere_at(dir, r0, 0.5, 2.0, false));
  store.append(sphere_at(dir, r0, -6.0, 2.0, true));

  const EffectiveField effective(field, &store);
  core::GoldenHash ground_hash;
  for (int i = 0; i < 5; ++i) {
    // Nearby radials through and around the edited column.
    const double t = (i - 2) * 1.2e-5;
    const Dir3 probe{det::Real((raw[0] / len) + t), det::Real(raw[1] / len),
                     det::Real((raw[2] / len) - t)};
    const double plen = std::sqrt(probe.x.to_double() * probe.x.to_double() +
                                  probe.y.to_double() * probe.y.to_double() +
                                  probe.z.to_double() * probe.z.to_double());
    const Dir3 unit{det::Real(probe.x.to_double() / plen), det::Real(probe.y.to_double() / plen),
                    det::Real(probe.z.to_double() / plen)};
    ground_hash.feed(
        std::bit_cast<std::uint64_t>(effective.ground_radius_m(unit).to_double()));
  }

  // Effective padded grid of the chunk at the edit site (the mesh INPUT
  // with the overlay folded in — exactly what workers sample).
  const TerrainSampler sampler(field, &store);
  const world::FaceUV face_uv = world::dir_to_face_uv(dir);
  const std::uint32_t cells = 1U << 8U;
  const auto to_cell = [&](double c) {
    const double f = (c + 1.0) * 0.5 * cells;
    return static_cast<std::uint32_t>(f < 0 ? 0 : (f >= cells ? cells - 1 : f));
  };
  core::GoldenHash grid_hash;
  for (const std::int16_t shell : {std::int16_t{-1}, std::int16_t{0}}) {
    const core::ChunkAddr addr{face_uv.face, 8, to_cell(face_uv.u.to_double()),
                               to_cell(face_uv.v.to_double()), shell};
    const world::ChunkGrid grid = world::ChunkGrid::from_addr(addr, planet.radius_m);
    const world::PaddedDensity padded = sampler.sample_padded(grid);
    for (const det::Real value : padded.values) {
      grid_hash.feed(std::bit_cast<std::uint64_t>(value.to_double()));
    }
  }

  char line[96];
  std::snprintf(line, sizeof(line), "edits/v1 ground %016llx\n",
                static_cast<unsigned long long>(ground_hash.value()));
  report += line;
  std::snprintf(line, sizeof(line), "edits/v1 grid %016llx\n",
                static_cast<unsigned long long>(grid_hash.value()));
  report += line;
  return report;
}

}  // namespace inf::gen
