# Asset provenance

| Resource | Editable or reproducible source | Origin |
|---|---|---|
| Base tower families, civic hall, ring and building primitives | `game/city/src/` | Existing project geometry, repository license |
| Coastline, districts, parcels, foreground facade, gardens and detail placements | `../src/scene.cpp` | Project-authored C++ scene, repository license |
| Panelled civic dome, occupied colonnade and sculpted ring | `../src/civic_landmarks.cpp` | Project-authored metric geometry, repository license |
| Sculpted garden outrigger and flush bronze journal | `../src/garden_frame.cpp` | Project-authored metric casting and curved panel geometry, repository license |
| Low planted market island, retained soil, irrigation and recessed lighting | `../src/street_forecourt.cpp` | Project-authored metric construction and supported botanical placements, repository license |
| Stocked corner market, cafe and static products | `../src/market_staging.cpp` | Project-authored reusable geometry and placements, repository license |
| Curved landing pavilion furnishings, library shelves and lighting | `../src/landing_lounge.cpp` | Project-authored reusable furniture, metric geometry and placements, repository license |
| Architectural and botanical mesh library | `../tools/author-kit-blender.py` | Project-authored Blender construction, repository license |
| Native kit | `../tools/build-assets.py`, `authored-kit-manifest.json` | Validated canonical glTF derivative of the editable Blender source |
| Ceramic, stone, soil, bark and brushed-metal material maps | `../src/textures.cpp` | Project-authored generated surface library, repository license |
| Reviewed marble, paving and terrazzo maps | `surface-manifest.json`; UM `design/assets/human-tech-materials/` | ambientCG Marble012, PavingStones136 and Tiles043, unchanged source JPGs, CC0-1.0 |
| Wide and close-view sky-only textures | `sky-manifest.json`; UM `design/assets/human-tech-metropolis/` | Project-authored immutable environment assets; source hashes and authoring record retained in UM |
| Architectural art direction and source sky framing | UM `design/concepts/cities/human/tech/metropolis-2026-09-06/` | Architecture is a modelling reference; sky-only derivatives are declared in the sky manifest. No concept-city pixels are embedded as scene assets |

The generated kit contains individually editable objects in a `.blend`, a raw
Blender glTF export, a canonical glTF export and a validated runtime file. These
build products are ignored and reproducible; their source recipes and reviewed
hashes are tracked. No external modelling service, paid mesh library or downloaded
vegetation is required. Sky sources contain no city, terrain or water and are
imported only after SHA-256 verification. They supply the environment surrounding
the independently rendered 3D city.

The reviewed surface manifest pins three ambientCG source archives and every
installed file. `../tools/import-surface-assets.py` verifies their SHA-256 hashes
before installing them. Their color, OpenGL normal, roughness, height and optional
occlusion maps retain the source pixels and channel encodings. The source licence
is [CC0-1.0](https://docs.ambientcg.com/license/). No provider account is required.
The copied generic `manifest.json` and `../tools/fetch-assets.py` retain additional
optional ambientCG texture IDs and Poly Haven HDRI URLs. Only the three selected
sets are included in this iteration's reviewed production recipe.

Scene-wide baked lighting and stored procedural game-world data are not required.
Blender diagnostic renders are labelled separately and excluded from engine
visual acceptance.
