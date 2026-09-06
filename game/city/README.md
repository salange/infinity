# game/city — the city system

The buildings, streets and props of settled worlds. `game::city`
(namespace `inf::city`) sits above `game::gen`: sites/v1 decides where a
settlement is, its blocks, arterials and lots (design §13–§14); this
library turns those lots into geometry and the app draws it through the
renderer's city pipeline. Everything here is cosmetic in the contract's
sense — a pure function of the site's keys, but float geometry that may
differ in the last bit across machines. What the player finds (the
lots, their usage and height, the key buildings' positions) is decided
below this layer.

## Layout

```
include/city/
  math.hpp rng.hpp mesh.hpp      vectors, keyed counter RNG, Emit/plan helpers
  scene.hpp materials.hpp        Scene (opaque + foliage meshes, lights, draw ranges + fine ranges), the material table
  flora.hpp towers.hpp           trees/lamps/benches/parks; parametric towers (plan × profile × facade × base × crown)
  standards.hpp props.hpp        3–8 storey fabric; entrances, roofs, basins, monuments, ring, pads, overpasses, the civic centre by stage
  streets.hpp                    lane paint, crosswalks, asphalt, medians, the plaza kinds
  architecture.hpp               Architecture interface + registry by (race, faction)
  human/tech/architecture.hpp    the human tech architecture (the only one so far)
  site_build.hpp                 sites/v1 lots + plan -> Scene
  showcase.hpp                   the catalog scene (--city-showcase, hash-city)
```

The layout is meant to grow per race and faction: `human/tech/` is one
directory of many. An `Architecture` answers `build_lot` (a lot's
geometry from its footprint, height budget, usage and `StyleVector`),
`build_key` (the government building, the unification ring, landing
pads, monuments — the buildings a player will look for; a loaded asset
can replace one here without touching the lot pipeline) and
`materials()`. `architecture_for(style)` picks by race and faction type;
every pair without its own implementation gets the human tech one.

## Frames and keys

Generators work in a flat y-up scene frame (x east, y up from the site
datum, z south, metres from the site centre). `site_build` maps
sites/v1's site-local (east, north) into it (z = −north; polygons are
re-oriented counter-clockwise after the flip; angles turn the other
way). The app maps the scene onto the sphere through the site frame at
upload. Every lot's geometry is keyed under `buildings/v1` by lot id,
exactly as the T0020 mass executor keys it: `Rng(derive_child(
derive_named(site.key, BuildingsV1), kind::Lot, lot.id))`.

## How a settlement grows (T0022)

One growth table per tier (`site_build.cpp`, `growth_row`) maps the
demo's six size classes onto the design's tiers: Outpost stage 0,
Hamlet and Village 1, Town 2, City 3, Metropolis 4, Capital 5 (the
ecumenopolis keeps the capital's row until it has its own renderer).
The radius and the lots come from sites/v1; the table decides what the
city layer puts on them: the capitol stage, tower density (the demo's
per-block probability on ~130 m blocks scaled to the site's block
pitch, in an absolute core of 250/350/450 m with 0.35 of the density
out to 1.6 x), the tallest tower, hero facade families and tower groups,
plaza odds, the fill probability (an unbuilt lot stays a lawn with a
tree now and then — keyed per lot, and a lot once built stays built),
prop budgets, the per-city tower geometry budgets (full detail for at
most 10–14 core towers, the near-context level for 24–40 more, the rest
from the far level up) and the share of the radius beyond which
standard buildings drop trims and balconies. Probabilities interpolate
toward the next tier's row by the site's progress.

The civic centre is one parametric object (`props.cpp`,
`build_government(..., stage)`) that only adds as the stage rises:
stage 0 is a settler couple's glass house on a small plate next to the
wreck of their landing pod, lying where the unification ring will later
stand; 1 gives the house a deck and a colonnade ring and keeps the pod
as a memorial; 2 is a two-storey civic hall on a low plinth; 3 adds the
foundation and a small dome; 4 is the full capitol with its flag court;
5 the taller dome and three flags. From stage 3 the ring replaces the
pod. Marble is for the foundation only; walls and columns are white
metal and glass. An outpost is the founding scene alone: no blocks, no
streets, no lots, no arterial kit.

## What the bridge builds

- **Lots**: tower or standard by height budget (≥ 34 m and inradius ≥ 8 m
  makes a tower), materials by faction type (Government marble/white,
  Independent warm panels, Outlaw dark panels, machine factions chrome
  and lattice facades), type by usage (civic, industrial, agricultural
  sheds with planters, landing pads, monuments); construction < 1
  shortens the building; the tier's fill probability leaves lawns.
- **Streets**: asphalt under every block, sidewalk plates with curbs, lane
  paint, crosswalks and corner lamps on the near levels; plazas in the
  courtyards the lot lattice leaves free; arterials with medians and
  trees; overpasses between plazas. Budgets by the tier's growth row.
- **Tower blocks** inside the core: a keyed share of blocks is given whole
  to one tower (or a group on a shared podium from the metropolis up) on
  its plaza floor; a metropolis shows every hero family in its core.
- **Civic centre** at every settlement: the founding site or the union
  square by stage (above); lots inside the civic radius are cleared.
- **Levels**: towers carry four levels in a group — three of real
  geometry switched at 200 m and 500 m by the camera, and beyond 1.2 km
  the far shell (one glass quad per plan segment and a roof, ~100
  triangles); shadows come from the coarsest real level. From level 2
  (lattices) and level 1 (fins, louvre blades) the members are not
  geometry but a pattern code on the glass that the shader draws
  band-limited (`towers.cpp`, `pattern_at`; the rule: never draw members
  thinner than a pixel; `set_far_patterns(false)` keeps the geometry for
  comparison). With a focus, lots drop one level beyond 350 m and two
  beyond 900 m from it. Draw ranges (one per block, one per tower level)
  are the units of culling and drawing; every standard building also
  registers a fine range inside its block (`Scene::fine`) — the
  granularity of occlusion culling.
- A lot whose geometry comes out non-finite is dropped whole.

## In the app and the tools

- The site view (`game/app/src/civ_view.cpp`) builds the near level (< 2.5
  km from the site, measured from the site's own datum) through the city
  system on a worker thread inside a focus of 1200 m around the player;
  the mid and far levels keep the T0020 mass path, and a site keeps mass
  boxes (superblocks on a metropolis) beyond the focus.
- The upload (`city_render.cpp`) packs every vertex into the renderer's
  32-byte layout (position f32x3, octahedral normal and tangent
  snorm16x2, uv f16x2, material / element random / occlusion + tangent
  sign bytes, facade coordinates unorm16x2 in 1 cm steps). A frame hands
  the renderer one item per mesh with a list of its ranges for the main
  pass, the near and the far cascades and the occlusion candidates (the
  block's buildings, frustum-tested, the gaps kept); the renderer culls
  per range, writes indirect arguments per pass and issues one
  multi-draw call per mesh. Occlusion culling tests every candidate
  against a depth pyramid of this frame's prepass in a compute pass.
- Player-facing options (`Rhi::CitySettings`, each recreating its
  targets at runtime): GPU occlusion culling (F7, `--no-occlusion`), AO
  at half or full resolution (F8, `--ssao-full`), the two far shadow
  cascades refit and redrawn on alternating frames (F10,
  `--shadow-half-rate`), the far cascade cast by tower shells only and
  no bounded ranges beyond 350 m (F11, `--shadow-far-lod`).
  `--no-far-patterns` keeps the far-level member geometry for an A/B;
  `--stress N` recreates the targets every frame N times (the leak
  regression). Camera passes render reversed Z (near = 1, far = 0) on a
  float depth buffer — the standing rule for every renderer here.
- `infinity --city-showcase` places the catalog scene on the first Town
  of the anchor body; `--city-debug N` (1 albedo, 2 normal, 3 occlusion,
  4 shadow, 5 roughness, 6 sun term, 7 sky irradiance, 12 material id);
  `--no-ssao`, `--no-shadows`, `--no-taa`, `--no-city`; `--bench N` (mean
  frame time after the script settles; resident, drawn and shadow
  triangles; main, occluded and shadow ranges); `--sweep N [--sweep-step
  m] [--sweep-dir right|forward|down] [--sweep-blur R] [--sweep-out
  name]` (temporal-artifact residual: every pixel reprojected into the
  previous frame through the depth buffer, sampled and differenced with
  half a pixel of gradient tolerated; luminance and chroma apart, per
  material; heat maps written); `--window WxH` with `--hidden`.
- `infinity-cli civ site --seed 83 --tier Town` prints the site's city
  scene statistics and capture camera lines (each tier shows its stage);
  `infinity-cli hash-city` is the golden (the showcase for two seeds and
  the seed-83 home town's whole scene, centimetre-quantised).
- `game/tests/test_city.cpp`: determinism, finite geometry, budgets per
  level, the home town, the capital's civic centre and ring-6 towers.

## Numbers (seed 83, Radeon 780M, 1600×900, all features, default options)

| Scene | Lots | Triangles resident / drawn | Frame T0021 → T0022 |
|---|---|---|---|
| Town (province 16), 200 m above the centre | 1639 | 1.49 M / 0.54 M | 76 → 28 ms |
| Town, street level, forward | 1639 | 1.49 M / 0.56 M | — → 34 ms (354 of 831 ranges occluded) |
| City (province 216), 1200 m focus, 216 towers | ~2560 | 1.69 M / 0.49 M | 82 → 45 ms |
| Capital (province 32), 1200 m focus, 103 towers | ~5100 | 3.24 M / 0.99 M | 74 → 29 ms |

The terrain and mass baseline at the City site was 52–55 ms before the
city work and is the larger share of every number above; the options
take another 1–2 ms off the town (half-rate far cascades and the far
LOD together: 28.4 → 26.5 ms). Occlusion culls almost nothing from the
air and up to 43 % of the ranges at street level. Temporal residual
(`--sweep 30`, sideways 3 cm): 0.0004 on the home town at 1600×900;
forward flight at 1.2 m per frame over the capital's towers (blurred):
0.00061 with the far facade patterns, 0.00070 with the member geometry.

The cityblock demo (`demos/cityblock/`) builds against this library and
stays the visual reference (`--bench`, `--sweep`).
