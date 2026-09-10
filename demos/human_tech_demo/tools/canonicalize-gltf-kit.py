#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Canonical glTF interchange: stable ordering and 10-micrometre precision.

Blender may reorder equivalent mesh vertices between authoring runs. Keep the
editable blend and raw export, then serialize an equivalent resource-level glTF
with deterministic vertex, triangle, material and resource ordering.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('kit_compiler',Path(__file__).with_name('compile-gltf-kit.py'))
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)

def canonicalize(source,output):
    g=c.Gltf(source);d=g.doc
    materials=sorted(enumerate(d['materials']),key=lambda x:x[1].get('name',''))
    remap={old:new for new,(old,_) in enumerate(materials)}
    buffer=bytearray();views=[];accessors=[];meshes=[];nodes=[];repaired_tangents=0
    def accessor(values,kind,position=False):
        offset=len(buffer);width=len(values[0]);code='I' if kind==5125 else 'f'
        for value in values:buffer.extend(struct.pack('<'+code*width,*value))
        view=len(views);views.append({'buffer':0,'byteOffset':offset,'byteLength':len(buffer)-offset})
        a={'bufferView':view,'componentType':kind,'count':len(values),'type':{1:'SCALAR',2:'VEC2',3:'VEC3',4:'VEC4'}[width]}
        if position:
            a['min']=[min(v[k] for v in values) for k in range(width)];a['max']=[max(v[k] for v in values) for k in range(width)]
        accessors.append(a);return len(accessors)-1
    roots=d['scenes'][d.get('scene',0)]['nodes']
    for root in sorted(roots,key=lambda i:d['nodes'][i]['name']):
        name=d['nodes'][root]['name'];groups={}
        if d['nodes'][root].get('extras',{}).get('resource') is not True: raise ValueError('all roots must declare resource=true')
        def visit(i,parent,ancestors):
            nonlocal repaired_tangents
            if i in ancestors:raise ValueError('node hierarchy cycle')
            node=d['nodes'][i];m=c.mul(parent,c.node_matrix(node));nm,det=c.normal_matrix(m)
            if 'mesh' in node:
                for p in d['meshes'][node['mesh']]['primitives']:
                    if p.get('mode',4)!=4 or p.get('targets'):raise ValueError('only static triangle primitives supported')
                    a=p['attributes']
                    if set(a)!={'POSITION','NORMAL','TANGENT','TEXCOORD_0'}:raise ValueError('unmapped/missing vertex attribute')
                    positions=g.accessor(a['POSITION']);normals=g.accessor(a['NORMAL']);tangents=g.accessor(a['TANGENT']);uvs=g.accessor(a['TEXCOORD_0'])
                    if len({len(positions),len(normals),len(tangents),len(uvs)})!=1:raise ValueError('attribute length mismatch')
                    ii=[v[0] for v in g.accessor(p['indices'])] if 'indices' in p else list(range(len(positions)))
                    if len(ii)%3 or any(v>=len(positions) for v in ii):raise ValueError('invalid primitive indices')
                    # MikkTSpace can emit a zero tangent at a decimated chart
                    # corner. Reconstruct that frame from its actual metric UV
                    # derivative; invalid UVs remain a hard error.
                    for j,tangent in enumerate(tangents):
                        if sum(x*x for x in tangent[:3])>1e-10:continue
                        recovered=None
                        for at in range(0,len(ii),3):
                            tri=ii[at:at+3]
                            if j not in tri:continue
                            a,b,e=tri;dp=[positions[b][k]-positions[a][k] for k in range(3)];dq=[positions[e][k]-positions[a][k] for k in range(3)]
                            du,dv=uvs[b][0]-uvs[a][0],uvs[b][1]-uvs[a][1]
                            eu,ev=uvs[e][0]-uvs[a][0],uvs[e][1]-uvs[a][1];detuv=du*ev-dv*eu
                            if abs(detuv)<1e-12:continue
                            t=[(dp[k]*ev-dq[k]*dv)/detuv for k in range(3)];normal=c.unit(normals[j]);dot=sum(t[k]*normal[k] for k in range(3))
                            t=[t[k]-normal[k]*dot for k in range(3)]
                            if sum(x*x for x in t)<1e-12:continue
                            recovered=(*c.unit(t),tangent[3]);break
                        if recovered is None:raise ValueError('cannot reconstruct tangent from geometry/UV')
                        tangents[j]=recovered;repaired_tangents+=1
                    vv=[]
                    for j,pos in enumerate(positions):
                        n=c.unit(c.transform(nm,normals[j],True));t=c.unit(c.transform(m,tangents[j][:3],True))
                        vv.append(tuple(round(v,5)+0.0 for v in (*c.transform(m,pos),*n,*t,tangents[j][3]*(-1 if det<0 else 1),*uvs[j])))
                    ii=[v[0] for v in g.accessor(p['indices'])] if 'indices' in p else list(range(len(vv)))
                    if len(ii)%3 or any(v>=len(vv) for v in ii):raise ValueError('invalid primitive indices')
                    group=groups.setdefault(remap[p['material']],[])
                    for j in range(0,len(ii),3):
                        triangle=[vv[ii[j+k]] for k in ((0,2,1) if det<0 else (0,1,2))]
                        # Removing only coincident vertices eliminates zero-area
                        # pole triangles created by export quantization.
                        if len(set(triangle))==3:group.append(triangle)
            for child in node.get('children',[]):visit(child,m,ancestors|{i})
        visit(root,c.IDENTITY,set())
        primitives=[]
        for material,triangles in sorted(groups.items()):
            verts=sorted({v for tri in triangles for v in tri});lookup={v:i for i,v in enumerate(verts)}
            indices=[]
            for tri in triangles:
                idx=tuple(lookup[v] for v in tri);indices.append(min(idx,idx[1:]+idx[:1],idx[2:]+idx[:2]))
            indices=sorted(indices)
            primitives.append({'attributes':{'POSITION':accessor([v[:3] for v in verts],5126,True),
                                               'NORMAL':accessor([v[3:6] for v in verts],5126),
                                               'TANGENT':accessor([v[6:10] for v in verts],5126),
                                               'TEXCOORD_0':accessor([v[10:12] for v in verts],5126)},
                               'indices':accessor([(i,) for tri in indices for i in tri],5125),'material':material,'mode':4})
        nodes.append({'name':name,'mesh':len(meshes),'extras':{'resource':True}});meshes.append({'name':name,'primitives':primitives})
    doc={'asset':{'version':'2.0','generator':'Human tech resource canonicalizer 2'},'scene':0,
         'scenes':[{'name':'Human Tech Resource Library','nodes':list(range(len(nodes)))}],
         'nodes':nodes,'meshes':meshes,'materials':[m for _,m in materials],
         'buffers':[{'byteLength':len(buffer)}],'bufferViews':views,'accessors':accessors}
    for key in ('extensionsUsed','extensionsRequired'):
        if key in d:doc[key]=sorted(d[key])
    blob=json.dumps(doc,sort_keys=True,separators=(',',':')).encode();blob+=b' '*(-len(blob)%4)
    buffer+=b'\0'*(-len(buffer)%4)
    result=struct.pack('<III',0x46546c67,2,12+8+len(blob)+8+len(buffer))+struct.pack('<II',len(blob),0x4e4f534a)+blob+struct.pack('<II',len(buffer),0x004e4942)+buffer
    Path(output).write_bytes(result)
    print('Canonical glTF:',len(nodes),'resources;',len(result),'bytes;',repaired_tangents,'UV tangent frames reconstructed')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args();canonicalize(a.source,a.output)
