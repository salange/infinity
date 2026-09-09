# Continuous galaxy flight

Press **F6** in normal flight mode. The player starts at the current position,
with the current attitude, roll, field of view and exposure. If the view points
more than 10 degrees away from the center, it first turns smoothly toward it.
A smaller correction blends into departure. The first second of translation
accelerates gently at first, then reaches interstellar cruise speed. The route
curves smoothly clear of the nearest departure body if it blocks the center;
that local detour vanishes before the interstellar crossing. The route
passes through the center and brakes at the opposite outskirts after 120 seconds.
The outskirts are 1.1 times the seed's nominal galaxy radius; the density has a
soft outer tail. Distances retain the existing `gen::kLightYearM` conversion.

**F6** or **Escape** brakes over one second and restores normal controls at the
reached location. Completion does the same at the route endpoint. It does not
restore the departure position. Exit map mode before starting. The automatic
controller owns movement while active; ordinary controls work after it finishes.
Small flight steps accumulate even at large interstellar coordinates.

## Shared rendering

The flight is a camera controller inside the ordinary game loop. Ordinary play,
J arrivals and this controller use the same spatial sky, star visibility,
atmosphere, exposure and projection. There is no separate flight scene, camera
cut, quality switch, future-image queue, catalog crossfade, or buffering screen.
Existing world generators, seeds, addresses and scale factors remain unchanged.
The departure system's bodies retain their actual positions and angular sizes;
a distant sun's minimum raster footprint conserves its shrinking visible area.

A disposable **192³ RGBA16F spatial field** samples the galaxy density, dust,
nebula and cluster models once during scene initialization. It stores emission
and extinction, not pictures of future camera views. Nonlinear coordinates
concentrate samples in the thin disc and core. Every frame integrates the field
from the actual observer with 96 logarithmically spaced segments, trilinear
sampling and continuous attenuation. The resolution is fixed throughout flight.
The field occupies 54 MiB plus a 13.5 MiB texture companion; CPU preparation
arrays are released after upload. It is rebuilt from the seed, never persisted.

A background stellar worker continually prepares a short corridor around the
current position and velocity (0.25 seconds behind, 1.25 seconds ahead). It holds
one pending request and one completed result. Meaningful direction/speed changes
invalidate obsolete work and completed results. A 64 MiB expendable cell cache
reduces repeated generation; result vertex buffers are additional transient
memory. There is no route-length lookahead or queue of future skies.

Stars belong to fixed spatial cells and fixed intrinsic-magnitude bands; cell
subdivision depends on population, not observer distance. Surviving points keep
the same seeded positions, luminosities and colors across catalog changes.
Actual distance, inverse-square flux, angular size and a smooth photometric
visibility window are evaluated on the GPU every frame. Preparation includes a
fainter visibility guard and follows continuously adapting exposure. Neighboring
galaxies also retain fixed positions. The macro background includes the home
cluster and its 26 adjacent clusters, with all of their generated companions.
Home-cluster galaxies use up to 48³ spatial density/dust fields (96³ for large nearby
companions), marched from the actual eye. More distant cluster objects use
unresolved morphology profiles. Neither level changes during this tour, and
objects are never moved or brightened to fill an empty part of the sky.
The external volumes share a 64 MiB texture budget, including their companion
height textures. Populous seeds use a smaller, fixed macro sampling grid.

Bright stellar points have round, compact optical profiles. Continuous HDR and
bloom highlight shoulders prevent saturated points from exposing either their
billboard or the blur kernel as white squares. These optical glows do not imply
large physical bodies: at interstellar distances the stellar discs usually
remain far below a pixel. This representation does not invent detailed systems
for the statistical luminosity samples.

Dark bands toward the center come primarily from modeled **dust extinction**,
including dark nebulae: they obscure light behind them. Low stellar density can
also make a region dim, but a dark lane does not imply an empty region.

These remain rendering approximations: the fixed field smooths features smaller
than its sample spacing, and resolved stellar points use the existing statistical
GalaxyOctree representation. Continuous travel does not yet stream an arbitrary
new planetary system's terrain on approach; J still materializes that destination
system. Matching-view checks therefore cover the galactic sky, not a claim of
complete planet/surface streaming along every possible interstellar path.

## Run and measure

For a Release build and a 1280×720 physical-pixel window:

```sh
./build/game/app/unendlich --release --render-width 1280 --galaxy-demo \
  --galaxy-profile /tmp/galaxy.csv
uv run --script tools/profile-galaxy.py /tmp/galaxy.csv
```

`--render-width` accepts 320–16384 pixels and uses a fixed-size 16:9 window. It disables
automatic framebuffer scaling on Retina displays; verify the actual dimensions
in the profile. `--window WxH` remains available for ordinary logical window
sizes. `--release` disables the debug capture ring. The automatic profile run
exits on completion; without `--galaxy-demo`, the chosen file records each F6
flight, replacing the previous contents.

The CSV covers the complete 120-second trajectory, including its first and last
frames. It records elapsed time, frame-call duration, last catalog build cost,
presentation success, actual framebuffer dimensions and galactocentric position.
Timing breakdowns cover resource updates, world/terrain work, stellar uploads,
draw preparation, and rendering/presentation.
A monotonic clock drives the camera independently of simulation frame clamping;
a late frame never silently extends the route. The summary checks completeness
and reports departure, entry, center and exit, including 24/60 fps budget misses.
Frame-call cadence includes render submission and presentation backpressure. It
is not a GPU timestamp or an OS compositor scanout measurement.

Use a separate capture run for inspection: `--script` accepts `galaxy`,
`galaxy-stop`, `pose`, `galactic x y z`, `attitude fx fy fz ux uy uz`,
`exposure-lock 1`, and the existing `wait`, `hud`, `capture` and `record` commands.
Matching poses with exposure locked and `--no-taa` compare a moving view against
a stationary one without exposure/history differences. Synchronous screenshots
can stall the GPU and must not be used as performance evidence.

The ordinary system's zodiacal light and gegenschein remain visible near its
orbital dust plane and fade continuously with distance from that system.
`INF_NOSKY` and `INF_NOSTARS` remain available for rendering diagnostics.

Automatic activation waits for the first normal scene presentation. That initial
scene frame belongs to application startup; the recorded first flight frame is
still at elapsed time zero. Manual F6 activation requires no preparation pause.
Material-library completion requests an asynchronous planet-texture refresh;
the main loop keeps rendering while an obsolete body bake finishes cancelling.
Each frame uploads at most one material tile and one face of a body texture;
a body texture becomes available only after all six faces are ready.
