# infinity

A fully procedural universe, computed from a single 128-bit seed — no stored
world data, deterministic across platforms, player changes as a diff overlay.
Current stage: prototype v0, a single planet (fly in from orbit, land, walk,
dig).

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
