# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Export the current native facade generator into an editable Blender scene.

Run with uv. No renderer, source kit, desktop scene or installed add-on is changed.
The generated mesh is the native production surface, including its normals.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys


def blender_stage(output: Path) -> None:
    import bpy
    from mathutils import Vector
    if not bpy.app.background:
        raise RuntimeError("The inspection exporter requires a separate background Blender process")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    vertices, normals, faces, materials = [], [], [], []
    active = 0
    origin = Vector((-345.0, 260.0, 405.0))
    for line in (output / "diagrid.obj").read_text().splitlines():
        fields = line.split()
        if fields[0] == "v":
            p = Vector(tuple(map(float, fields[1:]))) - origin
            vertices.append((p.x, -p.z, p.y))
        elif fields[0] == "vn":
            p = tuple(map(float, fields[1:]))
            normals.append((p[0], -p[2], p[1]))
        elif fields[0] == "g":
            active = int(fields[1].split("_")[-1])
        elif fields[0] == "f":
            faces.append(tuple(int(p.split("/")[0]) - 1 for p in fields[1:]))
            materials.append(active)
    mesh = bpy.data.meshes.new("Native formed diagrid profile and joined castings")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new("Production diagrid — editable native mesh", mesh)
    bpy.context.collection.objects.link(obj)
    obj["generator"] = "demos/human_tech_demo/src/sculpted_diagrid.cpp"
    obj["units"] = "metres; original world origin stored separately"
    obj["native_origin_y_up"] = list(origin)
    obj["inspection_limits"] = "Geometry and normals exact; native material maps and transport not reproduced."
    for description in json.loads((output / "materials.json").read_text()):
        material = bpy.data.materials.new(description["name"])
        material.use_nodes = True
        shader = material.node_tree.nodes.get("Principled BSDF")
        shader.inputs["Base Color"].default_value = (*description["color"], 1)
        shader.inputs["Metallic"].default_value = description["metallic"]
        shader.inputs["Roughness"].default_value = description["roughness"]
        mesh.materials.append(material)
    for polygon, material in zip(mesh.polygons, materials):
        polygon.material_index = material
        polygon.use_smooth = True
    mesh.normals_split_custom_set([normals[loop.vertex_index] for loop in mesh.loops])
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.render.resolution_x = 1280
    scene.render.resolution_y = 720
    scene.render.resolution_percentage = 100
    center = sum((Vector(v) for v in vertices), Vector()) / len(vertices)
    for name, offset in [("Frontal inspection", (42, -62, 3)), ("Oblique inspection", (76, -13, 4))]:
        data = bpy.data.cameras.new(name)
        camera = bpy.data.objects.new(name, data)
        bpy.context.collection.objects.link(camera)
        camera.location = center + Vector(offset)
        camera.rotation_euler = (center - camera.location).to_track_quat("-Z", "Y").to_euler()
        data.lens = 45
        scene.camera = camera
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "diagrid-inspection.blend"))
    # Saving has completed. Avoid desktop audio/add-on shutdown callbacks in
    # this isolated, noninteractive export process.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--blender", default="blender")
    parser.add_argument("--blender-stage", action="store_true")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else None)
    output = args.output.resolve()
    if args.blender_stage:
        blender_stage(output)
        return
    game = args.game.resolve()
    output.mkdir(parents=True, exist_ok=True)
    includes = ["demos/human_tech_demo/src", "game/city/include", "engine/core/include", "game/gen/include", "engine/world/include", "build/game/gen/generated"]
    sources = ["demos/human_tech_demo/tools/export-diagrid-inspection.cpp", "demos/human_tech_demo/src/sculpted_diagrid.cpp"]
    libraries = ["game/city/libgame_city.a", "game/gen/libgame_gen.a", "engine/world/libengine_world.a", "engine/core/libengine_core.a"]
    binary = output / "export-diagrid-inspection"
    subprocess.run(["c++", "-std=c++20", "-O1", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", *["-I" + str(game / p) for p in includes], *[str(game / p) for p in sources], "-Wl,--gc-sections", *[str(game / "build" / p) for p in libraries], "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(output)], check=True)
    manifest = {"generator_sha256": {p: hashlib.sha256((game / p).read_bytes()).hexdigest() for p in sources}, "mesh_sha256": hashlib.sha256((output / "diagrid.obj").read_bytes()).hexdigest(), "units": "metres", "native_geometry": True, "native_material_transport": False}
    (output / "inspection-provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run([args.blender, "--factory-startup", "-noaudio", "-b", "-t", "2", "--python", str(Path(__file__).resolve()), "--", "--blender-stage", "--output", str(output)], check=True)


if __name__ == "__main__":
    main()
