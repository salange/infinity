"""Render the same lookdev.scene assembly as the live engine using Cycles.

This independent integrator diagnoses shape/material/lighting differences; it
is never substituted for the live engine's visual acceptance captures.
"""
import bpy
import json
import hashlib
import math
from mathutils import Vector
from pathlib import Path
import sys
args=sys.argv[sys.argv.index('--')+1:]
folder=Path(args[0]).resolve();manifest=Path(args[1]).resolve() if len(args)>1 else folder.parent/'lookdev.scene'
output=Path(args[2]).resolve() if len(args)>2 else folder/'lookdev-cycles.png'
bpy.ops.wm.open_mainfile(filepath=str(folder/'human_tech_kit.blend'))
source=bpy.data.scenes['Human Tech Resource Library']
review=bpy.data.scenes.new('Matched lookdev diagnostic');bpy.context.window.scene=review
exposure=1.;camera=None;records=[];notes=[]

def B(p):return (p[0],-p[2],p[1])
def place(name,position,yaw,scale):
    original=source.objects.get(name)
    if original is None:raise ValueError('unknown resource '+name)
    parent=bpy.data.objects.new(name+'_review',None);review.collection.objects.link(parent)
    parent.location=B(position);parent.rotation_euler.z=math.radians(yaw);parent.scale=(scale[0],scale[2],scale[1])
    for old in original.children:
        obj=old.copy();review.collection.objects.link(obj);obj.parent=parent

for number,line in enumerate(manifest.read_text().splitlines(),1):
    fields=line.split()
    if not fields or fields[0].startswith('#'):continue
    kind=fields[0]
    if kind=='camera':
        v=list(map(float,fields[1:]));assert len(v)==7
        data=bpy.data.cameras.new('Matched camera');camera=bpy.data.objects.new('Matched camera',data);review.collection.objects.link(camera)
        camera.location=B(v[:3]);camera.rotation_euler=(Vector(B(v[3:6]))-camera.location).to_track_quat('-Z','Y').to_euler()
        data.sensor_fit='VERTICAL';data.sensor_height=24;data.lens=12/math.tan(math.radians(v[6])*.5);review.camera=camera
    elif kind=='instance':
        v=list(map(float,fields[2:]));assert len(v)==7;place(fields[1],v[:3],v[3],v[4:])
    elif kind=='box':
        v=list(map(float,fields[2:]));assert len(v)==6
        bpy.ops.mesh.primitive_cube_add(size=1,location=B(v[:3]));obj=bpy.context.object;obj.name='Matched terrazzo floor'
        obj.dimensions=(2*v[3],2*v[5],2*v[4]);bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
        if fields[1]=='terrazzo':
            material=bpy.data.materials.new('Matched terrazzo base');material.use_nodes=True
            p=material.node_tree.nodes.get('Principled BSDF');p.inputs['Base Color'].default_value=(1,1,1,1);p.inputs['Roughness'].default_value=.35
            notes.append('Terrazzo uses the same base factor/roughness; engine procedural texture maps are not reproduced.')
        else:
            material=bpy.data.materials.get(fields[1])
            if material is None:raise ValueError('unmapped authored box material '+fields[1])
        obj.data.materials.append(material)
    elif kind=='light':
        v=list(map(float,fields[1:]));assert len(v)==8
        data=bpy.data.lights.new('Matched practical','POINT');obj=bpy.data.objects.new('Matched practical',data);review.collection.objects.link(obj)
        obj.location=B(v[:3]);data.color=v[4:7];data.energy=v[7]*4*math.pi*.12;data.shadow_soft_size=.10
        notes.append('Practical radiant power matches the daytime 0.12 factor and inverse-square falloff; Cycles lacks the engine finite-radius window and +1 distance softening.')
    elif kind=='sun':
        v=list(map(float,fields[1:]));assert len(v)==6
        data=bpy.data.lights.new('Matched finite sun','SUN');obj=bpy.data.objects.new('Matched finite sun',data);review.collection.objects.link(obj)
        obj.rotation_euler=Vector(B(v[:3])).normalized().to_track_quat('Z','Y').to_euler();peak=max(v[3:]);data.energy=peak;data.color=tuple(x/peak for x in v[3:]);data.angle=math.radians(.53)
    elif kind=='exposure':exposure=float(fields[1])
    else:raise ValueError(f'unknown record at {number}: {kind}')
    records.append(line)
if camera is None:raise ValueError('lookdev camera missing')

world=bpy.data.worlds.new('Matched relative-linear environment');review.world=world;world.use_nodes=True
nodes=world.node_tree.nodes;links=world.node_tree.links
texture=nodes.new('ShaderNodeTexEnvironment');texture.image=bpy.data.images.load(str(folder/'sky-sunset-v1.png'));texture.image.colorspace_settings.name='sRGB'
coords=nodes.new('ShaderNodeTexCoord');mapping=nodes.new('ShaderNodeMapping')
# Blender's equirectangular meridian is +X. Engine +Y-up/-Z-forward converts
# to +Z-up/+Y-forward here; subtract 90 degrees and the engine's 45-degree yaw.
mapping.inputs['Rotation'].default_value[2]=math.radians(-135)
links.new(coords.outputs['Generated'],mapping.inputs['Vector']);links.new(mapping.outputs['Vector'],texture.inputs['Vector'])
links.new(texture.outputs['Color'],nodes['Background'].inputs['Color']);nodes['Background'].inputs['Strength'].default_value=1.

review.render.engine='CYCLES';review.cycles.device='CPU';review.cycles.samples=64;review.cycles.use_denoising=True
review.render.threads_mode='FIXED';review.render.threads=4
review.render.resolution_x=1280;review.render.resolution_y=720;review.render.resolution_percentage=100
review.render.image_settings.file_format='PNG';review.render.filepath=str(output)
# Match the engine's fitted ACES display curve after linear exposure; Blender
# Standard then performs sRGB encoding. Bloom, vignette and temporal AA differ.
review.view_settings.view_transform='Standard';review.view_settings.look='None';review.view_settings.exposure=0.;review.view_settings.gamma=1.
tree=bpy.data.node_groups.new('Matched display transform','CompositorNodeTree');review.compositing_node_group=tree
tree.interface.new_socket(name='Image',in_out='OUTPUT',socket_type='NodeSocketColor');nodes=tree.nodes;links=tree.links
layers=nodes.new('CompositorNodeRLayers');sep=nodes.new('CompositorNodeSeparateColor');sep.mode='RGB';links.new(layers.outputs['Image'],sep.inputs['Image'])
combine=nodes.new('CompositorNodeCombineColor');combine.mode='RGB'
def operation(op,a,b):
    node=nodes.new('ShaderNodeMath');node.operation=op
    for i,value in enumerate((a,b)):
        if isinstance(value,(float,int)):node.inputs[i].default_value=value
        else:links.new(value,node.inputs[i])
    return node.outputs[0]
for i in range(3):
    x=operation('MULTIPLY',sep.outputs[i],exposure)
    numerator=operation('MULTIPLY',x,operation('ADD',operation('MULTIPLY',x,2.51),.03))
    denominator=operation('ADD',operation('MULTIPLY',x,operation('ADD',operation('MULTIPLY',x,2.43),.59)),.14)
    mapped=operation('DIVIDE',numerator,denominator);mapped.node.use_clamp=True;links.new(mapped,combine.inputs[i])
composite=nodes.new('NodeGroupOutput');links.new(combine.outputs['Image'],composite.inputs['Image'])
notes.append('Display uses engine fitted ACES, linear exposure and sRGB; bloom, vignette and temporal-AA passes are intentionally excluded from this integrator comparison.')
notes.append('Leaf Principled materials retain authored factors; the engine botanical wrap/transmission model differs from the offline surface integrator.')
kit_identity=json.loads((folder/'manifest.json').read_text())
output.with_suffix('.json').write_text(json.dumps({'assembly':manifest.name,'assembly_sha256':hashlib.sha256(manifest.read_bytes()).hexdigest(),'kit_source_sha256':kit_identity['source_sha256'],'kit_runtime_sha256':kit_identity['runtime_sha256'],'records':records,'resolution':[1280,720],'integrator':'Blender 5.2 Cycles CPU, 64 samples, denoised','exposure_linear':exposure,'comparison_limits':notes},indent=2)+'\n')
if '--dry-run' not in args:bpy.ops.render.render(write_still=True)
else:print('Matched lookdev scene and display pipeline constructed successfully')
