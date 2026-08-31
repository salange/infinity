#include <doctest/doctest.h>

#include <cmath>

#include "core/key.hpp"
#include "gen/effective_field.hpp"
#include "gen/planet.hpp"
#include "gen/system.hpp"
#include "gen/terrain.hpp"
#include "gen/universe.hpp"
#include "sim/map_camera.hpp"
#include "sim/player.hpp"

using namespace inf;
using sim::MapCameraParams;
using sim::Pose;
using sim::Vec3;

namespace {

constexpr double kFov = 1.1;

bool orthonormal(const Pose& pose) {
  return std::abs(sim::length(pose.forward) - 1.0) < 1e-9 &&
         std::abs(sim::length(pose.up) - 1.0) < 1e-9 &&
         std::abs(sim::dot(pose.forward, pose.up)) < 1e-9;
}

}  // namespace

TEST_CASE("map pose: frames the outer orbit, departure toward screen bottom") {
  const MapCameraParams params;
  const Vec3 normal{0.0, 0.0, 1.0};
  const Vec3 departure{2.1e10, -0.6e10, 3.0e8};  // somewhere on an inner orbit
  const double outer = 4.0e11;
  const Pose pose = sim::map_pose(normal, departure, outer, kFov, params);

  CHECK(orthonormal(pose));
  // Looking at the barycenter.
  const Vec3 to_center = sim::normalize(Vec3{0.0, 0.0, 0.0} - pose.position);
  CHECK(sim::dot(to_center, pose.forward) > 0.999999);
  // The outer orbit fits inside the vertical fov with the 10% margin.
  const double distance = sim::length(pose.position);
  CHECK(std::atan(outer / distance) < kFov * 0.5);
  CHECK(distance == doctest::Approx(outer * params.frame_margin / std::tan(kFov * 0.5)));
  // Elevation off the plane matches the parameter.
  const double sin_elev = sim::dot(sim::normalize(pose.position), normal);
  CHECK(std::asin(sin_elev) ==
        doctest::Approx(params.elevation_deg * 3.14159265358979323846 / 180.0).epsilon(1e-6));
  // The departure point projects into the lower half of the screen.
  const double y_view = sim::dot(departure - pose.position, pose.up);
  CHECK(y_view < 0.0);
}

TEST_CASE("map transition: hits both endpoints, stays finite and orthonormal") {
  const MapCameraParams params;
  const Pose from{Vec3{6.4e5, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, Vec3{1.0, 0.0, 0.0}};
  const Pose to = sim::map_pose(Vec3{0.0, 0.0, 1.0}, from.position, 4.0e11, kFov, params);
  const Vec3 local_up{1.0, 0.0, 0.0};

  const Pose start = sim::transition_pose(from, to, local_up, 0.0, params);
  CHECK(sim::length(start.position - from.position) < 1.0);
  CHECK(sim::dot(start.forward, from.forward) > 0.9999);

  const Pose end = sim::transition_pose(from, to, local_up, 1.0, params);
  CHECK(sim::length(end.position - to.position) < 1.0);
  CHECK(sim::dot(end.forward, to.forward) > 0.9999);

  for (int i = 0; i <= 40; ++i) {
    const Pose pose = sim::transition_pose(from, to, local_up, i / 40.0, params);
    CHECK(orthonormal(pose));
    CHECK(std::isfinite(pose.position.x));
  }
  // The curve leaves the surface by climbing (early poses gain altitude).
  const Pose early = sim::transition_pose(from, to, local_up, 0.15, params);
  CHECK(sim::length(early.position) > sim::length(from.position));
}

TEST_CASE("player: map mode pushes and restores, planet-local, velocity zero") {
  const gen::BodyHandle body = gen::default_body(core::Seed128{0, 0xBEEF});
  const gen::PlanetParams planet =
      gen::derive_planet_params(body, gen::PlanetType::Barren);
  const gen::TerrainField field(body.entity, planet);
  const gen::EffectiveField effective(field);
  sim::Player player(effective, Vec3{planet.radius_m.to_double() * 2.0, 0.0, 0.0});

  // Build up some speed in flight, then enter the map.
  sim::InputFrame input;
  input.dt = 0.5;
  input.forward = true;
  for (int i = 0; i < 10; ++i) {
    player.update(input);
  }
  const Vec3 pos = player.position();
  REQUIRE(player.mode() == sim::PlayerMode::Flight);
  REQUIRE(player.speed() > 0.0);

  player.enter_map();
  CHECK(player.mode() == sim::PlayerMode::Map);
  // Suspended: input moves nothing.
  for (int i = 0; i < 10; ++i) {
    player.update(input);
  }
  CHECK(sim::length(player.position() - pos) == 0.0);

  player.exit_map();
  CHECK(player.mode() == sim::PlayerMode::Flight);
  CHECK(sim::length(player.position() - pos) == 0.0);  // exact planet-local spot
  CHECK(player.speed() == 0.0);                        // velocity = zero per spec
}

TEST_CASE("player: rebase swaps the anchor frame; push_out keeps bodies solid") {
  const gen::BodyHandle body_a = gen::default_body(core::Seed128{0, 0xA});
  const gen::PlanetParams planet_a =
      gen::derive_planet_params(body_a, gen::PlanetType::Barren);
  const gen::TerrainField field_a(body_a.entity, planet_a);
  const gen::EffectiveField effective_a(field_a);
  sim::Player player(effective_a, Vec3{planet_a.radius_m.to_double() * 3.0, 0.0, 0.0});

  // push_out: inside the keep-out sphere -> on its boundary, outside -> untouched.
  const Vec3 center = player.position() + Vec3{50.0, 0.0, 0.0};
  player.push_out(center, 200.0);
  CHECK(sim::length(player.position() - center) == doctest::Approx(200.0));
  const Vec3 kept = player.position();
  player.push_out(center, 100.0);
  CHECK(sim::length(player.position() - kept) == 0.0);

  // rebase: new field + frame, attitude kept, speed kept, mode kept.
  const gen::BodyHandle body_b = gen::default_body(core::Seed128{0, 0xB});
  const gen::PlanetParams planet_b =
      gen::derive_planet_params(body_b, gen::PlanetType::Ice);
  const gen::TerrainField field_b(body_b.entity, planet_b);
  const gen::EffectiveField effective_b(field_b);
  const Vec3 fwd_before = player.forward();
  const Vec3 new_pos{planet_b.radius_m.to_double() * 2.0, 1.0e5, 0.0};
  player.rebase(effective_b, new_pos);
  CHECK(player.mode() == sim::PlayerMode::Flight);
  CHECK(sim::length(player.position() - new_pos) == 0.0);
  CHECK(sim::dot(player.forward(), fwd_before) > 0.999999);
  // The new field governs physics now: zone comes from planet B.
  sim::InputFrame input;
  input.dt = 0.1;
  player.update(input);
  CHECK(std::isfinite(player.position().x));
}

TEST_CASE("body display names: deterministic, distinct-ish, printable") {
  const core::Key key_a{0x1234, 0x5678};
  const core::Key key_b{0x1234, 0x5679};
  const std::string name_a = gen::body_display_name(key_a);
  CHECK(name_a == gen::body_display_name(key_a));
  CHECK(name_a != gen::body_display_name(key_b));
  CHECK(name_a.size() >= 5);
  CHECK(name_a.find('-') != std::string::npos);
}
