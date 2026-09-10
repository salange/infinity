"""Metre/Y-up architectural primitives authored into a private Blender scene.

Resources retain closed slabs and core structure. Facade UVs are continuous
metric coordinates, also used by the native occupied-room material.
"""
import math
import bpy
import bmesh
from mathutils import Vector


def oval(rx, rz, n=96, center=(0, 0), yaw=0, exponent=2):
    points=[]
    for i in range(n):
        a=2*math.pi*i/n
        x=rx*math.copysign(abs(math.cos(a))**(2/exponent),math.cos(a))
        z=rz*math.copysign(abs(math.sin(a))**(2/exponent),math.sin(a))
        points.append((center[0]+x*math.cos(yaw)-z*math.sin(yaw),
                       center[1]+x*math.sin(yaw)+z*math.cos(yaw)))
    return points


def rounded_rect(hx, hz, radius, ncorner=8, center=(0,0), yaw=0):
    r=min(radius,hx,hz); points=[]
    for cx,cz,start in ((hx-r,hz-r,0),(-hx+r,hz-r,90),
                         (-hx+r,-hz+r,180),(hx-r,-hz+r,270)):
        for i in range(ncorner+1):
            a=math.radians(start+90*i/ncorner)
            x=cx+r*math.cos(a);z=cz+r*math.sin(a)
            points.append((center[0]+x*math.cos(yaw)-z*math.sin(yaw),
                           center[1]+x*math.sin(yaw)+z*math.cos(yaw)))
    return points


def inset(poly, distance):
    # Exact intersection of successive inward-offset supporting lines.
    out=[]
    for i,p in enumerate(poly):
        before=Vector(poly[i-1]);v=Vector(p);after=Vector(poly[(i+1)%len(poly)])
        a=(v-before).normalized();b=(after-v).normalized()
        na=Vector((-a.y,a.x));nb=Vector((-b.y,b.x))
        bis=na+nb
        out.append(tuple(v+bis*(distance/max(1e-8,bis.dot(na)))))
    return out


M={}


def materials():
    definitions={
        'glass_dark':((.50,.60,.66),.075,.31),
        'glass_bronze':((.78,.58,.36),.075,.60),
        'glass_blue':((.62,.76,.82),.075,.35),
        'graphite':((.043,.050,.056),.28,.42),
        'ivory':((.86,.84,.77),.28,.15),
        'ivory_edge':((.64,.62,.55),.35,.25),
        'bronze':((.49,.29,.10),.25,.70),
        'bronze_dark':((.16,.105,.052),.31,.62),
        'stone':((.63,.57,.46),.65,0),
        'soil':((.060,.039,.021),.94,0),
        'leaf':((.10,.20,.043),.85,0),
        'roof_dark':((.085,.097,.095),.72,0),
        'light':((1,.57,.20),.30,0),
    }
    for name,(color,rough,metal) in definitions.items():
        mat=bpy.data.materials.new('arrival_'+name);mat.diffuse_color=(*color,1)
        mat.use_nodes=True;mat.use_backface_culling=True
        bs=mat.node_tree.nodes.get('Principled BSDF')
        bs.inputs['Base Color'].default_value=(*color,1)
        bs.inputs['Roughness'].default_value=rough;bs.inputs['Metallic'].default_value=metal
        if name.startswith('glass_'):
            mat['engine_flags']=1
            mat['engine_room']=[3.6,4.0,6.0,.32]
            mat['engine_normal_strength']=.06
            # Pane transmission and coating belong to the factors above.
            # The occupied-room tint matches the native scene palette;
            # reusing a dark pane tint here attenuates the interior twice.
            mat['engine_tint2']=[.94,.90,.81]
        if name=='light':
            bs.inputs['Emission Color'].default_value=(*color,1)
            bs.inputs['Emission Strength'].default_value=2.0
        M[name]=mat


class Builder:
    def __init__(self, resource_name):
        self.name=resource_name;self.batches={};self.objects=[]
        self.root=bpy.data.objects.new(resource_name,None)
        bpy.context.scene.collection.objects.link(self.root)
        self.root['resource']=True
        self.root['units']='metres';self.root['authoring_up']='Y, converted to Blender Z on write'

    def mesh(self,name,vertices,faces,mat,uvs=None,smooth=False):
        key=(mat,smooth)
        batch=self.batches.setdefault(key,[[],[],[]])
        offset=len(batch[0]);batch[0].extend(vertices)
        for fi,face in enumerate(faces):
            if len(face)<3:continue
            batch[1].append(tuple(i+offset for i in face))
            if uvs is not None:batch[2].append(uvs[fi])
            else:
                points=[Vector(vertices[i]) for i in face]
                normal=(points[1]-points[0]).cross(points[2]-points[0])
                axis=max(range(3),key=lambda j:abs(normal[j]))
                axes=[j for j in range(3) if j!=axis]
                batch[2].append([(p[axes[0]],p[axes[1]]) for p in points])

    def profile(self,name,rings,mat,caps=False,smooth=True):
        n=len(rings[0][1]);verts=[(x,y,z) for y,poly in rings for x,z in poly]
        assert all(len(p)==n for y,p in rings)
        distances=[0]
        for i in range(n):
            a=rings[0][1][i];b=rings[0][1][(i+1)%n]
            distances.append(distances[-1]+math.dist(a,b))
        faces=[];uvs=[]
        for row in range(len(rings)-1):
            for i in range(n):
                j=(i+1)%n
                faces.append((row*n+j,row*n+i,(row+1)*n+i,(row+1)*n+j))
                uvs.append([(distances[i+1],rings[row][0]),(distances[i],rings[row][0]),
                            (distances[i],rings[row+1][0]),(distances[i+1],rings[row+1][0])])
        self.mesh(name,verts,faces,mat,uvs,smooth)
        if caps:
            self.mesh(name+'_caps',verts,[tuple(range(n)),tuple(reversed(range(len(verts)-n,len(verts))))],mat)

    def slab(self,name,outline,y,thickness,mat):
        self.profile(name,[(y-thickness,outline),(y,outline)],mat,True,False)

    def ring(self,name,outline,y,width,thickness,mat):
        inner=inset(outline,width);n=len(outline)
        vertices=[(x,h,z) for h,p in ((y-thickness,outline),(y,outline),
                                     (y-thickness,inner),(y,inner)) for x,z in p]
        faces=[]
        for i in range(n):
            j=(i+1)%n
            faces.extend([(j,i,n+i,n+j),(2*n+i,2*n+j,3*n+j,3*n+i),
                          (n+j,n+i,3*n+i,3*n+j),(i,j,2*n+j,2*n+i)])
        self.mesh(name,vertices,faces,mat)

    def box(self,name,center,half,mat,bevel=.04):
        x,y,z=center;hx,hy,hz=half
        b=min(bevel,hx*.3,hy*.3,hz*.3)
        if b>1e-5:
            outer=rounded_rect(hx,hz,b,2,(x,z))
            inner=rounded_rect(hx-b,hz-b,b*.35,2,(x,z))
            self.profile(name,[(y-hy,inner),(y-hy+b,outer),(y+hy-b,outer),(y+hy,inner)],mat,True,False)
        else:
            self.slab(name,[(x+hx,z+hz),(x-hx,z+hz),(x-hx,z-hz),(x+hx,z-hz)],y+hy,hy*2,mat)

    def beam(self,name,a,b,width,depth,mat):
        a=Vector(a);b=Vector(b);direction=(b-a).normalized()
        helper=Vector((0,1,0)) if abs(direction.y)<.98 else Vector((1,0,0))
        u=direction.cross(helper).normalized()*width*.5
        v=direction.cross(u).normalized()*depth*.5
        vertices=[tuple(p+u*su+v*sv) for p in (a,b) for su,sv in ((-1,-1),(1,-1),(1,1),(-1,1))]
        self.mesh(name,vertices,[(3,2,1,0),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)],mat)

    def tube(self,name,a,b,radius,mat,sides=8):
        a=Vector(a);b=Vector(b);direction=(b-a).normalized()
        helper=Vector((0,1,0)) if abs(direction.y)<.98 else Vector((1,0,0))
        u=direction.cross(helper).normalized();v=direction.cross(u).normalized()
        verts=[tuple(p+radius*(u*math.cos(2*math.pi*i/sides)+v*math.sin(2*math.pi*i/sides)))
               for p in (a,b) for i in range(sides)]
        faces=[tuple(reversed(range(sides))),tuple(range(sides,2*sides))]
        faces.extend((i,(i+1)%sides,(i+1)%sides+sides,i+sides) for i in range(sides))
        self.mesh(name,verts,faces,mat,smooth=False)

    def finish(self):
        if self.objects:return self
        for (mat,smooth),(vertices,faces,uvs) in self.batches.items():
            mesh=bpy.data.meshes.new(self.name+'_'+mat)
            mesh.from_pydata([(x,-z,y) for x,y,z in vertices],[],faces);mesh.update()
            material=M[mat]
            if mat.startswith('glass_') and 'engine_room_h' in self.root:
                material=material.copy();material.name=self.name+'_'+mat
                room=list(material['engine_room']);room[1]=float(self.root['engine_room_h'])
                material['engine_room']=room
            mesh.materials.append(material);layer=mesh.uv_layers.new(name='Metric facade coordinates')
            for poly,uv in zip(mesh.polygons,uvs):
                poly.use_smooth=smooth
                for index,value in zip(poly.loop_indices,uv):
                    # Native occupied rooms use metres with V increasing up
                    # from the resource base. Blender exports glTF V as 1-V;
                    # compensate only this procedural facade chart here.
                    # Canonicalization aligns its room tangent handedness
                    # with the exported upward metric UV derivatives.
                    layer.data[index].uv=(value[0],1-value[1]) if mat.startswith('glass_') else value
            # Explicit triangles preserve the authoring UV charts and let
            # Blender calculate Mikk tangents for closed many-sided slabs.
            bm=bmesh.new();bm.from_mesh(mesh)
            bmesh.ops.triangulate(bm,faces=list(bm.faces))
            bm.to_mesh(mesh);bm.free();mesh.update()
            obj=bpy.data.objects.new(mesh.name,mesh);bpy.context.scene.collection.objects.link(obj)
            obj.parent=self.root;self.objects.append(obj)
        self.batches.clear();return self
