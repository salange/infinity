# human_tech_demo

A standalone, authored human-tech metropolis for visual development. The city
uses the shared `game::city` geometry library and a reusable Blender detail kit;
its scene, renderer and lighting remain independent of the game and `cityblock`.
Coordinates are metres in the city library's Y-up frame, at architectural 1:1
scale. The finite scene has no inhabitants, vehicles, simulation or collision.

## Build and run

From the game repository root, using C++20, CMake, Ninja and Blender 5.2:

```sh
uv run --script demos/human_tech_demo/tools/build-assets.py
uv run --script demos/human_tech_demo/tools/import-sky-assets.py \
  --source /path/to/um-unendlich/design/assets/human-tech-metropolis
uv run --script demos/human_tech_demo/tools/import-surface-assets.py \
  --source /path/to/um-unendlich/design/assets/human-tech-materials
cmake -S . -B build -G Ninja
cmake --build build --target human_tech_demo human_tech_scene_test \
  human_tech_asset_test human_tech_route_test human_tech_roof_test \
  human_tech_renderer_memory_test human_tech_renderer_lights_test \
  human_tech_point_visibility_test human_tech_voxel_coverage_test \
  human_tech_window_floor_filter_test human_tech_tower_floor_identity_test -j 4
ctest --test-dir build -R '^human_tech_' --output-on-failure
build/demos/human_tech_demo/human_tech_demo
```

The pinned wgpu, GLFW, stb and doctest dependencies are fetched during the first
configuration. Required kit, environment or reviewed material assets that are missing or invalid
produce an error. `--procedural-only` explicitly selects the reduced-detail,
dependency-free content preview. `--sky studio` selects an analytic sky.
Neither option represents the production reference captures.

The reviewed marble, paving and terrazzo source maps are unchanged CC0 assets
from ambientCG. `assets/surface-manifest.json` pins their individual file hashes,
source archives, channel encodings and measured material means. Import verifies
the complete library before copying files. Production requires decoded color,
normal and roughness maps; missing optional occlusion means an unoccluded surface.
Color resampling uses sRGB-aware filtering; normal and scalar maps stay linear.

The default window is 1280×720. Keys `1`–`6` select First Arrival, Galaxy,
Civic Square, Street, Lattice Garden and Landing Terrace. Click for mouse look;
Esc releases it. WASD and QE move, Shift accelerates, Ctrl slows, and the scroll
wheel changes speed. `N` changes lighting, `P` prints the camera and `F12` captures.

`--fullscreen` presents on the primary display at its current mode, preserving
the requested `--width`/`--height` as the internal rendering resolution. The image
is scaled to fit with its aspect ratio intact. Close the window to end the demo.
F1 cycles diagnostics; F2–F11 retain renderer effect and performance toggles.
The inspection camera can fly through surfaces; this demo has no player controller.

The actual city is identical for all six cameras. The layout and camera manifest
can be exported with `--scene-manifest scene.json`. `--cam x,y,z --target x,y,z
--fov degrees` sets an inspection camera. Use `--asset lookdev` for the shared
architectural, glass and planting assembly defined in `assets/lookdev.scene`.

## Repeatable engine captures

A working GPU and desktop display are required even for a hidden window.
Rendering and readback are offscreen and do not require window presentation.

```sh
uv run --script demos/human_tech_demo/tools/capture.py \
  --binary build/demos/human_tech_demo/human_tech_demo \
  --output build/human-tech-captures --repeat --analysis
```

The output directory must be empty. The script captures all six cameras at
1280×720 with seed `83`, 512² material textures, MSAA 4×, TAA, indirect light,
scene reflections, cascaded shadows and restrained post-processing. Every view
has a first updated frame, a settled frame after 16 fixed-time frames and a
completed-frame timing report. `--analysis` adds clay, silhouette, neutral and
camera movement comparisons. `--repeat` checks a fresh-process First Arrival
against the gallery on this executable/GPU. Pixel identity is not promised
across different devices or builds.

`captures.json` reports decoded image checks, dimensions, hashes, timing and
repeatability. These checks detect corrupt or missing images; **they cannot
establish concept parity**. Reference review is separate and must assess
composition, architecture, vegetation, material response, light and motion.
No capture is repainted or substituted with a Blender render.

For controlled renderer comparisons, `--shader-dir directory` loads a complete
saved shader snapshot. All twelve required shader files are checked before the
graphics device is created. Record the snapshot hashes with those diagnostic
captures; ordinary captures use the production shader directory by default.

`--gallery-components` adds single-frame solar diffuse, diffuse/room transmission,
reflected light, solar specular and window-field diagnostics to a gallery, with
bloom and temporal accumulation disabled for those components. Use
`--gallery-single-shot --shot street` to restrict the gallery to one camera.
Component definitions are pre-atmosphere; true glass transmission includes the
sampled background. Diagnostic sky tone mapping differs from the display-referred
beauty background, so compare like components when inspecting changes.
`--environment-light-multiplier M` scales sky illumination and reflections once,
before both voxel and surface consumers. It leaves exposure and independent
sun, moon and practical sources unchanged; the default is1.

The civic preset uses a fixed cloud panorama outside its original directional
sky plate. The plate core and lower hemisphere are preserved. Compare the same
native silver sphere and bevelled cube with
`--asset reflection-proof-environment-silver`, then repeat with
`--analytic-sky-exterior` to select the previous smooth exterior.
Geometry, optical material, civic light, exposure and camera are identical.

For a single view:

```sh
build/demos/human_tech_demo/human_tech_demo --hidden --shot garden \
  --capture garden.png --capture-initial garden-first.png --frames 16 \
  --timings garden-timings.json
```

Use `--view clay|silhouette|neutral`, `--no-indirect` and `--no-reflections` to
isolate contributions. `--sweep 24 --sweep-step 0.12 --sweep-dir forward
--sweep-blur 2 --sweep-out build/street-motion` records motion residuals.

The foreground diagrid can be inspected with `--asset diagrid-sample-frontal`,
`diagrid-sample-oblique` or `diagrid-sample-arrival`. These extract a bounded
section of the production curved lattice with recessed glass and floor geometry.
The Arrival option uses the actual shot camera to check apparent member width.
Append `-shallow` for a reduced-depth control with the same layout and material;
it is a geometry control, not a reconstruction of a previous complete scene.
Use `--sky authored` for the Arrival environment and `--sky studio` for the
built-in lighting comparison. Finite captures use the same `--capture` and
`--frames` options as the city.

Use `--asset arrival-blockout --view neutral` to inspect the production city's
street and landmark composition with coarse facades and reduced dressing.
This diagnostic shares the full city's layout; omit the asset option for the
detailed city. The camera and geometry stay identical across lighting views.

`--mesh-page-mib N` forces a smaller vertex upload page for graphics diagnostics.
The normal setting uses the adapter's buffer limit. This divides storage without
removing triangles or changing vertex attributes; a low cap can check page
boundaries on an isolated facade before loading the complete city.

## Scene and rendering

First Arrival uses a straight canal, connected bridges and two perpendicular
street families with rectangular parcels. Differing building heights, courtyards,
terraced frontage and civic landmarks provide variation within that survey;
the outer coastal districts retain their shoreline response.
Building massing is generated once; no four full copies of
all tower details are stored. Foreground construction uses bevelled ceramic,
bronze joints, recessed glazing, furnished rooms, railings, drains, roof plant
and layered vegetation. The editable Blender kit and metric placement contract
are documented in [AUTHORED_KIT.md](assets/AUTHORED_KIT.md).

The renderer uploads each kit resource once and uses instance transforms across
visibility, shadows, reflection and glass passes. It computes coarse city and
fine local voxel fields from current scene geometry at load time for indirect
light and offscreen reflected radiance. Ocean and wet-paving planar passes
reflect actual scene geometry; thin glass transmits an opaque scene-colour layer.
Clustered practical-light selection considers the full light list. Separate
sunset and galaxy environment assets supply sky imagery with explicit physical
light directions and cinematic exposure settings. The galaxy has independent
twilight sun and crescent moon lighting. The four close views use separate dusk,
rain-break, garden daylight and golden-hour presets. Sky orientation and all
light directions stay fixed during camera movement. The canonical scene manifest
records the lighting values separately from geometry and camera locations.
All presets retain the same city geometry.

Geometry submissions have a fixed upper triangle bound and preserve render
attachments across chunks, including multisample colour and depth. Opaque-only
resources are excluded from the transparent pass. Device loss stops capture
cleanly with an error instead of submitting readback to a failed device. First
frame diagnostics identify each stage and chunk; unique uploaded mesh triangles
and the expanded instance workload are different counts.

The pinned wgpu-native C interface does not expose the required acceleration
structure construction API, even on an adapter advertising experimental ray
queries. This implementation therefore uses raster visibility and approximate
voxel transport. It does not claim hardware triangle ray tracing. Coarse voxel
reflection, a single opaque background layer through glass, approximate water
absorption and simplified distant vegetation remain renderer limitations.
The skies are relative-radiance LDR assets with explicitly calibrated lighting.

The live target is a useful completed 720p frame within 1000 ms on the review
machine. Reports separate startup, first-frame, median, p95, maximum and peak
process memory; a stationary accumulation image alone cannot satisfy this target.
Visual acceptance requires the raw engine views and nearby motion to pass the
reference review, independently of performance and unit checks.
Visual parity comes first; performance tuning follows that gate.

Source and licensing: [assets/PROVENANCE.md](assets/PROVENANCE.md).

The First Arrival canal tower has a parameterized shaft, complete cellular
facade, shaped open crown rings and a raised roof pavilion. For native geometry
inspection, use `--asset canal-tower-sample-crown` or
`--asset canal-tower-sample-full`. These isolated views omit the surrounding city
and are not full-scene appearance or performance evidence.

Export the same production mesh into an editable Blender inspection scene after
building the demo:

```sh
uv run --script demos/human_tech_demo/tools/inspect-canal-tower.py \
  --output build/canal-tower-inspection
```

This uses a separate background Blender process, preserving the open desktop
scene. The exported geometry and normals are exact; Blender materials and
lighting are simplified for shape inspection. The native module remains the
source of the rendered tower.

The canonical manifest also includes camera waypoints along the market bridge
approach, cylinder podium access, landing stairs and retained garden connections.
The garden loggia route starts on its occupied elevated floor; the former east
ground-access staircase and high connector have been retired with their tower.
Capture continuous camera updates and representative images separately:

```sh
uv run --script demos/human_tech_demo/tools/capture-routes.py \
  --binary build/demos/human_tech_demo/human_tech_demo \
  --manifest build/human-tech-captures/scene.json \
  --output build/human-tech-routes
```

This retains the initial viewpoint and follows every connecting segment with
at most one metre of translation and five degrees of rotation per update, with
uninterrupted temporal history. It tests the actual fixed city; it does not add
collision or pathfinding to the inspection camera. Retained stairs are sampled
at every tread and landing. A separate structural test checks support,
headroom and wall crossings against the actual scene triangles along the full
routes; botanical visibility still requires image review. Raw images and frame timings accompany
the route definition for obstruction and temporal review.
