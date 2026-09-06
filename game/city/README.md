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
  scene.hpp materials.hpp        Scene (opaque + foliage meshes, lights, draw ranges), the material table
  flora.hpp towers.hpp           trees/lamps/benches/parks; parametric towers (plan × profile × facade × base × crown)
  standards.hpp props.hpp        3–8 storey fabric; entrances, roofs, basins, monuments, ring, pads, overpasses, government
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

## What the bridge builds

- **Lots**: tower or standard by height budget (≥ 34 m and inradius ≥ 8 m
  makes a tower), materials by faction type (Government marble/white,
  Independent warm panels, Outlaw dark panels, machine factions chrome
  and lattice facades), type by usage (civic, industrial, agricultural
  sheds with planters, landing pads, monuments); construction < 1
  shortens the building.
- **Streets**: asphalt under every block, sidewalk plates with curbs, lane
  paint, crosswalks and corner lamps on the near levels; plazas in the
  courtyards the lot lattice leaves free; arterials with medians and
  trees; overpasses between plazas. Budgets by the development level's
  size class (L1–2 small, L3 medium, L4–5 large, L6+ metropolis).
- **Civic centre** on a planetary capital (level ≥ 5): the government
  building facing the unification ring across a marble plaza; lots inside
  the civic radius are cleared.
- **Levels**: with a focus, lots drop one level beyond 350 m and two beyond
  900 m from it; towers inside the full range carry three levels in a
  group switched at 200 m and 500 m by the camera; shadows come from the
  coarsest level. Draw ranges (one per block, one per tower level) are
  the units of culling and drawing.
- A lot whose geometry comes out non-finite is dropped whole.

## In the app and the tools

- The site view (`game/app/src/civ_view.cpp`) builds the near level (< 2.5
  km) through the city system on a worker thread inside a focus of 1200 m
  around the player; the mid and far levels keep the T0020 mass path, and
  a site keeps mass boxes (superblocks on a metropolis) beyond the focus.
- `infinity --city-showcase` places the catalog scene on the first Town
  of the anchor body; `--city-debug N` (1 albedo, 2 normal, 3 occlusion,
  4 shadow, 5 roughness, 6 sun term, 7 sky irradiance); `--no-ssao`,
  `--no-shadows`, `--no-taa`, `--no-city`; `--bench N` (mean frame time
  after the script settles, resident against drawn triangles); `--sweep N
  [--sweep-step m] [--sweep-out name]` (temporal-artifact residual with
  depth reprojection, heat map written); `--window WxH` with `--hidden`.
- `infinity-cli civ site --seed 83 --tier Town` prints the site's city
  scene statistics and capture camera lines; `infinity-cli hash-city` is
  the golden (the showcase for two seeds and the seed-83 home town's
  whole scene, centimetre-quantised).
- `game/tests/test_city.cpp`: determinism, finite geometry, budgets per
  level, the home town, the capital's civic centre and ring-6 towers.

## Numbers (seed 83, Radeon 780M, 1600×900)

| Scene | Lots | Triangles resident / drawn | Frame (all features) |
|---|---|---|---|
| Home town (province 8), whole site, full detail | 1237 | 1.17 M / 0.43 M | 76 ms |
| City (province 216), 1200 m focus, 220 towers | ~2800 | 2.6 M / 0.77 M | 82 ms (72 with no city) |
| Capital (province 32), 1200 m focus, civic centre, 69 towers | ~5300 | 3.0 M / 0.84 M | 74 ms |

The game's terrain and mass baseline at the City was 52–55 ms before this
work; the city's own share is ~10–13 ms and is vertex bandwidth (two
unshared 68-byte vertices per triangle over five passes). Temporal
residual (`--sweep 30`): 0.0011 on the home town at 1600×900, 0.0023 on
the showcase at 4K (the demo's own scene 0.0023).

The cityblock demo (`demos/cityblock/`) builds against this library and
stays the visual reference (`--bench`, `--sweep`).
