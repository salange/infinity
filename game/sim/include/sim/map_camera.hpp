#pragma once

// Map-mode camera math (design/map-mode.md, ACCEPTED v1). Pure functions
// in the SYSTEM frame (star/barycenter at origin) — the app converts to
// planet-local for the floating-origin render path. Kept headless so the
// pose/transition geometry is testable without a window.

#include "sim/vec3.hpp"

namespace inf::sim {

struct Pose {
  Vec3 position;
  Vec3 forward;  // unit
  Vec3 up;       // unit, orthogonal to forward
};

// Everything tunable about the map camera and its cinematic transition —
// exposed for tuning by eye (spec section 1: "the money shot").
struct MapCameraParams {
  double enter_duration_s = 3.2;   // ~2.5-4 s per spec
  double exit_duration_s = 1.3;    // abbreviated reverse
  double elevation_deg = 78.0;     // off-plane elevation (75-80)
  double frame_margin = 1.10;      // outermost orbit +10%
  double pull_up_fraction = 0.06;  // first control point: climb along local up,
                                   // as a fraction of the map distance
  double arc_fraction = 0.55;      // second control point: how far out along
                                   // the map direction the arc bends
  double ease_exponent = 2.4;      // smoothstep sharpening (higher = softer ends)
};

// The stationary map pose (spec section 1): on a direction tilted
// elevation_deg off the orbital plane, azimuth aligned with the departure
// point so it lies toward the bottom of the screen, at a distance framing
// outer_orbit_radius_m with the given margin in the vertical fov.
Pose map_pose(const Vec3& plane_normal, const Vec3& departure_system_pos,
              double outer_orbit_radius_m, double fov_y, const MapCameraParams& params);

// Eased keyframed pose on the cinematic curve, t in [0, 1] (0 = from,
// 1 = to). Position follows a cubic Bezier: pull up along local_up first,
// then arc out toward the map pose while the orientation blends to frame
// the system. Used forward for entering and with (from, to) swapped for
// the abbreviated exit.
Pose transition_pose(const Pose& from, const Pose& to, const Vec3& local_up, double t,
                     const MapCameraParams& params);

}  // namespace inf::sim
