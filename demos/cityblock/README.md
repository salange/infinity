# cityblock — standalone city-block generator and renderer

![city block](../../docs/cityblock.png)

A separate binary that generates one city block of the human **tech
faction** and renders it with its own PBR pipeline. It exists to find out,
outside the game's terrain/LOD machinery, what building detail and image
quality a from-scratch procedural approach can reach. Nothing here is used
by the game yet; the game can adopt pieces once they prove out.

## Build and run

The demo is part of the superbuild (`INFINITY_BUILD_DEMOS`, on by default
when the app targets build):

```
cmake -S . -B build -G Ninja
ninja -C build cityblock
demos/cityblock/tools/fetch-assets.py        # CC0 textures + skies (once)
build/demos/cityblock/cityblock
```

Without the assets the demo still runs: every material has a procedural
fallback and the sky falls back to an analytic gradient.

Controls: click to capture the mouse (Esc releases), mouse look (yaw is
unbounded), `W A S D` move, `Q`/`E` down/up, Shift 5× fast, Ctrl slow,
scroll changes the cruise speed (72 m/s default; movement accelerates and
brakes over ~0.25 s), `N`
day/night, `F1` cycles debug views (albedo, normals, ambient occlusion,
shadow cascades, roughness, direct sun, IBL diffuse, IBL specular, sun
specular), `F2`–`F5` toggle SSAO / shadows / bloom / FXAA, `+`/`-`
exposure, `R` next seed, `P` prints the camera as `--cam/--target`
arguments, `F12` screenshot.

Flags: `--seed S`, `--width W --height H`, `--sky day|night|sunset|file.hdr`,
`--sky-yaw deg`, `--night`, `--cam x,y,z --target x,y,z`, `--msaa 1|4`,
`--no-context`, `--debug N`, `--ev bias`, `--hidden --capture out.png
--frames N` for scripted captures without a visible window.

## The city

`--size outpost|village|small|medium|large|metropolis` (default: from the
seed). Sizes grow non-linearly, each step roughly doubling the radius:
outpost 40 m (one settler couple's glass house beside the wreck of their
landing pod, nothing else), village 150 m (the house gains a deck and a
colonnade ring, the pod becomes a memorial where the unification ring will
stand, a few sparse buildings and gardens), small 320 m (a two-storey civic
hall on a plinth, one modest tower at most), medium 620 m (hall with a
dome, towers in the core), large 1.25 km (the full capitol, lattice
families unlocked), metropolis 2.5 km (taller dome and flag court, a dense
downtown of towers, standard buildings to the horizon). Building density
scales with size: small settlements leave lots unbuilt as lawns and voids
and use larger lots; the metropolis fills every lot. The city
(`src/city.cpp`) is a jittered street grid: every third line is an artery
(26 m, raised median with hedges and sparse trees), the rest secondary
roads (14 m); blocks are split into lots along alleys; one or two diagonal
arteries are cut through the blocks (convex clipping), which is what makes
the grid read as grown rather than drawn. Districts follow the distance to
the centre: the government building (marble foundation with wide stairs,
colonnade, glass dome) sits on the centre block with the unification
ring plaza in front; towers are placed on inner blocks with a probability
and a height ceiling set by the city size, and the lattice families
(diagrid, X-frame, hex) only unlock at large and metropolis; the rest of
the blocks carry standard buildings; some blocks become plazas.

- **Standard buildings** (`src/standards.cpp`): five types (office,
  residential with balconies, mixed with retail ground floor, civic in
  marble, lab), 3–6 storeys, one wall band and one glass strip per storey
  per edge or punched windows, corner pilasters, slab lines; each picks
  one of four **entrances** (canopy, portal, stairs with columns, glass
  vestibule) and one of four **roofs** (flat, parapet with equipment,
  green roof with hedges, monopitch metal roof with gable walls). The
  same entrance/roof kit is available to the towers.
- **Plazas** (five parametric kinds): fountain square with hedges and
  benches, formal square with twin basins, an axis path and a monument,
  terraced park, monument square on a raised marble foundation, garden
  with a curved path, and a landing lot with white pads (not in small
  cities).
- **Ground kit** (`src/props.cpp`): round and square water basins,
  tiered fountains, trimmed hedges (single boxes, hedge rings with gaps),
  low walls with caps, marble foundations with wide stairs, four monuments
  (fluted pillar, twisted metal ribbon, two free-standing diagrid strands
  joined by a ring, obelisk), the unification ring (chrome torus on a
  marble drum, glowing inner band at night, facing the government
  building), landing pads with markings, edge lights and a control mast,
  and white curved pedestrian overpasses (spline deck with an arched
  underside, slender Y columns, stair flights at both ends) between
  plazas across an artery.
- Trees are budgeted per city size and used sparsely (medians, plazas,
  gardens); point lights are capped at the 64 nearest the centre.

Geometry budget: full tower detail exists only for the core towers (at
most 10–14), mid detail out to the tower zone, the rest at the coarse
level; outer standard buildings drop trims and balconies. Triangle counts:
outpost 1 k, village 5 k, small 50 k, medium 0.4 M, large ≈ 3 M,
metropolis ≈ 5–6 M.

## What is generated (the original hero block, kept as families)

Everything is a pure function of the seed (Philox keys from the engine's
`core::Key` tree; `src/rng.hpp`). The block is 240 × 200 m with streets,
sidewalks, lane paint and crosswalks around it, and holds:

- a **diagrid tower** (rounded-square plan, 42 floors, white steel lattice
  over blue glass, two-storey lobby, crown lattice and lantern),
- **twin lens towers** (lens plans tapering to a point, horizontal white
  fins, sky bridge, terrazzo podium),
- a **fin tower** (dark glass cylinder with a weave of bronze vertical fins,
  cantilevered cap, louvre crown),
- an **X-frame block** (white structural diamond exoskeleton over a
  rectangular mid-rise),
- a **folded pavilion** (faceted white shell with triangulated glazing and
  internal trusses),
- a **terraced park** (stepped grass/concrete arcs, pool, benches, trees),
  tree groves, planters, paths, bollards, street lamps,
- **context towers** around the block so the sky has a skyline.

## Parametric towers

`src/towers.hpp/.cpp` is the building system. A `TowerSpec` has five
independent axes, so one generator produces the hero buildings *and*
endless variants for the rest of the city:

| Axis | Options |
|---|---|
| Plan | superellipse (exponent), lens (half width/thickness), circle, rounded rectangle, polygon; rotation |
| Profile | floors, floor height, quadratic taper, tip pinch, twist, one setback |
| Facade | curtain wall, sail (smooth glass, shader grid), ribbon (horizontal fins), fin weave (offset vertical fins with a depth wave), louvre blades, diagrid, X-frame (flat beams, legs to the ground), hex lattice |
| Base | two-storey lobby with columns and canopy, podium with roof terrace, colonnade, plinth with steps, splayed legs |
| Crown | parapet, lattice continuing past the roof, mast, glass lantern, louvre rings |

`spec_diagrid`, `spec_lens`, `spec_sail`, `spec_finweave`, `spec_xframe`
and `spec_hex` are the named families; `random_tower` picks a family and
jitters every axis inside its plausible ranges; `build_tower_group` puts
two or three towers of one family on a shared podium with a roof garden.
`build_tower` takes a detail level (2 full, 1 near context without mullion
boxes, 0 far context) so the same building costs a fraction outside the
centre. Glass materials are bound to floor heights so the interior-mapped
room grid always matches the real floors.

The site generators (streets, park, pavilion, trees, lamps, planters) live
in `src/scene.cpp` on top of the geometry builders in `src/geometry.cpp`
(plans, offsets, ear clipping, extrusions, tubes, beams, spheres, frusta).
Plans are counter-clockwise on paper (x right, z up on the page); the
outward wall normal is the right-hand normal of each edge.

## Renderer

`src/renderer.cpp` and `shaders/*.wgsl`, on wgpu-native:

- three cascaded shadow maps (2048², stabilised, rotated-Poisson PCF),
- depth/normal prepass → SSAO (16 samples, depth-aware blur),
- MSAA forward PBR (GGX/Smith/Schlick, geometric specular anti-aliasing,
  sun as a disc, HDR clamp), image-based lighting from a Poly Haven HDRI
  (sun extracted and removed from the map, GGX-prefiltered specular cube,
  SH9 irradiance, all computed on the CPU at load, `src/ibl.cpp`),
- **interior-mapped glass**: rooms behind every window are ray-cast
  analytically in the fragment shader (lit/unlit per room, warm/cool light,
  blinds, distance LOD, mullion grid), reflections from the environment,
- up to 64 point lights (street lamps, night only), emissive materials,
- alpha-to-coverage foliage with canopy-space normals,
- temporal anti-aliasing: Halton-jittered projection, exact depth
  reprojection (static world), 3×3 variance clipping, 0.92 history weight,
  on top of MSAA — this is what removes the shimmer and moiré of sub-pixel
  fins, tubes and window frames when the camera moves,
- per-object draw ranges: three detail levels per tower chosen by camera
  distance (200 m / 500 m), frustum culling per building block, shadows
  from the coarse level,
- bloom (13-tap downsample, tent upsample), ACES tonemap, FXAA,
- day/night switch with automatic exposure from the environment.

Materials are three RGBA8 texture arrays (albedo sRGB, tangent normal,
AO/roughness/height) with CPU mip chains; the sets are CC0 from ambientCG
listed in `assets/manifest.json`, fetched by `tools/fetch-assets.py`.

## Measuring temporal artifacts

`--sweep N [--sweep-step m] [--sweep-out name]` renders N frames while the
camera slides sideways by `step` per frame, reads back each final frame
and the depth buffer, reprojects every pixel into the previous frame
(exact for a static world) and subtracts the change a band-limited image
would show (local gradient × screen motion). What remains is temporal
aliasing — shimmer, moiré, crawling edges. It prints the mean residual,
the ten materials that contribute most (via the material-id debug view),
and writes `name-heat.png` (amplified residual, dark = stable) and
`name-frame.png`. `--no-taa` measures the MSAA-only path for comparison.
`--bench N` renders N offscreen frames and prints ms/frame. `--showcase`
replaces the city with the asset catalog: all tower families, the five
standard types and the ground kit arranged for one camera. A metropolis
always places every tower family in its core.

## Layout

```
demos/cityblock/
  CMakeLists.txt
  assets/manifest.json     CC0 asset list (textures, skies)
  tools/fetch-assets.py    downloads them into assets/{textures,sky} (git-ignored)
  shaders/*.wgsl           common, shadow, prepass, ssao, sky, main, post
  src/gpu.*                wgpu context, buffers, textures, readback
  src/renderer.*           passes and pipelines
  src/ibl.*                HDRI → sun + IBL
  src/textures.*           texture arrays, procedural fallbacks, leaf texture
  src/mesh.hpp geometry.cpp   vertex format and geometry builders
  src/city.*               street grid, districts, plazas, lots, overpasses
  src/standards.*          standard 3–6 storey buildings (types, entrances, roofs)
  src/props.*              ground kit: basins, fountains, hedges, walls, foundations, monuments, ring, pads, overpasses, government
  src/towers.*             parametric tower system (families, variants, groups)
  src/scene.*              materials, the block, site and landscape
  src/camera.hpp rng.hpp math.hpp main.cpp
```
