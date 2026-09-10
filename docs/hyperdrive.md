# Hyperdrive

Press **H** in flight to charge, then fly at **2–10c**. Hold **W/S** to raise/lower
the selected speed; mouse steering and **A/D** roll work as usual. The HUD shows
actual speed, selection and charge/cruise state. A cyan aperture and outward
trails mark the transition without hiding the target at the centre.

**H or Escape stops immediately**; trails fade out. Escape during hyperdrive
stops the ship instead of quitting. **E**, **M**, **F6**, focus loss and a confirmed
**J** jump also cancel. Ordinary flight resumes at rest. Firing is disabled while
hyperdrive controls translation.

Charge takes 0.8 simulation seconds. Acceleration/deceleration toward the selected
speed is 4c per second; W/S changes the selection by 2c per second. Here
`c = 299,792,458 game metres/second`, and “10c” is ten times that constant.
No world distances, orbital periods or generator settings change.

The drive refuses to engage near a body and automatically stops at the first
planet, moon or star in its swept path. Planet/moon shells extend at least 100 km
above the surface/atmosphere (5% of radius for large bodies); stellar shells are
1.6 star radii. **PROXIMITY STOP** means continue with ordinary thrust for the
approach. This keeps hyperdrive out of cities and terrain even if detail meshes
have not loaded. H must be pressed again after leaving the protected region.

Planet and moon anchor changes preserve flight. **Hold J to visit another star's
system.** J stops hyperdrive before the system/body frame switch and arrives at
rest. Continuous flight updates the galaxy view, but does not automatically load
another star's planets or terrain; the active system remains the one selected
through J. Crossing galactic spatial cells does not itself perform a system jump.

## Verification and profiling

The ordinary app script supports `hyper` (H toggle), `brake` (cancel), `thrust 1/0`
(W), `slow 1/0` (S), and the existing `attitude`, `pos`, `pose`, `wait`, `jump`,
`map`, `capture` and `quit` commands. Use explicit nonparallel forward/up vectors
for scripted attitudes. For example, after moving safely clear of all bodies:

```text
pos 0 0 100000000
attitude 0 0 1 0 1 0
hyper
wait 4
pose
thrust 1
wait 5
thrust 0
wait 2
pose
brake
```

Run the game with `--release --render-width 1280 --hyper-profile hyper.csv`
and an optional `--script` file. CSV rows record wall/simulation delta, frame-call
cost, state (0 off, 1 charging, 2 cruise, 3 proximity stop), actual/selected c,
planet-local position, last stellar-catalog build cost, resident/pending terrain
meshes, draw-item count and presentation success. Anchor/system handoffs are logged to stdout;
positions on opposite sides of a rebase must be transformed before comparison.
The catalog cost is a background-job duration, not a main-thread stall measurement.

Use a separate capture run: synchronous image readbacks perturb timings.
Simulation retains the ordinary flight limit of 0.1 seconds per frame, so stalls
longer than that slow traversal relative to wall time. The CSV exposes this;
optical motion is not proof of distance travelled. Existing synchronous anchor
creation, city preparation and J system generation can still stall. Terrain
streaming and star catalog completion are not hidden behind a loading animation.
Dense arrival scenes grow the renderer's uniform capacity within device limits;
late terrain and HUD items are no longer silently dropped at 4,096 draws.
