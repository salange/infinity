# Live galaxy traversal

Press **F6** in flight mode to enter the galaxy camera demo. Press **F6** again
or **Escape** to return to the untouched player position, orientation and mode.
The demo returns automatically after 120 seconds. Map mode owns its own input;
exit the map before starting the tour. The preparation screen remains cancellable.

The current app visits systems within its home galaxy. The route uses that same
seed's `home_galaxy_params`, never a separately generated galaxy or a fixed
100,000-light-year distance. Endpoints are ±1.1 times its nominal radius along
the normalized galactic vector (1, 0.35, 0.12). A galaxy has a diffuse outer tail;
these endpoints mean 10% beyond the model's nominal radius, not zero density.
Motion is linear in galactocentric game metres, crosses the origin at 60 seconds,
and reaches the opposite endpoint at 120 seconds. Speed is route length / 120;
`gen::kLightYearM` retains the existing 1:10 interstellar conversion. The camera
looks along travel on entry and smoothly turns to look back during the exit.

The demo is an observing camera, separate from the player's local system and
planet coordinates, physical speed limits, collision and persistent edits.
It does not jump between systems, alter a generator, or write a player save.

The existing sky integrator computes the shared galaxy density, dust, nebulae,
clusters and neighboring galaxy splats at half-second route samples, using
**512×512 cube faces**, the same resolution as the stationary sky. The background
worker retains 72 initial views (36 seconds of route lookahead) before departure,
then continues generating while flying. Its bounded queue holds up to 128 views:
at most roughly **3 GB** of sky pixels and 60,000-star catalogs, plus transient
worker and renderer allocations. The first generated entry view is displayed
while buffering, and preparation remains cancellable. Its measured time is
reported separately from the 120-second traversal.

Two GPU cube slots interpolate decoded linear radiance every frame. Only newly
arrived samples are uploaded; full images are never blended or uploaded on the
CPU each display frame. Resolved stars retain sample-relative positions, so
camera-relative parallax and inverse-square brightness update every frame.
Overlapping catalogs fade between samples. Coordinate subtraction happens in
f64 game metres before conversion to render-only light-year offsets. This does
not alter world coordinates or the interstellar scale.

No route samples survive a run. If production misses a deadline the camera clock
continues and the last available sky is held; the profiler exposes that age
instead of hiding it by slowing the tour. Cancellation is checked between
ray-march rows. Interplanetary zodiacal light is omitted for this interstellar
observer. The number of ray-march threads leaves two hardware threads available
(on systems with at least four).

Remaining approximations: diffuse sky views crossfade between half-second
samples, so rapid central features can smear; changing magnitude-limited catalogs
can fade stars in/out. The exact center is intrinsically bright in the existing
density and exposure model. Local planets and surfaces are not materialized
while traversing interstellar distances; their subpixel appearance is carried
by the galaxy and stellar LODs. The player resumes its original system on exit.

## Reproduce and measure

Run a Release build with the default seed (83):

```sh
./build-t0003/game/app/unendlich --release --galaxy-demo --galaxy-profile /tmp/galaxy.csv
```

The default fullscreen mode uses the primary display; the CSV records actual
framebuffer dimensions, including Retina scaling. `--window WxH` selects a
window instead. `--release` disables the debug capture ring. The profile command
exits after returning from the demo. Without `--galaxy-demo`, `--galaxy-profile`
records each manually activated tour to the selected file (replacing it).

CSV rows cover the whole traversal: monotonic elapsed time, frame-call cadence,
CPU upload submission, sky deadline lag, source bake cost, surface
presentation success and framebuffer size. Header metadata includes adapter,
preparation time, cancellation/completion, shutdown join time, diameter and speed.
The target on the development Mac is its native 3024×1964 framebuffer and a
16.7 ms frame budget (`--window 1512x982` with 2× Retina scaling). Check the CSV
dimensions rather than inferring physical pixels from window points.

Frame cadence includes render submission and presentation backpressure; it is
**not** a GPU timestamp or an OS compositor scanout measurement. Failed surface
acquisition remains a row and must not be counted as a displayed frame.
Preparation precedes the route clock and is reported separately. Compare entry
(0–40 s), center (40–80 s), and exit (80–120 s); count frames above 16.7 ms as
well as tail percentiles, maximum, missing presentations and nonzero sky lag.

For visual inspection, add `--capture /tmp/galaxy` to a separate run. It produces
`/tmp/galaxy-0.ppm`, `-60.ppm`, and `-120.ppm`. These synchronous GPU readbacks
perturb timing; do not use a capture run as performance evidence.
