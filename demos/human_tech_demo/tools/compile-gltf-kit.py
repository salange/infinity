#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Validate authored glTF 2.0 resources and compile the runtime's checked HTKIT02.

Only the documented material subset is accepted. Unknown extensions, texture
maps, skinning, animation and morph data are errors instead of grey substitutes.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

IDENTITY = [1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.]
SUPPORTED_EXTENSIONS = {'KHR_materials_ior', 'KHR_materials_specular', 'KHR_materials_transmission', 'KHR_materials_emissive_strength'}


def mul(a, b):
    return [sum(a[k*4+r]*b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]


def transform(m, p, direction=False):
    return tuple(sum(m[k*4+r]*p[k] for k in range(3)) + (0 if direction else m[12+r]) for r in range(3))


def unit(v):
    n = math.sqrt(sum(x*x for x in v))
    if n < 1e-9:
        raise ValueError('degenerate normal/tangent')
    return tuple(x/n for x in v)


def normal_matrix(m):
    a,b,c = (m[0],m[1],m[2]), (m[4],m[5],m[6]), (m[8],m[9],m[10])
    cross = lambda u,v:(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])
    bc,ca,ab=cross(b,c),cross(c,a),cross(a,b)
    det=sum(a[i]*bc[i] for i in range(3))
    if abs(det)<1e-10:
        raise ValueError('singular transform')
    return [x/det for col in (bc,ca,ab,(0,0,0)) for x in (*col,0)][0:16],det


def node_matrix(n):
    if 'matrix' in n:
        if any(k in n for k in ('translation','rotation','scale')):
            raise ValueError('matrix and TRS are mutually exclusive')
        return n['matrix']
    x,y,z,w=n.get('rotation',[0,0,0,1])
    sx,sy,sz=n.get('scale',[1,1,1]);tx,ty,tz=n.get('translation',[0,0,0])
    return [(1-2*y*y-2*z*z)*sx,(2*x*y+2*w*z)*sx,(2*x*z-2*w*y)*sx,0,
            (2*x*y-2*w*z)*sy,(1-2*x*x-2*z*z)*sy,(2*y*z+2*w*x)*sy,0,
            (2*x*z+2*w*y)*sz,(2*y*z-2*w*x)*sz,(1-2*x*x-2*y*y)*sz,0,tx,ty,tz,1]


class Gltf:
    def __init__(self,path):
        self.raw=Path(path).read_bytes()
        magic,version,size=struct.unpack_from('<III',self.raw)
        if magic!=0x46546c67 or version!=2 or size!=len(self.raw):
            raise ValueError('expected complete glTF 2.0 binary')
        off=12;chunks={}
        while off<len(self.raw):
            length,kind=struct.unpack_from('<II',self.raw,off);off+=8
            if off+length>len(self.raw) or kind in chunks: raise ValueError('invalid GLB chunk')
            chunks[kind]=self.raw[off:off+length];off+=length
        self.doc=json.loads(chunks[0x4e4f534a]);self.blob=chunks[0x004e4942]
        if self.doc.get('animations') or self.doc.get('skins'): raise ValueError('animation/skinning not supported')
        if set(self.doc.get('extensionsRequired',[]))-SUPPORTED_EXTENSIONS: raise ValueError('unsupported required glTF extension')
        if len(self.doc['buffers'])!=1 or 'uri' in self.doc['buffers'][0]: raise ValueError('only embedded buffer supported')
        if self.doc['buffers'][0]['byteLength']>len(self.blob): raise ValueError('truncated buffer')

    def accessor(self,index):
        a=self.doc['accessors'][index]
        if 'sparse' in a or a.get('normalized'): raise ValueError('sparse/normalized accessors unsupported')
        formats={5121:('B',1),5123:('H',2),5125:('I',4),5126:('f',4)}
        fmt,width=formats[a['componentType']];n={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
        v=self.doc['bufferViews'][a['bufferView']]
        if v.get('buffer',0)!=0: raise ValueError('invalid buffer reference')
        start=v.get('byteOffset',0)+a.get('byteOffset',0);stride=v.get('byteStride',width*n)
        end=start+(a['count']-1)*stride+width*n
        if stride<width*n or end>v.get('byteOffset',0)+v['byteLength'] or end>len(self.blob): raise ValueError('accessor exceeds buffer view')
        values=[struct.unpack_from('<'+fmt*n,self.blob,start+i*stride) for i in range(a['count'])]
        if not all(math.isfinite(x) for v in values for x in v): raise ValueError('non-finite accessor')
        return values


def compile_kit(source, output, manifest=None):
    gltf=Gltf(source);doc=gltf.doc
    payload=bytearray()
    def u32(value): payload.extend(struct.pack('<I',value))
    def f32(*value): payload.extend(struct.pack('<'+'f'*len(value),*value))
    def string(value):
        b=value.encode('utf-8');u32(len(b));payload.extend(b)
    material_records=[]
    for m in doc.get('materials',[]):
        p=m.get('pbrMetallicRoughness',{})
        if any(k.endswith('Texture') for k in m) or any(k.endswith('Texture') for k in p):
            raise ValueError(f"{m.get('name')}: texture map must be explicitly compiled; no fallback accepted")
        ext=m.get('extensions',{})
        if set(ext)-SUPPORTED_EXTENSIONS: raise ValueError(f"unsupported material extension: {set(ext)-SUPPORTED_EXTENSIONS}")
        ior=ext.get('KHR_materials_ior',{}).get('ior',1.5)
        if not 1.0<=ior<=2.5: raise ValueError('dielectric IOR outside supported range 1..2.5')
        spec=ext.get('KHR_materials_specular',{})
        if spec.get('specularFactor',1)!=1 or spec.get('specularColorFactor',[1,1,1])!=[1,1,1]: raise ValueError('non-default specular not mapped')
        if m.get('alphaMode','OPAQUE') not in ('OPAQUE','BLEND'): raise ValueError('alpha masking requires a compiled map')
        color=p.get('baseColorFactor',[1,1,1,1]);rough=p.get('roughnessFactor',1);metal=p.get('metallicFactor',1)
        flags=int(m.get('extras',{}).get('engine_flags',0));trans=ext.get('KHR_materials_transmission',{}).get('transmissionFactor',0)
        if trans>0 or m.get('alphaMode')=='BLEND': flags|=128
        emit=m.get('emissiveFactor',[0,0,0]);strength=ext.get('KHR_materials_emissive_strength',{}).get('emissiveStrength',1)
        peak=max(emit)*strength
        if peak>0: flags|=2
        tint=[x*strength/peak for x in emit] if peak>0 else color[:3]
        # Factors in glTF are linear. No extra gamma transform is applied.
        string(m.get('name','material'));f32(*color[:3],rough,metal,peak,0.35);u32(flags)
        optical=[ior,trans,color[3],m.get('extras',{}).get('engine_thickness_m',.012)] if flags&128 else [4.5,3.6,6.,.7]
        if flags&128 and (not 0<=trans<=1 or not 0<=color[3]<=1 or not 0<optical[3]<1): raise ValueError('invalid glass optical factors')
        f32(*tint,*optical);string(m.get('extras',{}).get('engine_albedo_set',''));f32(1.)
        material_records.append({'name':m.get('name'),'base_color_linear':color[:3],'roughness':rough,'metallic':metal,'flags':flags,'double_sided':m.get('doubleSided',False),'glass_optical':dict(zip(('ior','transmission','alpha','thickness_m'),optical)) if flags&128 else None})
    resources=[]
    for root in doc['scenes'][doc.get('scene',0)]['nodes']:
        n=doc['nodes'][root]
        if n.get('extras',{}).get('resource') is not True: raise ValueError(f"scene root {n.get('name')} is not a declared resource")
        name=n['name'];verts=[];indices=[]
        def visit(index,parent,ancestors):
            if index in ancestors: raise ValueError('node hierarchy cycle')
            node=doc['nodes'][index];matrix=mul(parent,node_matrix(node));nm,det=normal_matrix(matrix)
            if 'mesh' in node:
                for p in doc['meshes'][node['mesh']]['primitives']:
                    if p.get('mode',4)!=4 or p.get('targets'): raise ValueError('only triangle primitives without morphs supported')
                    attrs=p['attributes']
                    required={'POSITION','NORMAL','TANGENT','TEXCOORD_0'}
                    if not required<=attrs.keys(): raise ValueError(f'{name}: missing required vertex attributes {required-attrs.keys()}')
                    if set(attrs)-required: raise ValueError(f'{name}: unmapped vertex attributes {set(attrs)-required}')
                    positions=gltf.accessor(attrs['POSITION']);normals=gltf.accessor(attrs['NORMAL']);tangents=gltf.accessor(attrs['TANGENT']);uvs=gltf.accessor(attrs['TEXCOORD_0'])
                    if len({len(positions),len(normals),len(tangents),len(uvs)})!=1: raise ValueError('vertex attribute length mismatch')
                    mat=p['material']
                    if mat>=len(material_records): raise ValueError('invalid primitive material')
                    base=len(verts)
                    for i,pos in enumerate(positions):
                        pos=transform(matrix,pos);normal=unit(transform(nm,normals[i],True));t=unit(transform(matrix,tangents[i][:3],True));sign=tangents[i][3]*(-1 if det<0 else 1)
                        verts.append((*pos,*normal,*t,sign,*uvs[i],mat,*uvs[i],0.37,1.))
                    idx=[v[0] for v in gltf.accessor(p['indices'])] if 'indices' in p else list(range(len(positions)))
                    if len(idx)%3 or any(i>=len(positions) for i in idx): raise ValueError('invalid triangle indices')
                    if det<0: idx=[x for i in range(0,len(idx),3) for x in (idx[i],idx[i+2],idx[i+1])]
                    indices.extend(i+base for i in idx)
                    if doc['materials'][mat].get('doubleSided',False):
                        # Explicit reversed faces preserve the material's contract in
                        # every visibility pass, including shadow/voxel passes.
                        back=len(verts)
                        for v in verts[base:base+len(positions)]: verts.append((*v[:3],*(-x for x in v[3:6]),*v[6:9],-v[9],*v[10:]))
                        indices.extend(x+back for i in range(0,len(idx),3) for x in (idx[i],idx[i+2],idx[i+1]))
            for child in node.get('children',[]): visit(child,matrix,ancestors|{index})
        visit(root,IDENTITY,set())
        if not verts: raise ValueError(f'{name}: empty resource')
        string(name);u32(len(verts));u32(len(indices))
        for v in verts: payload.extend(struct.pack('<12fI4f',*v))
        payload.extend(struct.pack('<'+'I'*len(indices),*indices))
        bounds=[[min(v[d] for v in verts) for d in range(3)],[max(v[d] for v in verts) for d in range(3)]]
        resources.append({'name':name,'vertices':len(verts),'triangles':len(indices)//3,'bounds_m_y_up':bounds})
    source_hash=hashlib.sha256(gltf.raw).hexdigest();payload_hash=hashlib.sha256(payload).hexdigest()
    binary=b'HTKIT02\0'+struct.pack('<III',2,len(material_records),len(resources))+source_hash.encode()+payload_hash.encode()+payload
    Path(output).write_bytes(binary)
    result={'schema_version':2,'source_format':'glTF 2.0','source_sha256':source_hash,'runtime_sha256':hashlib.sha256(binary).hexdigest(),'units':'metres','up_axis':'Y','license':'Project-authored geometry; same license as repository','materials':material_records,'resources':resources}
    if manifest: Path(manifest).write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('source',type=Path);parser.add_argument('output',type=Path);parser.add_argument('--manifest',type=Path)
    args=parser.parse_args();report=compile_kit(args.source,args.output,args.manifest)
    print(f"Compiled {len(report['resources'])} named resources, {sum(r['triangles'] for r in report['resources']):,} unique triangles; sha256 {report['runtime_sha256']}")
