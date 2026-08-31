#include <doctest/doctest.h>

#include <cmath>

#include "core/key.hpp"
#include "gen/universe.hpp"
#include "gen/planet.hpp"
#include "gen/terrain.hpp"
#include "sim/player.hpp"
#include "gen/effective_field.hpp"

using namespace inf;
using sim::InputFrame;
using sim::Player;
using sim::PlayerMode;
using sim::Vec3;

namespace {

gen::BodyHandle body_for(std::uint64_t lo) {
  return gen::default_body(core::Seed128{0, lo});
}

InputFrame tick(double dt) {
  InputFrame input;
  input.dt = dt;
  return input;
}

double ground_r(const gen::TerrainField& field, const Vec3& pos) {
  const Vec3 dir = sim::normalize(pos);
  return field.ground_radius_m(gen::Dir3{det::Real(dir.x), det::Real(dir.y), det::Real(dir.z)})
      .to_double();
}

}  // namespace

TEST_CASE("player: ship cannot penetrate the ground") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);

  // Spawn deliberately below the surface: the first update must clamp the
  // ship back to clearance.
  const Vec3 dir{1.0, 0.2, 0.1};
  const Vec3 unit = sim::normalize(dir);
  const double ground = ground_r(field, unit);
  const gen::EffectiveField eff(field);
  Player player(eff, unit * (ground - 25.0));
  player.update(tick(0.016));
  CHECK(sim::length(player.position()) >= ground + Player::kShipClearance - 0.01);

  // Full throttle for a while: radius must never dip below clearance.
  for (int i = 0; i < 300; ++i) {
    InputFrame input = tick(0.016);
    input.forward = true;
    player.update(input);
    const double r = sim::length(player.position());
    const double local_ground = ground_r(field, player.position());
    REQUIRE(r >= local_ground + Player::kShipClearance - 0.05);
  }
}

TEST_CASE("player: E lands to eye height, walking stays glued, E takes off") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::EarthLike);
  const gen::TerrainField field(body.entity, planet);

  const Vec3 unit = sim::normalize(Vec3{1.0, -0.3, 0.25});
  const gen::EffectiveField eff(field);
  Player player(eff, unit * (ground_r(field, unit) + 120.0));

  InputFrame land = tick(0.016);
  land.interact_pressed = true;
  player.update(land);
  CHECK(player.mode() == PlayerMode::Landing);

  for (int i = 0; i < 600 && player.mode() == PlayerMode::Landing; ++i) {
    player.update(tick(0.016));
  }
  REQUIRE(player.mode() == PlayerMode::OnFoot);
  {
    const double expected = ground_r(field, player.position()) + Player::kEyeHeight;
    CHECK(sim::length(player.position()) == doctest::Approx(expected).epsilon(1e-6));
  }

  // Walk (with run) for 3 seconds across the terrain: always exactly eye
  // height above the local ground — cannot fall through.
  for (int i = 0; i < 180; ++i) {
    InputFrame input = tick(0.016);
    input.forward = true;
    input.run = true;
    input.mouse_dx = 3.0;  // wander
    player.update(input);
    const double expected = ground_r(field, player.position()) + Player::kEyeHeight;
    REQUIRE(sim::length(player.position()) == doctest::Approx(expected).epsilon(1e-6));
  }

  InputFrame takeoff = tick(0.016);
  takeoff.interact_pressed = true;
  player.update(takeoff);
  CHECK(player.mode() == PlayerMode::Takeoff);
  for (int i = 0; i < 600 && player.mode() == PlayerMode::Takeoff; ++i) {
    player.update(tick(0.016));
  }
  REQUIRE(player.mode() == PlayerMode::Flight);
  CHECK(player.altitude() > 30.0);
}

TEST_CASE("player: beams fire toward the crosshair and expire by distance") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::Barren);
  const gen::TerrainField field(body.entity, planet);
  const Vec3 unit = sim::normalize(Vec3{0.3, 1.0, 0.2});
  const gen::EffectiveField eff(field);
  Player player(eff, unit * (ground_r(field, unit) + 500.0));

  InputFrame fire = tick(0.016);
  fire.fire = true;
  player.update(fire);
  REQUIRE(player.beams().size() == 1);

  // Cooldown limits fire rate.
  player.update(fire);
  CHECK(player.beams().size() == 1);

  // Beam flies roughly along the ship forward (centered reticle).
  const auto& beam = player.beams().front();
  const Vec3 beam_dir = sim::normalize(beam.velocity);
  CHECK(sim::dot(beam_dir, player.forward()) > 0.99);

  // Expires after the max distance.
  for (int i = 0; i < 400 && !player.beams().empty(); ++i) {
    player.update(tick(0.016));
  }
  CHECK(player.beams().empty());
}

TEST_CASE("player: throttle never reverses") {
  const gen::BodyHandle body = body_for(0xBEEF);
  const gen::PlanetParams planet = gen::derive_planet_params(body, gen::PlanetType::Ice);
  const gen::TerrainField field(body.entity, planet);
  const Vec3 unit = sim::normalize(Vec3{1.0, 0.0, 0.0});
  const gen::EffectiveField eff(field);
  Player player(eff, unit * (ground_r(field, unit) + 2000.0));

  InputFrame accel = tick(0.016);
  accel.forward = true;
  for (int i = 0; i < 60; ++i) player.update(accel);
  CHECK(player.speed() > 0.0);

  InputFrame brake = tick(0.016);
  brake.back = true;
  for (int i = 0; i < 600; ++i) {
    player.update(brake);
    REQUIRE(player.speed() >= 0.0);
  }
  CHECK(player.speed() == 0.0);
}
