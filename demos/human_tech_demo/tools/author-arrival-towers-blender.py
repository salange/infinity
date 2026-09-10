"""Create the seven editable near-station resources in private background Blender."""
import json
import math
import os
from pathlib import Path
import sys
import bpy
from mathutils import Vector

if not bpy.app.background:
    raise RuntimeError('Use a separate background Blender process for asset authoring')
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent))
from arrival_tower_geometry import materials
import arrival_hero_tower
import arrival_left_towers
import arrival_right_towers

args=sys.argv[sys.argv.index('--')+1:]
out=Path(args[0]).resolve();out.mkdir(parents=True,exist_ok=True)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
scene=bpy.context.scene;scene.name='First Arrival Editable Tower Library'
scene.unit_settings.system='METRIC'
materials();resources=[];specs={}
for module in (arrival_hero_tower,arrival_left_towers,arrival_right_towers):
    specs.update(module.SPEC)
    resources.extend(module.build())
for resource in resources:resource.finish()
for obj in scene.objects:obj.select_set(True)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'arrival_towers.blend'),check_existing=False)
bpy.ops.export_scene.gltf(filepath=str(out/'arrival_towers.authoring.glb'),export_format='GLB',
    use_selection=True,use_active_scene=True,export_yup=True,export_apply=True,
    export_texcoords=True,export_normals=True,export_tangents=True,export_materials='EXPORT',
    export_extras=True,export_cameras=False,export_lights=False,export_animations=False,export_unused_images=False)
(out/'arrival-towers-spec.json').write_text(json.dumps(specs,indent=2)+'\n')

if '--inspect' in args:
    for resource in resources:
        spec=specs[resource.name];center=spec['center']
        x,z=(center[0],center[-1]);y=center[1] if len(center)==3 else spec.get('base_y',13.2)
        resource.root.location=(x,-z,y)
        resource.root.rotation_euler.z=-spec['yaw']
    bpy.ops.mesh.primitive_plane_add(size=1800,location=(100,-150,.60))
    ground=bpy.context.object;ground.name='Inspection ground only'
    mat=bpy.data.materials.new('Inspection warm grey ground');mat.diffuse_color=(.27,.28,.27,1)
    ground.data.materials.append(mat)
    world=scene.world;world.use_nodes=True
    world.node_tree.nodes.get('Background').inputs['Color'].default_value=(.46,.53,.65,1)
    world.node_tree.nodes.get('Background').inputs['Strength'].default_value=.65
    sun=bpy.data.lights.new('Inspection late sun','SUN');sun.energy=3;sun.angle=.06;sun.color=(1,.82,.60)
    light=bpy.data.objects.new(sun.name,sun);scene.collection.objects.link(light)
    light.rotation_euler=(math.radians(49),math.radians(-23),math.radians(-45))
    camera=bpy.data.objects.new('Unchanged First Arrival camera',bpy.data.cameras.new('First Arrival'))
    scene.collection.objects.link(camera)
    camera.location=(-292.481,-591.547,228.538)
    target=Vector((119.189,289.079,-6.035))
    camera.rotation_euler=(target-camera.location).to_track_quat('-Z','Y').to_euler()
    camera.data.sensor_fit='VERTICAL';camera.data.sensor_height=24
    camera.data.lens=24/(2*math.tan(math.radians(46)*.5));camera.data.clip_end=10000
    scene.camera=camera;scene.render.engine='CYCLES';scene.cycles.samples=12
    scene.cycles.use_denoising=True;scene.cycles.max_bounces=4
    scene.render.resolution_x=1280;scene.render.resolution_y=720;scene.render.resolution_percentage=100
    scene.render.image_settings.file_format='PNG';scene.render.filepath=str(out/'arrival-towers-blender-inspection.png')
    bpy.ops.wm.save_as_mainfile(filepath=str(out/'arrival-towers-layout.blend'),check_existing=False)
    bpy.ops.render.render(write_still=True)
print('Authored',len(resources),'First Arrival resources',flush=True)
sys.stdout.flush();sys.stderr.flush();os._exit(0)
