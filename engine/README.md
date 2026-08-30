# infinity-engine

Self-contained engine underneath the game "Infinity": deterministic
procedural-universe framework, world streaming machinery, and a thin GPU
layer. Designed to become its own repository (included as a submodule)
without changes — nothing in here includes or links anything outside
`engine/`.

| Library | Contents | Constraints |
|---|---|---|
| `engine-core` | det:: numerics (fixed-point, controlled reals, bit-exact trig), Philox keys, golden hashing, WorldTime/WorldClock, Kepler/ephemeris math, the InfinityTree framework (Address, Node, AxisView, materializer, InceptionStore) | headless, deterministic, no external deps |
| `engine-world` | cube-sphere geometry, gradient noise, chunk grids, Transvoxel meshing, chunk/LOD streaming over a game-provided `ChunkSampler` | headless, depends on core |
| `engine-render` | RHI on wgpu-native (Vulkan/Metal), render math | client-only |

Rules of the house:

- The deterministic sim pair (core + world) is ONE lib set — a future
  game server links exactly these, never render.
- No OS clock reads outside `core/time/`; no `std::random` anywhere; no
  naked `new`/`delete`; 128-bit keys end to end.
- The tree is never stored: address = identity = seed source = diff key.

Build standalone:

```sh
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
```
