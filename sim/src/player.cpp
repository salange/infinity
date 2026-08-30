#include "sim/player.hpp"

#include <algorithm>
#include <cmath>

namespace inf::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Non-linear reticle deviation -> turn rate: gentle near center, firm at
// the edge ("agile but not crazy").
double turn_curve(double deviation, double max_deviation, double max_rate) {
  const double normalized = std::clamp(deviation / max_deviation, -1.0, 1.0);
  // Signed quadratic: gentle near the center, firm at full deflection.
  return normalized * std::abs(normalized) * max_rate;
}

double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

}  // namespace

Player::Player(const gen::TerrainField& field, Vec3 spawn_position)
    : field_(field), position_(spawn_position) {
  // Start pointing "east-ish" with radial up-ish; orthonormalized below.
  up_ = normalize(position_);
  Vec3 reference{0.0, 0.0, 1.0};
  if (std::abs(dot(reference, up_)) > 0.98) {
    reference = Vec3{0.0, 1.0, 0.0};
  }
  forward_ = normalize(cross(reference, up_));
}

double Player::ground_radius(const Vec3& dir) const {
  const gen::Dir3 unit{det::Real(dir.x), det::Real(dir.y), det::Real(dir.z)};
  return field_.ground_radius_m(unit).to_double();
}

double Player::altitude() const {
  const Vec3 radial = normalize(position_);
  return length(position_) - ground_radius(radial);
}

void Player::update(const InputFrame& input) {
  fire_cooldown_ = std::max(0.0, fire_cooldown_ - input.dt);
  switch (mode_) {
    case PlayerMode::Flight: update_flight(input); break;
    case PlayerMode::OnFoot: update_on_foot(input); break;
    case PlayerMode::Landing:
    case PlayerMode::Takeoff: update_transition(input); break;
  }
  update_beams(input.dt);
}

void Player::update_flight(const InputFrame& input) {
  const double dt = input.dt;

  // --- reticle steering ---------------------------------------------------
  constexpr double kSensitivity = 0.0016;  // pixels -> NDC units
  reticle_x_ = std::clamp(reticle_x_ + input.mouse_dx * kSensitivity, -kReticleMax, kReticleMax);
  reticle_y_ = std::clamp(reticle_y_ - input.mouse_dy * kSensitivity, -kReticleMax, kReticleMax);

  constexpr double kMaxPitchRate = 1.5;  // rad/s at full deflection
  constexpr double kMaxYawRate = 1.1;
  constexpr double kRollRate = 1.7;
  const double pitch_rate = turn_curve(reticle_y_, kReticleMax, kMaxPitchRate);
  const double yaw_rate = turn_curve(reticle_x_, kReticleMax, kMaxYawRate);
  double roll_rate = 0.0;
  if (input.left) roll_rate += kRollRate;   // A: roll left
  if (input.right) roll_rate -= kRollRate;  // D: roll right

  // Apply body-frame angular velocities.
  const Vec3 right_axis = normalize(cross(forward_, up_));
  forward_ = rotate(forward_, right_axis, pitch_rate * dt);
  up_ = rotate(up_, right_axis, pitch_rate * dt);
  forward_ = rotate(forward_, up_, -yaw_rate * dt);
  up_ = rotate(up_, forward_, roll_rate * dt);
  // Re-orthonormalize (drift control).
  forward_ = normalize(forward_);
  up_ = normalize(up_ - forward_ * dot(up_, forward_));

  // --- throttle -----------------------------------------------------------
  const double alt = std::max(0.0, length(position_) - field_.planet().radius_m.to_double());
  const double cap = std::clamp(alt * 0.8, 40.0, 29'979'245.8);  // governor, 0.1c hard cap
  const double accel = std::max(25.0, cap / 2.5);
  if (input.forward) {
    speed_ += accel * dt;
  }
  if (input.back) {
    speed_ -= accel * 1.5 * dt;
  }
  speed_ = std::clamp(speed_, 0.0, cap);

  position_ = position_ + forward_ * (speed_ * dt);
  clamp_to_ground_flight();

  try_fire(input);

  if (input.interact_pressed) {
    // Begin landing: eased descent to eye height over the local ground,
    // leveling the ship into the tangent plane.
    const Vec3 radial = normalize(position_);
    const double target_r = ground_radius(radial) + kEyeHeight;
    mode_ = PlayerMode::Landing;
    transition_t_ = 0.0;
    transition_from_pos_ = position_;
    transition_to_pos_ = radial * target_r;
    // Fixed-feel ease, stretched for high-altitude landings so streaming
    // has a chance to keep up.
    transition_duration_ =
        std::clamp(length(transition_from_pos_ - transition_to_pos_) / 3000.0, 2.2, 8.0);
    transition_from_fwd_ = forward_;
    transition_from_up_ = up_;
    Vec3 level_forward = forward_ - radial * dot(forward_, radial);
    if (length(level_forward) < 1e-6) {
      level_forward = cross(radial, Vec3{0.0, 0.0, 1.0});
    }
    transition_to_fwd_ = normalize(level_forward);
    transition_to_up_ = radial;
    speed_ = 0.0;
    reticle_x_ = 0.0;
    reticle_y_ = 0.0;
  }
}

void Player::clamp_to_ground_flight() {
  const Vec3 radial = normalize(position_);
  const double min_r = ground_radius(radial) + kShipClearance;
  const double r = length(position_);
  if (r < min_r) {
    // Stop the inward component: slide along the surface at clearance.
    position_ = radial * min_r;
  }
}

void Player::update_on_foot(const InputFrame& input) {
  const double dt = input.dt;
  const Vec3 radial = normalize(position_);
  up_ = radial;

  // Shooter mouse: yaw about the radial, pitch clamped to the 180° arc.
  constexpr double kLookSensitivity = 0.0022;
  const double yaw = -input.mouse_dx * kLookSensitivity;
  const double pitch = -input.mouse_dy * kLookSensitivity;
  forward_ = rotate(forward_, up_, yaw);
  const Vec3 right_axis = normalize(cross(forward_, up_));
  Vec3 pitched = rotate(forward_, right_axis, pitch);
  // Clamp: keep at least ~1.5° away from straight up/down.
  constexpr double kMinAngle = 0.026;
  const double cos_up = dot(normalize(pitched), up_);
  if (cos_up < std::cos(kPi - kMinAngle) || cos_up > std::cos(kMinAngle)) {
    pitched = forward_;  // reject the step beyond the pole
  }
  forward_ = normalize(pitched);

  // Walking on the tangent plane.
  const Vec3 walk_forward = normalize(forward_ - up_ * dot(forward_, up_));
  const Vec3 walk_right = normalize(cross(walk_forward, up_));
  Vec3 move{0.0, 0.0, 0.0};
  if (input.forward) move = move + walk_forward;
  if (input.back) move = move - walk_forward;
  if (input.right) move = move + walk_right;
  if (input.left) move = move - walk_right;
  if (length(move) > 0.0) {
    // Brisk default gait (16 km/h); shift sprints.
    const double walk_speed = input.run ? 7.0 : 4.44;
    position_ = position_ + normalize(move) * (walk_speed * dt);
  }

  // Glued to the ground: eye height above the surface along the radial.
  const Vec3 new_radial = normalize(position_);
  position_ = new_radial * (ground_radius(new_radial) + kEyeHeight);

  try_fire(input);

  if (input.interact_pressed) {
    // Takeoff: eased climb to hover altitude, ship level.
    const Vec3 up_dir = normalize(position_);
    mode_ = PlayerMode::Takeoff;
    transition_t_ = 0.0;
    transition_duration_ = 1.8;
    transition_from_pos_ = position_;
    transition_to_pos_ = position_ + up_dir * 60.0;
    transition_from_fwd_ = forward_;
    transition_from_up_ = up_;
    Vec3 level_forward = forward_ - up_dir * dot(forward_, up_dir);
    if (length(level_forward) < 1e-6) {
      level_forward = cross(up_dir, Vec3{0.0, 0.0, 1.0});
    }
    transition_to_fwd_ = normalize(level_forward);
    transition_to_up_ = up_dir;
    speed_ = 0.0;
  }
}

void Player::update_transition(const InputFrame& input) {
  transition_t_ += input.dt / transition_duration_;
  const double t = smoothstep(std::clamp(transition_t_, 0.0, 1.0));
  position_ = lerp(transition_from_pos_, transition_to_pos_, t);
  forward_ = normalize(lerp(transition_from_fwd_, transition_to_fwd_, t));
  Vec3 up_blend = normalize(lerp(transition_from_up_, transition_to_up_, t));
  up_ = normalize(up_blend - forward_ * dot(up_blend, forward_));

  if (transition_t_ >= 1.0) {
    if (mode_ == PlayerMode::Landing) {
      mode_ = PlayerMode::OnFoot;
      const Vec3 radial = normalize(position_);
      position_ = radial * (ground_radius(radial) + kEyeHeight);
      up_ = radial;
      forward_ = normalize(forward_ - up_ * dot(forward_, up_));
    } else {
      mode_ = PlayerMode::Flight;
      speed_ = 0.0;
    }
  }
}

void Player::try_fire(const InputFrame& input) {
  if (!input.fire || fire_cooldown_ > 0.0) {
    return;
  }
  fire_cooldown_ = 0.16;

  // Aim ray through the crosshair. On foot the crosshair is centered
  // (view forward); in flight it is the reticle.
  Vec3 aim = forward_;
  if (mode_ == PlayerMode::Flight) {
    const double tan_half = std::tan(input.fov_y * 0.5);
    const Vec3 cam_right = normalize(cross(forward_, up_));
    aim = normalize(forward_ + cam_right * (reticle_x_ * tan_half * input.aspect) +
                    up_ * (reticle_y_ * tan_half));
  }
  Beam beam;
  beam.position = position_ + aim * 4.0 - up_ * 0.4;
  beam.velocity = aim * kBeamSpeed;
  beams_.push_back(beam);
}

void Player::update_beams(double dt) {
  for (Beam& beam : beams_) {
    beam.position = beam.position + beam.velocity * dt;
    beam.traveled += kBeamSpeed * dt;
  }
  beams_.erase(std::remove_if(beams_.begin(), beams_.end(),
                              [](const Beam& b) { return b.traveled > kBeamMaxDistance; }),
               beams_.end());
}

}  // namespace inf::sim
