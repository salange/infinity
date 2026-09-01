# infinity

![The galactic band rising over a night-side ocean, a moon hanging in the
dust lanes — rendered live, generated from the seed](docs/deep-sky.png)

A fully procedural universe, computed from a single 128-bit seed — no stored
world data, deterministic across platforms, player changes as a diff overlay.
Current stage: planetary systems inside a procedural galaxy. Fly in from
orbit, land, walk, dig; hold J to jump the ship to a neighbouring star and
watch the whole sky change. The night sky above is computed, not painted:
the Milky Way band is a line integral of the galaxy's density model, the
dust rift is its extinction term, every star is a system you can visit, and
HDR eye adaptation opens it all up when you fly into a planet's shadow.

## Build

Requirements: CMake ≥ 3.24, a C++20 compiler, ninja (recommended). On Linux
additionally the usual Wayland/X11 development headers (for GLFW).
Dependencies (GLFW, wgpu-native, doctest) are fetched and pinned by CMake.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
```

Targets:

- `app/infinity` — windowed app (wgpu-native: Vulkan on Linux/Windows,
  Metal on macOS). Starts fullscreen on the primary monitor; `--windowed`
  keeps a 1280x720 window. `--frames N` renders N frames and exits
  (smoke testing).
- `cli/infinity-cli` — headless tool (generation, hashing, determinism
  checks). Never links window or GPU libraries.
- `tests/infinity_tests` — unit tests (doctest, via ctest).

`ci/check.sh` runs the full local gauntlet: configure, build, lint gates,
tests, smoke runs.

Headless-only build (no window/GPU dependencies at all):

```sh
cmake -B build-headless -DINFINITY_BUILD_APP=OFF
```

## Layout

| Module | Contents |
|---|---|
| `core/` | deterministic math, RNG, keys, chunk addressing |
| `gen/` | planet parameters, provinces, density pipeline, meshing |
| `world/` | chunk manager, LOD, diff overlay, effective-state API |
| `sim/` | player controller, input |
| `render/` | thin RHI (wgpu-native), shaders, mesh upload |
| `app/` | windowed entry point |
| `cli/` | headless entry point |
| `tests/` | unit tests |

Rule of the house: `core`/`gen`/`world`/`cli` never depend on a window,
GPU, or engine. Rendering is a view of the world, not part of it.
