# Authored human-tech kit

The standalone demo's reusable detail terminals are modelled by
`tools/author-kit-blender.py` in Blender 5.2. Geometry is project-authored and
uses the repository's license. There are no downloaded meshes, image textures,
scene-wide lightmaps, stored game-world data, or external generation services.

Build with:

```
uv run --script demos/human_tech_demo/tools/build-assets.py
```

This creates ignored `assets/generated/human_tech_kit.blend`, `.glb`, `.htkit`,
`manifest.json`, and an authoring log. It uses a separate background Blender
process with factory startup, leaving the open Blender session untouched.
The blend file retains individually editable construction components beneath
named resource roots. The script reproduces every component and material.
The raw Blender export is retained as `.authoring.glb`. A canonicalizer stabilizes
vertex, triangle, material and resource order with 10-micrometre coordinate
precision; `.glb` is the stable interchange source; the versioned native file is only its
validated runtime representation. Generated data can be deleted and rebuilt.

`authored-kit-manifest.json` records the reviewed recipe, glTF, runtime and
editable source hashes, material factors and resource bounds. Blender save
metadata can change a `.blend` file hash; runtime content is checked separately.
`--compile-only` rebuilds the runtime file from the existing glTF without Blender.

The same build also creates a separate `arrival_towers` library, containing the
six concept01 near-station shafts and the graphite tower's rounded socket.
`arrival-towers-manifest.json` pins its recipes and derivative hashes. Each shaft
is a complete editable mesh assembly with physical floors, facade relief, core
and crown. `build-arrival-towers.py` rebuilds this addition independently.
The native city appends its named resources and materials to the unchanged base
library, retaining both source hashes and rejecting duplicate resource names.
Occupied-glass metadata explicitly carries the room width, height, depth,
lighting probability and tint. It is distinct from the thin-sheet optical model.

## Coordinates and representation

Architectural measurements are metres at 1:1 scale. The authoring helpers
convert Y-up metre coordinates to Blender Z-up; the glTF exporter converts
back to Y-up. Root/resource transforms, nested transforms, non-uniform scale,
mirrored triangle winding, and inverse-transpose normals are compiled explicitly.
Meshes preserve normals, tangent handedness, metric UVs, material IDs and bounds.
Each named resource is uploaded once. City placements carry resource ID,
translation, yaw, scale and linear tint; they never expand the original mesh.
Ten additional `_mid` vegetation resources simplify subpixel leaf curvature
and branch geometry; they retain the same pivots and materials. Instances use
the appropriate resource without copying the full mesh for each plant.
The three broadleaf tree families use 14–24 cm asymmetric curved blades and
overlapping sprays, preserving a dense crown at close viewing distance.

The west garden derives `canopy_broadleaf_garden_lean` by a proper local
rotation of the full broadleaf mesh. Positions, normals and tangent directions
rotate together; winding and handedness remain unchanged. It also derives
`fern_arching_dry` with identical geometry, bounds, indices and RGB, remapping
only `leaf_middle` and `leaf_sunlit` to named dry-fern materials with roughness
0.67. This explicit species-specific surface variant leaves the kit and shared
waxy plant materials unchanged. Both derivatives upload once and use ordinary
world-space instances. The supported ceramic garden frame is authored by the
separate demo frame helper; the original kit junction remains available.

The garden also builds `garden_fig_canopy` and `garden_strelitzia` from
`src/garden_canopy.cpp`, using fixed root streams and the kit's bark and leaf
materials. The fig has curved tapered wood, six major boughs, overlapping
secondary crowns, hanging sprays and individually folded 14–24 cm leaves.
The broad crown is grown from branch geometry; all instance scales are uniform.
Its maintained clearance volume corresponds to the actual nearby ceramic blade
at the declared garden root/yaw, and prunes entire sprays before meshing.
The strelitzia adds 15 petioles and folded 0.72–1.12 m blades with modelled
midribs. Both are reproducible native resources and upload once; they do not
modify the Blender kit or its recorded source hashes. Their ground-centre pivots
use the same metre/Y-up, normal, tangent, UV and material contract.

The roof helper derives `roof_solar_rack` from the existing solar panel with a
12-degree tilt and adds actual posts, pads, rails and electrical connections.
The authoring source panel remains unchanged. Other roof/market staging
resources are reproducible procedural assemblies declared in their demo C++
helpers; they use the same mesh/material/instance contract.

Resource origins are ground centres except `ceramic_lattice_node` (junction
centre), `climber_cascade` (planter lip, leaves extend down), and `pendant_lamp`
(ceiling attachment). A rail is 4 m long and 1.26 m high, an entry door is 3 m
high, and the furnished lounge is 8 by 7 m. Per-resource bounds in the manifest
are the authoritative exported dimensions.

## Material contract

The kit intentionally uses exportable Principled base colour, roughness,
metallic, emission and dielectric transmission factors. These glTF factors
are linear; no second gamma conversion is performed. Clear glazing uses
IOR 1.5 and the demo's thin-transmission material bit 128. Its IOR, transmission,
alpha and optical thickness are mapped to the four otherwise unused room fields
for this material type; all four are retained in the manifest. Botanical bit 256
adds transmission through modelled leaves without applying an alpha image. Architectural glass
has no fabricated room image; the separate furnished lounge provides real
foreground interior geometry. Satin bronze is metallic; glass, ceramic,
leaves and soil are dielectric. Leaves have modelled curved silhouettes and
explicit back faces for consistent raster, shadow and indirect-light passes.

Manufactured bevels, panel seams, gaskets, screw heads, timber slats, growth
scars, branch scaffolds, frond leaflets, soil and mulch are actual geometry.
The source contains no Blender-only procedural shader that could be silently
lost in export. This kit requires no bitmap maps. `engine_albedo_set` can name
an explicitly supported demo texture set; unknown glTF image map bindings,
custom specular factors, alpha masking, material extensions, animation,
skinning and morphs are currently rejected. Adding those features requires
an explicit compiler and renderer mapping, not an untextured substitute.

## Validation

The compiler validates all required vertex attributes, bounded accessors,
triangle indices, transforms, material ranges, required extensions and named
resource roots. The binary carries source and payload SHA-256 hashes; the
runtime verifies integrity before upload, normal lengths, material IDs,
geometry limits and resource names. Missing required files or resources produce
a descriptive error. The separately selected procedural-only demo mode is the
legible fallback; it is never used silently for visual acceptance.

`tests/test_gltf_compiler.py` checks independent malformed and transformed glTF
fixtures. `human_tech_asset_test <kit>` checks the actual resource library,
metre/Y-up orientation, material remapping, instance transforms and rejection
of a corrupted payload. These data checks accompany engine visual reviews;
they do not establish visual parity by themselves.

The First Arrival tower library uses native occupied-room coordinates in metres:
U follows the facade and V equals the resource-local floor height, increasing
upward. Its Blender authoring compensates glTF's V conversion only for these
procedural occupied-glass charts; ordinary textured materials retain their
standard UV path. The imported geometry check verifies signed floor alignment
and an upward tangent basis, as well as physical floor spacing. Interior tint
uses the scene's warm room-light palette independently of pane transmission.
