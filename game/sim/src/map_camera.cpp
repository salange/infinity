#include "sim/map_camera.hpp"

#include <algorithm>
#include <cmath>

namespace inf::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

double eased(double t, double exponent) {
  t = std::clamp(t, 0.0, 1.0);
  // Symmetric smoothstep-style ease with adjustable sharpness.
  const double s = std::pow(t, exponent);
  const double inv = std::pow(1.0 - t, exponent);
  return s / (s + inv);
}

Vec3 orthonormal_up(const Vec3& forward, const Vec3& up_hint) {
  const Vec3 up = up_hint - forward * dot(up_hint, forward);
  const double len = length(up);
  if (len < 1e-9) {
    // Degenerate hint: any perpendicular will do.
    const Vec3 alt = std::abs(forward.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    return normalize(alt - forward * dot(alt, forward));
  }
  return up * (1.0 / len);
}

}  // namespace

Pose map_pose(const Vec3& plane_normal, const Vec3& departure_system_pos,
              double outer_orbit_radius_m, double fov_y, const MapCameraParams& params) {
  const Vec3 normal = normalize(plane_normal);

  // Azimuth: the departure point projected into the plane. The camera is
  // displaced toward that azimuth, so with up ~ plane normal the
  // departure side lands in the lower half of the screen (continuity with
  // where the player came from).
  Vec3 azimuth = departure_system_pos - normal * dot(departure_system_pos, normal);
  if (length(azimuth) < 1.0) {
    azimuth = std::abs(normal.z) < 0.9 ? cross(normal, Vec3{0.0, 0.0, 1.0})
                                       : cross(normal, Vec3{1.0, 0.0, 0.0});
  }
  azimuth = normalize(azimuth);

  const double elevation = params.elevation_deg * kPi / 180.0;
  const Vec3 dir = normalize(normal * std::sin(elevation) + azimuth * std::cos(elevation));
  const double distance =
      outer_orbit_radius_m * params.frame_margin / std::tan(fov_y * 0.5);

  Pose pose;
  pose.position = dir * distance;
  pose.forward = dir * -1.0;  // look at the barycenter
  pose.up = orthonormal_up(pose.forward, normal);
  return pose;
}

Pose transition_pose(const Pose& from, const Pose& to, const Vec3& local_up, double t,
                     const MapCameraParams& params) {
  const double u = eased(t, params.ease_exponent);
  const double map_distance = length(to.position - from.position);

  // Cubic Bezier: climb along the local vertical first, then arc out
  // toward the map pose ("planet falling away below, then the system
  // sliding into frame").
  const Vec3 p0 = from.position;
  const Vec3 p1 = from.position + normalize(local_up) * (map_distance * params.pull_up_fraction);
  const Vec3 p2 = to.position + (from.position - to.position) * params.arc_fraction;
  const Vec3 p3 = to.position;
  const double w = 1.0 - u;
  const Vec3 position = p0 * (w * w * w) + p1 * (3.0 * w * w * u) +
                        p2 * (3.0 * w * u * u) + p3 * (u * u * u);

  // Orientation: blend forward/up and re-orthonormalize (nlerp — no pops,
  // roll continuity comes from blending the up vectors).
  Pose pose;
  pose.position = position;
  pose.forward = normalize(from.forward * (1.0 - u) + to.forward * u);
  pose.up = orthonormal_up(pose.forward, from.up * (1.0 - u) + to.up * u);
  return pose;
}

}  // namespace inf::sim
