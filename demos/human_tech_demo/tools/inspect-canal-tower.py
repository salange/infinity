# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Export exact production tower parts into a separate editable Blender file."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys


def blender_stage(output):
    import bpy
    from mathutils import Vector
    if not bpy.app.background:
        raise RuntimeError("Use a separate background Blender process for this inspection")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    metadata = json.loads((output / "geometry.json").read_text())
    descriptions = json.loads((output / "materials.json").read_text())
    origin = Vector(metadata["native_origin_y_up"])
    vertices, normals, groups = [], [], {}
    active = 0
    for line in (output / "canal-tower.obj").read_text().splitlines():
        fields = line.split()
        if fields[0] == "v":
            p = Vector(tuple(map(float, fields[1:]))) - origin
            vertices.append((p.x, -p.z, p.y))
        elif fields[0] == "vn":
            x, y, z = map(float, fields[1:])
            normals.append((x, -z, y))
        elif fields[0] == "g":
            active = int(fields[1].split("_")[-1])
        elif fields[0] == "f":
            groups.setdefault(active, []).append(tuple(int(v.split("/")[0]) - 1 for v in fields[1:]))
    for material_id, faces in groups.items():
        description = descriptions[material_id]
        used = sorted({v for face in faces for v in face})
        remap = {v: i for i, v in enumerate(used)}
        mesh = bpy.data.meshes.new(description["name"])
        mesh.from_pydata([vertices[i] for i in used], [], [tuple(remap[i] for i in face) for face in faces])
        mesh.update()
        mesh.normals_split_custom_set([normals[used[loop.vertex_index]] for loop in mesh.loops])
        obj = bpy.data.objects.new(description["name"], mesh)
        bpy.context.collection.objects.link(obj)
        obj["generator"] = "demos/human_tech_demo/src/canal_tower.cpp"
        obj["native_origin_y_up"] = list(origin)
        obj["inspection_limits"] = "Exact native geometry/normals; simplified Blender material and lighting."
        material = bpy.data.materials.new(description["name"])
        material.use_nodes = True
        shader = material.node_tree.nodes.get("Principled BSDF")
        shader.inputs["Base Color"].default_value = (*description["color"], 1)
        shader.inputs["Metallic"].default_value = description["metallic"]
        shader.inputs["Roughness"].default_value = description["roughness"]
        mesh.materials.append(material)
        for face in mesh.polygons:
            face.use_smooth = True
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 24
    scene.render.resolution_x = 960
    scene.render.resolution_y = 960
    scene.render.resolution_percentage = 100
    scene.world.color = (.24, .24, .24)
    light = bpy.data.lights.new("Warm broad inspection light", "AREA")
    light.energy, light.shape, light.size = 75000, "DISK", 65
    obj = bpy.data.objects.new(light.name, light)
    bpy.context.collection.objects.link(obj)
    obj.location = (40, -50, 145)
    obj.rotation_euler = (Vector((0, 0, 75)) - obj.location).to_track_quat("-Z", "Y").to_euler()
    for name, focus, position, lens in [
        ("Whole tower", (0, 0, 53), (-65, -145, 114), 44),
        ("Raised pavilion and open crown", (0, 0, 94), (-48, -105, 142), 65),
    ]:
        camera = bpy.data.objects.new(name, bpy.data.cameras.new(name))
        bpy.context.collection.objects.link(camera)
        camera.location = position
        camera.rotation_euler = (Vector(focus) - camera.location).to_track_quat("-Z", "Y").to_euler()
        camera.data.lens = lens
        scene.camera = camera
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "canal-tower-inspection.blend"))
    scene.render.filepath = str(output / "crown-inspection.png")
    bpy.ops.render.render(write_still=True)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0)


def main():
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
    (output / "temp").mkdir(exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(output / "temp"))
    includes = ["demos/human_tech_demo/src", "game/city/include", "engine/core/include", "game/gen/include", "engine/world/include", "build/game/gen/generated"]
    command = ["c++", "-std=c++20", "-O1", "-ffunction-sections", "-fdata-sections"]
    command += [item for name in includes for item in ("-I", str(game / name))]
    command += [str(game / "demos/human_tech_demo/tools/export-canal-tower.cpp"), str(game / "demos/human_tech_demo/src/canal_tower.cpp"),
                str(game / "build/game/city/libgame_city.a"), str(game / "build/engine/core/libengine_core.a"), "-Wl,--gc-sections", "-o", str(output / "export-tower")]
    subprocess.run(command, check=True, env=environment)
    subprocess.run([str(output / "export-tower"), str(output)], check=True, env=environment)
    subprocess.run([args.blender, "--background", "--factory-startup", "--threads", "2", "--python", str(Path(__file__).resolve()), "--", "--blender-stage", "--output", str(output)], check=True, env=environment)


if __name__ == "__main__":
    main()
