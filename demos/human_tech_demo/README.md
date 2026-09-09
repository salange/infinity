# human_tech_demo

A standalone visual prototype of a human-tech metropolis, copied from
`demos/cityblock` at `db47f8e`. It composes a fixed city with the existing
`game::city` architecture kit and owns its renderer, materials, sky and cameras.
`cityblock` and the game remain independent targets; their sources are unchanged.

The scene has 585 connected blocks, 146 towers and 878 mid-rise buildings:
ivory diagrid and hex lattices, curved ribbon towers, bronze fins, a domed civic
hall and a vertical unification ring. Terraced frontages, canal crossings, roof
gardens, a sheltered storefront and a lattice garden connect the landmarks.
It is an empty visual set: no people, vehicles, simulation or collision.
Coordinates use the city library's metre, y-up frame at architectural 1:1 scale.

## Build and run

From the game repository root (C++20, CMake, Ninja; the existing pinned wgpu,
GLFW, stb and doctest dependencies are fetched on first configuration):

```sh
cmake -S . -B build -G Ninja
cmake --build build --target human_tech_demo cityblock human_tech_scene_test -j 8
ctest --test-dir build -R '^human_tech_scene$' --output-on-failure
build/demos/human_tech_demo/human_tech_demo
```

The executable is `build/demos/human_tech_demo/human_tech_demo`. A working GPU
and desktop display are required even for `--hidden`; capture rendering itself
is offscreen and does not wait for a hidden window to supply a drawable.

Keys `1`–`4` select aerial, civic, street and terrace views. Click for mouse look;
Esc releases it. WASD and QE retain cityblock's inspection camera, with Shift
fast, Ctrl slow and scroll speed. `N` toggles daylight/blue hour; `P` prints the
camera; `F12` captures. F1 cycles diagnostics; F2–F11 retain the renderer's effect
and performance toggles. This task adds no traversal system.

## Repeatable captures

```sh
uv run --no-project --python 3.12 python demos/human_tech_demo/tools/capture.py \
  --binary build/demos/human_tech_demo/human_tech_demo \
  --output build/human-tech-captures --repeat
```

This produces five 1600×900 PNGs, logs and `captures.json`, validates decoded
image content, rejects GPU errors and compares a second aerial PNG byte for
byte. Each capture settles for 48 fixed-time frames at 60 Hz, seed `83`, 512²
material textures, MSAA 4×, TAA, SSAO, cascaded shadows, bloom and corrected FXAA.
The script selects an empty asset directory so optional downloads cannot change
the result. Identical output is checked on the same executable/GPU; it is not a
cross-platform pixel determinism promise.

| View / key | Position (m) | Target (m) | Comparison criterion |
|---|---|---|---|
| `aerial` / 1 | −175, 300, 310 | 75, 32, −80 | Layered skyline, multiple tower families, connected canal and terrace blocks |
| `civic` / 2 | −26, 3, 111 | 4, 34, −45 | Ring and dome, basin, stairs, planted pedestrian edges |
| `street` / 3 | 49, 3, 140 | 25, 15, 28 | Sheltered frontage, bronze columns, planting and paving joints beneath the skyline |
| `terrace` / 4 | 84, 16, 145 | −35, 34, −55 | Ceramic structural member, bronze node, garden and railing with city beyond |
| `blue-hour` | Civic camera | Civic target | Warm civic lighting against cool atmospheric glass |

All views use a 55° vertical field of view and exposure bias 0. The built-in
`--sky studio` supplies a fixed late-afternoon cloud environment, with sun
direction normalized from `(−0.62, 0.48, 0.62)`. `--night` uses a separate fixed
blue-hour environment. `--cam x,y,z --target x,y,z` overrides the named camera.
An individual capture can use `--hidden --shot terrace --capture terrace.png
--frames 48`. Capture failures return a nonzero exit code.

## Rendering changes

The copy retains reversed Z, four tower detail levels, interior mapping and
GPU culling. Its additions are height-dependent atmospheric depth, longer shadow
coverage, ceramic/bronze/petrol material tuning, subtler stone variation, broad
damp patches, stable material-noise hashing, and built-in clouds and blue hour.
FXAA now measures edge distances along the correct axis and guards its divisor;
this removes the edge corruption seen in the inherited pass. Environment cleanup
checks shared ownership before release. The authored layout replaces the copied
city-size generator; its old size/showcase options are intentionally absent.

## Validation and limits

The headless scene check validates finite vertices, material/index bounds,
complete disjoint draw ranges, four levels per tower, density, light budget and
camera validity. Useful GPU checks:

```sh
build/demos/human_tech_demo/human_tech_demo --hidden --shot street \
  --width 1600 --height 900 --tex-size 512 --sweep 24 --sweep-step 0.12 \
  --sweep-dir forward --sweep-blur 2 --sweep-out build/street-motion
build/demos/human_tech_demo/human_tech_demo --hidden --shot terrace \
  --width 960 --height 540 --tex-size 256 --stress 12
```

The comparison target is the UM human-tech metropolis concept collection of
2026-09-06. Similarity is qualitative: the architectural vocabulary and connected
city composition are represented, while film-quality foliage, transparent
furnished rooms, local reflections in water/glass, transport activity and the
galaxy panorama remain outside this prototype. Interiors use parallax shading;
reflections use the sky environment. The finite authored set retains repeated
blocks and is not an approved game city plan. Approximately 20.4 million resident
triangles include alternative LODs; this is a visual benchmark, not a streaming
or memory optimization milestone.

Source and licensing: [assets/PROVENANCE.md](assets/PROVENANCE.md).
