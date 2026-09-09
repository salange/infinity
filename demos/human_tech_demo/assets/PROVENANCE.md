# Asset provenance

The default scene and documented captures require no downloaded assets.

| Material | Editable source | Origin / license |
|---|---|---|
| Tower families, civic hall, ring, trees and building kit | `game/city/src/` | Existing project geometry; repository BSD-3-Clause license |
| District composition, arcades, storefront, bridges, gardens and structural details | `../src/scene.cpp` | Authored for this demo; repository BSD-3-Clause license |
| Ceramic, stone and brushed-metal fallback textures; leaf texture | `../src/textures.cpp` | Derived from cityblock's source; repository BSD-3-Clause license |
| Cloud environment and blue-hour lighting | `../src/ibl.cpp` | Authored directional noise and lighting; repository BSD-3-Clause license |
| Architectural art direction | UM `design/concepts/cities/human/tech/metropolis-2026-09-06/` | Reference only; no concept pixels are embedded as scene assets |

The scene's editable asset source is C++; no external mesh, Blender file or
raster asset was added. The copied `manifest.json` and `../tools/fetch-assets.py`
retain the optional ambientCG texture IDs and Poly Haven HDRI URLs, licensed
CC0-1.0 by their providers. The fetcher records download hashes in the ignored
`fetched.json`; downloaded files remain ignored. They change appearance when
selected and are excluded from the representative capture recipe.

```sh
uv run --no-project --python 3.12 python demos/human_tech_demo/tools/fetch-assets.py
```

These optional downloads were unavailable during the implementation checks;
no claim about their rendered appearance is included in the capture evidence.
