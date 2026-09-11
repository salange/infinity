# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Render complete native mesh resources in a separate Blender studio scene.

Run with background Blender. Geometry is read from the actual native kit or
inventory export; the studio is a diagnostic, not the native room/lighting shader.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys


def read_kit(path, selected):
    blob=path.read_bytes();position=148
    if blob[:8]!=b'HTKIT02\0':raise ValueError('Unsupported native kit')
    materials_count,resources_count=struct.unpack_from('<II',blob,12)
    def u32():
        nonlocal position
        value=struct.unpack_from('<I',blob,position)[0];position+=4;return value
    def floats(n):
        nonlocal position
        value=struct.unpack_from('<'+'f'*n,blob,position);position+=n*4;return value
    def string():
        nonlocal position
        n=u32();value=blob[position:position+n].decode();position+=n;return value
    materials=[]
    for _ in range(materials_count):
        name=string();base=floats(7);flags=u32();extra=floats(7);texture=string();uv_scale=floats(1)[0]
        materials.append(dict(name=name,color=base[:3],roughness=base[3],metallic=base[4],
            emissive=base[5],normal_strength=base[6],flags=flags,tint=extra[:3],room=extra[3:],texture=texture,uv_scale=uv_scale))
    meshes=[]
    for _ in range(resources_count):
        start=position;name=string();nv,ni=u32(),u32();vo=position;position+=nv*68;io=position;position+=ni*4
        if name==selected:
            meshes.append(dict(name=name,vertex_offset=vo,vertex_count=nv,index_offset=io,index_count=ni,
                               resource_payload_bytes=position-start))
    if not meshes:raise ValueError('Named resource is absent: '+selected)
    return dict(materials=materials,meshes=meshes,instances=[dict(mesh=0,translation=[0,0,0],yaw=0,scale=[1,1,1])]),blob


def main():
    import bpy
    import numpy as np
    from mathutils import Vector
    if not bpy.app.background:raise RuntimeError('Inventory renders require a private background Blender process')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True);p.add_argument('--resource')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--samples',type=int,default=24)
    p.add_argument('--azimuth',type=float,default=28);p.add_argument('--elevation',type=float,default=12)
    p.add_argument('--width',type=int,default=720);p.add_argument('--height',type=int,default=960)
    args=p.parse_args(sys.argv[sys.argv.index('--')+1:]);args.output.mkdir(parents=True,exist_ok=True)
    if args.source.suffix=='.htkit':data,blob=read_kit(args.source,args.resource)
    else:
        data=json.loads(args.source.read_text());blob=(args.source.parent/data['binary']).read_bytes()
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene;scene.name='Complete tower inventory studio';scene.unit_settings.system='METRIC'
    native_materials=[]
    for record in data['materials']:
        mat=bpy.data.materials.new(record['name']);mat.use_nodes=True
        shader=mat.node_tree.nodes.get('Principled BSDF');color=record['color']
        # Native material albedo is multiplied by per-instance RGB. Keep
        # shared geometry/materials while preserving that instance factor.
        object_info=mat.node_tree.nodes.new('ShaderNodeObjectInfo')
        tint=mat.node_tree.nodes.new('ShaderNodeMixRGB');tint.blend_type='MULTIPLY'
        tint.inputs[0].default_value=1;tint.inputs[1].default_value=(*color,1)
        mat.node_tree.links.new(object_info.outputs['Color'],tint.inputs[2])
        mat.node_tree.links.new(tint.outputs[0],shader.inputs['Base Color'])
        shader.inputs['Metallic'].default_value=record['metallic']
        shader.inputs['Roughness'].default_value=record['roughness']
        if record['flags']&1:
            # The native occupied model has reflected/transmitted light,
            # not a diffuse wall with the pane-transmission RGB as albedo.
            # Use real studio transmission through the exported floor/core
            # geometry. Procedural room occupancy remains native-only.
            shader.inputs['Metallic'].default_value=0
            shader.inputs['Transmission Weight'].default_value=1
            shader.inputs['IOR'].default_value=1.5
            peak=max(max(color),.001)
            f0=sum(.04*(1-record['metallic'])+.32*c/peak*record['metallic'] for c in color)/3
            shader.inputs['Specular IOR Level'].default_value=min(1.,f0/.08)
        if record['flags']&128:
            shader.inputs['Metallic'].default_value=0
            shader.inputs['Transmission Weight'].default_value=record['room'][1]
            shader.inputs['IOR'].default_value=record['room'][0]
        if record['flags']&2:
            shader.inputs['Emission Color'].default_value=(*record['tint'],1)
            shader.inputs['Emission Strength'].default_value=min(1.,record['emissive'])
        native_materials.append(mat)
    dtype=np.dtype([('p','<f4',3),('n','<f4',3),('t','<f4',4),('uv','<f4',2),('mat','<u4'),('aux','<f4',4)])
    meshes=[];used_materials=set();total_vertices=0;unique_triangles=0
    for record in data['meshes']:
        vv=np.frombuffer(blob,dtype=dtype,count=record['vertex_count'],offset=record['vertex_offset'])
        ii=np.frombuffer(blob,dtype='<u4',count=record['index_count'],offset=record['index_offset'])
        if not len(ii):meshes.append(None);continue
        points=vv['p'][:,[0,2,1]].copy();points[:,1]*=-1
        normals=vv['n'][:,[0,2,1]].copy();normals[:,1]*=-1
        mesh=bpy.data.meshes.new(record['name']);mesh.vertices.add(len(vv));mesh.vertices.foreach_set('co',points.ravel())
        mesh.loops.add(len(ii));mesh.loops.foreach_set('vertex_index',ii)
        mesh.polygons.add(len(ii)//3);mesh.polygons.foreach_set('loop_start',np.arange(0,len(ii),3,dtype=np.int32))
        mesh.polygons.foreach_set('loop_total',np.full(len(ii)//3,3,dtype=np.int32))
        for material in native_materials:mesh.materials.append(material)
        material_ids=vv['mat'][ii[::3]].astype(np.int32);mesh.polygons.foreach_set('material_index',material_ids)
        used_materials.update(int(v) for v in np.unique(material_ids))
        mesh.polygons.foreach_set('use_smooth',np.ones(len(ii)//3,dtype=np.bool_));mesh.update()
        mesh.normals_split_custom_set_from_vertices(normals)
        uv=mesh.uv_layers.new(name='Native metric coordinates')
        # Blender's chart origin differs from the native image convention.
        coords=vv['uv'][ii].copy();coords[:,1]=1-coords[:,1];uv.data.foreach_set('uv',coords.ravel())
        meshes.append(mesh);total_vertices+=len(vv);unique_triangles+=len(ii)//3
    objects=[];triangles=0;bounds=[]
    for instance in data['instances']:
        mesh=meshes[instance['mesh']]
        if mesh is None:continue
        obj=bpy.data.objects.new(mesh.name,mesh);scene.collection.objects.link(obj)
        x,y,z=instance['translation'];sx,sy,sz=instance['scale']
        obj.location=(x,-z,y);obj.rotation_euler.z=instance['yaw'];obj.scale=(sx,sz,sy)
        obj.color=(*instance.get('tint',[1,1,1]),1)
        objects.append(obj);triangles+=len(mesh.polygons)
    bpy.context.view_layer.update()
    for obj in objects:bounds.extend(obj.matrix_world@Vector(point) for point in obj.bound_box)
    lo=Vector(tuple(min(v[k] for v in bounds) for k in range(3)));hi=Vector(tuple(max(v[k] for v in bounds) for k in range(3)))
    centre=(lo+hi)*.5;extent=hi-lo;height=max(extent.z,1);width=max(extent.x,extent.y,1)
    # Translate the catalog stage only; source geometry remains unmodified.
    shift=Vector((-centre.x,-centre.y,-lo.z))
    for obj in objects:obj.location+=shift
    target=Vector((0,0,height*.5));az=math.radians(args.azimuth);el=math.radians(args.elevation)
    direction=Vector((math.sin(az)*math.cos(el),-math.cos(az)*math.cos(el),math.sin(el)))
    camera=bpy.data.objects.new('Full-asset orthographic view',bpy.data.cameras.new('Inventory camera'))
    scene.collection.objects.link(camera);camera.location=target+direction*max(height,width)*4
    camera.rotation_euler=(target-camera.location).to_track_quat('-Z','Y').to_euler();camera.data.type='ORTHO'
    # Tall native landmarks exceed Blender's default kilometre far clip.
    # Fit visibility as well as composition to the complete exported bounds.
    camera.data.clip_start=.01;camera.data.clip_end=max(height,width)*10+100
    view=camera.rotation_euler.to_matrix().transposed();framed=[view@(v+shift-target) for v in bounds]
    span_x=max(v.x for v in framed)-min(v.x for v in framed);span_y=max(v.y for v in framed)-min(v.y for v in framed)
    camera.data.ortho_scale=max(span_y,span_x*args.height/args.width)*1.22
    # Attached gardens can make the projected assembly asymmetric. Centre
    # its complete screen-space bounds rather than its world-space box.
    camera.location+=view.transposed()@Vector(((min(v.x for v in framed)+max(v.x for v in framed))*.5,
                                               (min(v.y for v in framed)+max(v.y for v in framed))*.5,0))
    visible_bounds=[view@(v+shift-camera.location) for v in bounds]
    half_y=camera.data.ortho_scale*.5;half_x=half_y*args.width/args.height
    if not all(abs(v.x)<half_x and abs(v.y)<half_y and camera.data.clip_start<-v.z<camera.data.clip_end for v in visible_bounds):
        raise ValueError('Complete asset bounds do not fit the inventory camera')
    scene.camera=camera
    ground_size=max(height,width)*4
    bpy.ops.mesh.primitive_plane_add(size=ground_size,location=(0,0,-.035))
    floor=bpy.context.object;floor.name='Diagnostic studio floor — excluded from asset counts'
    mat=bpy.data.materials.new('Neutral warm grey studio');mat.use_nodes=True
    mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value=(.19,.205,.22,1)
    mat.node_tree.nodes.get('Principled BSDF').inputs['Roughness'].default_value=.78;floor.data.materials.append(mat)
    scene.world.use_nodes=True;scene.world.node_tree.nodes['Background'].inputs['Color'].default_value=(.72,.76,.82,1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value=.4
    scale=max(height,width)
    for name,position,power,size in [('Key',(-1.3,-1.6,1.9),90,1.3),('Fill',(1.7,-.4,.8),35,1.1),('Rim',(.7,1.5,1.4),100,1.)]:
        light=bpy.data.lights.new(name,'AREA');light.shape='DISK';light.energy=scale*scale*power;light.size=scale*size
        obj=bpy.data.objects.new(name,light);scene.collection.objects.link(obj);obj.location=Vector(position)*scale
        obj.rotation_euler=(target-obj.location).to_track_quat('-Z','Y').to_euler()
    scene.render.engine='CYCLES';scene.cycles.device='CPU';scene.cycles.samples=args.samples
    scene.cycles.use_denoising=True;scene.cycles.max_bounces=6;scene.cycles.transparent_max_bounces=6
    scene.render.resolution_x=args.width;scene.render.resolution_y=args.height;scene.render.resolution_percentage=100
    scene.view_settings.view_transform='AgX';scene.render.image_settings.file_format='PNG'
    scene.render.filepath=str(args.output/'full.png');scene.render.film_transparent=False
    metadata=dict(schema_version=1,source_name=args.source.name,resource=args.resource or data.get('asset'),
        source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),source_file_bytes=args.source.stat().st_size,
        geometry_binary_sha256=hashlib.sha256(blob).hexdigest(),geometry_binary_bytes=len(blob),
        unique_triangles=unique_triangles,rendered_triangles=triangles,vertices=total_vertices,instances=len(objects),
        bounds_y_up_metres=[[float(lo.x),float(lo.z),float(-hi.y)],[float(hi.x),float(hi.z),float(-lo.y)]],
        dimensions_metres=[float(extent.x),float(extent.z),float(extent.y)],
        material_count=len(used_materials),texture_sets=sorted({data['materials'][i]['texture'] for i in used_materials if data['materials'][i]['texture']}),
        materials=[data['materials'][i] for i in sorted(used_materials)],
        resource_payload_bytes=sum(m.get('resource_payload_bytes',m['vertex_count']*68+m['index_count']*4) for m in data['meshes']),
        render_recipe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        render=dict(engine='Blender Cycles',version=bpy.app.version_string,resolution=[args.width,args.height],samples=args.samples,
                    camera='orthographic',framing_margin=1.22,camera_clip=[camera.data.clip_start,camera.data.clip_end],
                    complete_bounds_in_frustum=True,lighting='neutral three-area-light studio'),
        note='Exact native geometry, normals, UVs and instance transforms. Native pane factors are approximated by transmissive Blender studio glass; procedural room occupancy, foliage-alpha and surface-map shaders are not reproduced. Studio floor/lights excluded from counts.')
    (args.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    bpy.ops.render.render(write_still=True)
    metadata['image_sha256']=hashlib.sha256((args.output/'full.png').read_bytes()).hexdigest()
    metadata['image_bytes']=(args.output/'full.png').stat().st_size
    (args.output/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print('Inventory complete:',args.resource or data.get('asset'),triangles,'triangles',flush=True)
    sys.stdout.flush();sys.stderr.flush();os._exit(0)


if __name__=='__main__':main()
