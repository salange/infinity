"""Rebuild the editable human-tech resource library in a private Blender scene.

Invoke with Blender's bundled Python through tools/build-assets.py. Positions
below use metres and Y up; B() converts them into Blender's Z-up authoring frame.
"""
import bpy
import bmesh
import math
from mathutils import Vector
from pathlib import Path
import random
import sys

args=sys.argv[sys.argv.index('--')+1:]
out=Path(args[0]);out.mkdir(parents=True,exist_ok=True)
scene=bpy.data.scenes.new('Human Tech Resource Library')
bpy.context.window.scene=scene
scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1
M={}

def B(p): return (p[0],-p[2],p[1])
def material(name,color,rough,metal=0,glass=False,emission=0,two=False):
    m=bpy.data.materials.new(name);m.use_nodes=True
    p=m.node_tree.nodes.get('Principled BSDF');p.inputs['Base Color'].default_value=(*color,1)
    p.inputs['Roughness'].default_value=rough;p.inputs['Metallic'].default_value=metal
    if glass:
        p.inputs['Transmission Weight'].default_value=0.86;p.inputs['IOR'].default_value=1.5
        p.inputs['Alpha'].default_value=0.18;m.surface_render_method='DITHERED'
    if emission:
        p.inputs['Emission Color'].default_value=(*color,1);p.inputs['Emission Strength'].default_value=emission
    m.use_backface_culling=not two
    m['engine_flags']=256 if two else 0
    M[name]=m;return m
material('ceramic_ivory',(0.83,0.80,0.70),.31)
material('ceramic_cool',(0.63,0.68,0.66),.4)
material('ceramic_edge',(0.43,0.48,0.46),.49)
material('bronze_satin',(.42,.24,.085),.29,.86)
material('bronze_oxidized',(.12,.23,.19),.63,.45)
material('gasket_charcoal',(.024,.031,.033),.83)
material('glass_clear',(.66,.82,.81),.075,glass=True)
material('stone_warm',(.44,.42,.35),.71)
material('wood_oiled',(.19,.086,.032),.42)
material('soil_mulch',(.043,.026,.014),.97)
material('bark_ridged',(.12,.069,.033),.92)
material('leaf_deep',(.019,.090,.028),.56,two=True)
material('leaf_middle',(.045,.17,.045),.51,two=True)
material('leaf_sunlit',(.13,.28,.069),.47,two=True)
material('leaf_silver',(.19,.28,.17),.58,two=True)
material('flower_coral',(.48,.097,.067),.53,two=True)
material('flower_cream',(.72,.59,.30),.62,two=True)
material('lamp_warm',(1.,.61,.23),.3,emission=2.3)
material('fabric_sand',(.40,.35,.24),.92)
material('water_dark',(.035,.09,.075),.08)
current=None

def root(name):
    global current
    current=bpy.data.objects.new(name,None);scene.collection.objects.link(current)
    current['resource']=True
    return current

def finish(obj,mat):
    obj.parent=current
    if mat: obj.data.materials.append(M[mat])
    return obj

def smooth(obj):
    for p in obj.data.polygons:p.use_smooth=True
    return obj

def uv(obj):
    layer=obj.data.uv_layers.active or obj.data.uv_layers.new(name='UVMap')
    for poly in obj.data.polygons:
        n=poly.normal;axis=max(range(3),key=lambda i:abs(n[i]));axes=[i for i in range(3) if i!=axis]
        for li in poly.loop_indices:
            co=obj.data.vertices[obj.data.loops[li].vertex_index].co
            layer.data[li].uv=(co[axes[0]],co[axes[1]])
    return obj

def box(name,pos,size,mat,bevel=0.04):
    bpy.ops.mesh.primitive_cube_add(size=1,location=B(pos));obj=bpy.context.object;obj.name=name
    obj.dimensions=(size[0],size[2],size[1]);bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    finish(obj,mat)
    if bevel:
        mod=obj.modifiers.new('Manufactured edge radius','BEVEL');mod.width=bevel;mod.segments=3
        bpy.context.view_layer.objects.active=obj;bpy.ops.object.modifier_apply(modifier=mod.name)
        mod=obj.modifiers.new('Weighted corner normals','WEIGHTED_NORMAL');mod.keep_sharp=True
        bpy.ops.object.modifier_apply(modifier=mod.name)
    return uv(obj)

def tube(name,a,b,r,mat,sides=12,r1=None):
    delta=Vector(B(b))-Vector(B(a));mid=(Vector(B(b))+Vector(B(a)))*.5
    bpy.ops.mesh.primitive_cone_add(vertices=sides,radius1=r,radius2=r if r1 is None else r1,depth=delta.length,location=mid)
    obj=bpy.context.object;obj.name=name;obj.rotation_mode='QUATERNION';obj.rotation_quaternion=delta.to_track_quat('Z','Y')
    finish(obj,mat);smooth(obj);return uv(obj)

def sphere(name,pos,scale,mat,segments=16,rings=10):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments,ring_count=rings,radius=1,location=B(pos));obj=bpy.context.object
    obj.name=name;obj.scale=(scale[0],scale[2],scale[1]);finish(obj,mat);smooth(obj);return uv(obj)

def torus(name,pos,R,r,mat,axis=(0,1,0),major=36,minor=8):
    bpy.ops.mesh.primitive_torus_add(major_segments=major,minor_segments=minor,location=B(pos),major_radius=R,minor_radius=r)
    obj=bpy.context.object;obj.name=name;obj.rotation_mode='QUATERNION';obj.rotation_quaternion=Vector(B(axis)).to_track_quat('Z','Y')
    finish(obj,mat);smooth(obj);return uv(obj)

def path(name,points,r,mat,sides=10):
    # Smooth swept circular section, one connected indexed surface.
    v=[];f=[]
    for i,p in enumerate(points):
        tangent=Vector(points[min(i+1,len(points)-1)])-Vector(points[max(0,i-1)])
        tangent.normalize();ref=Vector((0,1,0)) if abs(tangent.y)<.9 else Vector((1,0,0))
        a=tangent.cross(ref).normalized();b=tangent.cross(a).normalized()
        for j in range(sides):
            radius=r[i] if isinstance(r,(tuple,list)) else r
            q=Vector(p)+(a*math.cos(j*2*math.pi/sides)+b*math.sin(j*2*math.pi/sides))*radius
            v.append(B(q))
        if i:
            for j in range(sides):f.append(((i-1)*sides+j,(i-1)*sides+(j+1)%sides,i*sides+(j+1)%sides,i*sides+j))
    f.extend([tuple(reversed(range(sides))),tuple(range((len(points)-1)*sides,len(points)*sides))])
    mesh=bpy.data.meshes.new(name);mesh.from_pydata(v,[],f);mesh.update()
    obj=bpy.data.objects.new(name,mesh);scene.collection.objects.link(obj);finish(obj,mat);smooth(obj);return uv(obj)

class BranchMesh:
    def __init__(self,name):self.name=name;self.vertices=[];self.faces=[]
    def stem(self,a,b,r0,r1,sides=5):
        a=Vector(a);b=Vector(b);axis=(b-a).normalized()
        side=axis.cross(Vector((0,1,0)) if abs(axis.y)<.95 else Vector((1,0,0))).normalized()
        up=axis.cross(side).normalized();first=len(self.vertices)
        for point,r in ((a,r0),(b,r1)):
            for i in range(sides):self.vertices.append(B(point+(side*math.cos(i*2*math.pi/sides)+up*math.sin(i*2*math.pi/sides))*r))
        for i in range(sides):self.faces.append((first+i,first+(i+1)%sides,first+sides+(i+1)%sides,first+sides+i))
    def object(self):
        mesh=bpy.data.meshes.new(self.name);mesh.from_pydata(self.vertices,[],self.faces);mesh.update()
        obj=bpy.data.objects.new(self.name,mesh);scene.collection.objects.link(obj);finish(obj,'bark_ridged');smooth(obj);return uv(obj)

class Leaves:
    def __init__(self,name,segments=5,rounded=False):self.name=name;self.vertices=[];self.faces=[];self.mats=[];self.segments=segments;self.rounded=rounded
    def leaf(self,base,tip,width,mat=1,curl=.08,roll=0.):
        base=Vector(base);tip=Vector(tip);d=tip-base
        side=d.cross(Vector((0,1,0)))
        if side.length<.001:side=Vector((1,0,0))
        side.normalize();side=side*math.cos(roll)+d.normalized().cross(side)*math.sin(roll)
        start=len(self.vertices);N=self.segments
        for j in range(N+1):
            t=j/N;arc=max(0.,math.sin(math.pi*t));center=base+d*t+Vector((0,arc*curl,0));w=(arc**.85 if self.rounded else arc)*width
            for s in (-1,0,1):
                asymmetry=(1.+s*.08*math.sin(math.pi*t+.4)) if self.rounded else 1.
                self.vertices.append(B(center+side*(w*s*asymmetry)+Vector((0,-abs(s)*width*.18*arc,0))))
        for j in range(N):
            for k in range(2):self.faces.append((start+j*3+k,start+(j+1)*3+k,start+(j+1)*3+k+1,start+j*3+k+1));self.mats.append(mat)
    def object(self):
        mesh=bpy.data.meshes.new(self.name);mesh.from_pydata(self.vertices,[],self.faces);mesh.update()
        obj=bpy.data.objects.new(self.name,mesh);scene.collection.objects.link(obj);finish(obj,None)
        for m in ('leaf_deep','leaf_middle','leaf_sunlit','leaf_silver','flower_coral','flower_cream'):mesh.materials.append(M[m])
        for p,mat in zip(mesh.polygons,self.mats):p.material_index=mat;p.use_smooth=True
        uv(obj);return obj

# Hero: three ceramic shells wrap a bronze mechanical junction. Segment seams,
# collars, inset fasteners and a separate inner gasket survive grazing light.
root('ceramic_lattice_node')
sphere('Forged heart',(0,0,0),(1.18,1.18,.94),'bronze_satin',24,16)
for arm in range(3):
    angle=arm*2*math.pi/3+math.pi/2;d=Vector((math.cos(angle),math.sin(angle),0))
    for i in range(4):
        a=d*(.70+i*1.42);b=d*(.70+(i+1)*1.42-.065)
        tube('Curved ceramic shell',a,b,.64-i*.016,'ceramic_ivory',24)
        if i<3:torus('Recessed expansion seal',d*(.70+(i+1)*1.42-.033),.612-i*.016,.032,'gasket_charcoal',d,24,8)
    for dist in (.95,1.16):torus('Bronze collar',d*dist,.655,.065,'bronze_satin',d)
    a=d*.98
    for j in range(6):
        # Screw heads distributed around arm; no texture substitutes.
        perp=Vector((-d.y,d.x,0));q=a+perp*(math.cos(j*math.pi/3)*.69)+Vector((0,0,math.sin(j*math.pi/3)*.69))
        sphere('Socket screw',q,(.055,.055,.035),'bronze_oxidized',8,6)
# Fine ornament around front-facing inspection cover.
torus('Inspection cover trim',(0,0,.92),.62,.06,'bronze_satin',(0,0,1))
for j in range(12):
    a=j*math.pi/6;tube('Radial engraving',(.3*math.cos(a),.3*math.sin(a),.98),(.53*math.cos(a),.53*math.sin(a),.98),.013,'ceramic_edge',6)

root('planter_bench')
box('Basalt base',(0,.17,0),(5.6,.34,2.6),'stone_warm',.16)
for z in (-1.15,1.15):box('Planter ceramic lip',(0,.53,z),(5.3,.60,.24),'ceramic_ivory',.10)
for x in (-2.55,2.55):box('Rounded end',(x,.53,0),(.24,.60,2.2),'ceramic_ivory',.10)
box('Visible soil',(0,.69,0),(4.88,.16,1.94),'soil_mulch',.03)
for j in range(13):box('Oiled timber seating',(0,.87,1.61+j*.058),(5.25,.075,.044),'wood_oiled',.014)
for x in (-2.2,0,2.2):tube('Bench support',(x,.10,1.82),(x,.77,1.82),.045,'bronze_satin',10)
rng=random.Random(991)
for i in range(65):
    x=rng.uniform(-2.35,2.35);z=rng.uniform(-.86,.86)
    tube('Mulch chip',(x,.79,z),(x+rng.uniform(.03,.13),.80,z+.035),rng.uniform(.01,.025),'wood_oiled',5)

root('glass_rail')
box('Laminated glass',(0,.64,0),(3.9,1.15,.027),'glass_clear',.012)
for x in (-1.94,0,1.94):
    box('Rail shoe',(x,.06,0),(.13,.12,.12),'bronze_satin',.025)
    for y in (.16,1.1):box('Glass clamp',(x,y,0),(.12,.11,.075),'bronze_satin',.013)
box('Continuous cap',(0,1.235,0),(4.,.055,.066),'bronze_satin',.024)

root('bronze_light')
box('Recess frame',(0,.09,0),(.74,.18,.21),'bronze_satin',.065)
box('Frosted diffuser',(0,.16,.015),(.60,.035,.115),'lamp_warm',.016)
for x in (-.32,.32):sphere('Mounting screw',(x,.185,0),(.018,.009,.018),'gasket_charcoal',8,6)

root('interior_lounge')
box('Lounge floor',(0,.08,0),(8.,.16,7.),'wood_oiled',.025)
box('Rear wall',(0,1.65,-3.45),(8.,3.3,.15),'ceramic_cool',.03)
for x in (-2.8,2.8):
    box('Sofa base',(x,.26,-1),(1.5,.36,2.7),'wood_oiled',.10)
    box('Sofa cushion',(x,.55,-1),(1.44,.26,2.58),'fabric_sand',.12)
    box('Sofa back',(x+.48 if x<0 else x-.48,.94,-1),(.4,.66,2.58),'fabric_sand',.13)
for z in (-1.8,.45):
    box('Low table',(0,.58,z),(1.62,.10,1.0),'stone_warm',.12)
    for x in (-.56,.56):tube('Table leg',(x,.10,z),(x,.53,z),.034,'bronze_satin',10)
for y in (.62,1.22,1.82,2.42):box('Display shelf',(0,y,-3.2),(4.4,.065,.38),'wood_oiled',.02)
for i in range(18):box('Book spine',(-1.97+i*.22,1.41,-3.20),(.12,.31+(i%3)*.04,.21),'ceramic_edge' if i%3 else 'bronze_oxidized',.009)
for x in (-3,0,3):box('Ceiling bounce light',(x,3.12,-2.8),(1.8,.05,.13),'lamp_warm',.015)

# Independent terminal modules (part origins are floor centres unless noted).
for variant in range(4):
    root(('facade_ceramic_panel','facade_bronze_louver','facade_service_panel','facade_window_bay')[variant])
    box('Panel backing',(0,1.8,0),(3.6,3.6,.22),'ceramic_ivory',.13)
    if variant==0:
        for y in (.42,1.8,3.18):box('Expansion joint',(0,y,.118),(3.43,.025,.012),'gasket_charcoal',.005)
    elif variant==1:
        for i in range(13):box('Solar fin',(-1.6+i*.26,1.8,.40),(.065,3.42,.69),'bronze_satin',.021)
    elif variant==2:
        box('Service recess',(0,1.6,.15),(1.8,2.3,.06),'ceramic_edge',.08)
        for y in (1.,1.3,1.6,1.9,2.2):box('Vent blade',(0,y,.20),(1.6,.07,.10),'bronze_satin',.02)
    else:
        box('Window glass',(0,1.8,.19),(3.15,3.14,.028),'glass_clear',.01)
        for x in (-1.64,1.64):box('Bronze mullion',(x,1.8,.24),(.052,3.2,.06),'bronze_satin',.014)
for v in range(4):
    root(('bridge_ceramic_rib','bridge_glass_canopy','bridge_deck_joint','bridge_cable_saddle')[v])
    if v==0:
        points=[(math.cos(math.pi*i/32)*3.4,math.sin(math.pi*i/32)*2.9,0) for i in range(33)]
        path('Swept rib',points,.21,'ceramic_ivory',16)
        for s in (-1,1):box('Rib shoe',(s*3.4,.08,0),(.6,.16,.7),'bronze_satin',.08)
    elif v==1:
        box('Overhead glass',(0,2.85,0),(6.4,.035,2.9),'glass_clear',.01)
        for z in (-1.45,1.45):tube('Canopy transom',(-3.2,2.84,z),(3.2,2.84,z),.045,'bronze_satin',10)
    elif v==2:
        box('Deck nosing',(0,.09,0),(6.4,.18,.46),'bronze_satin',.04)
        for x in range(-15,16):box('Drain slot',(x*.2,.184,0),(.055,.009,.33),'gasket_charcoal',.012)
    else:
        tube('Curved saddle',(-1,0,0),(1,1.6,0),.22,'ceramic_ivory',16)
        for y in (.65,.95):torus('Tension collar',(0,y,0),.26,.04,'bronze_satin',(1,1,0))
for v in range(6):
    root(('street_bollard','street_drain','street_access_cover','street_sign','street_table','street_seat')[v])
    if v==0:
        tube('Bollard body',(0,0,0),(0,.91,0),.11,'ceramic_cool',16);torus('Reflector',(0,.74,0),.111,.021,'lamp_warm')
    elif v==1:
        box('Drain frame',(0,.026,0),(1.4,.052,.42),'bronze_oxidized',.022)
        for x in range(18):box('Drain slit',(-.62+x*.074,.055,0),(.025,.008,.32),'gasket_charcoal',.005)
    elif v==2:
        tube('Utility cover',(0,0,0),(0,.04,0),.65,'bronze_oxidized',32)
        for r in (.31,.55):torus('Raised grip',(0,.045,0),r,.011,'bronze_satin')
    elif v==3:
        tube('Wayfinding stem',(0,0,0),(0,2.35,0),.044,'bronze_satin')
        box('Sign face',(0,2.0,0),(1.2,.66,.09),'ceramic_cool',.11)
        for y in (1.86,2.03,2.18):box('Inset route stripe',(-.1,y,.05),(.69,.018,.006),'bronze_satin',.005)
    elif v==4:
        tube('Cafe pedestal',(0,0,0),(0,.74,0),.065,'bronze_satin');tube('Tabletop',(0,.74,0),(0,.80,0),.65,'stone_warm',40)
    else:
        box('Seat cushion',(0,.43,0),(.60,.13,.58),'wood_oiled',.08)
        box('Seat back',(0,.75,-.24),(.60,.61,.09),'wood_oiled',.07)
        for x in (-.23,.23):
            for z in (-.21,.21):tube('Seat leg',(x,0,z),(x,.39,z),.022,'bronze_satin',8)
for v in range(4):
    root(('roof_vent','roof_solar_panel','roof_irrigation','roof_service_cabinet')[v])
    if v==0:
        box('Vent housing',(0,.46,0),(1.6,.92,1.2),'ceramic_cool',.16)
        for i in range(9):box('Vent blades',(-.65+i*.16,.96,0),(.09,.06,1.),'bronze_oxidized',.021)
    elif v==1:
        box('Solar frame',(0,.42,0),(2.8,.07,1.6),'bronze_satin',.025)
        for x in range(8):
            for z in range(4):box('Photovoltaic cell',(-1.2+x*.34,.465,-.59+z*.39),(.32,.016,.37),'glass_clear',.007)
    elif v==2:
        path('Manifold',[(0,0,0),(0,.32,0),(.5,.4,0),(1.3,.4,0)],.038,'bronze_oxidized')
        torus('Valve handle',(.55,.63,0),.13,.018,'bronze_satin')
    else:
        box('Equipment enclosure',(0,.90,0),(1.8,1.8,.8),'ceramic_cool',.12)
        for x in (-.42,.42):box('Service door',(x,.95,.413),(.77,1.51,.024),'ceramic_edge',.04)

# Additional construction and maintenance terminals.
for v in range(10):
    root(('stair_flight','curb_segment','planter_channel','glass_door','facade_jamb',
          'window_lintel','service_pipe','market_counter','pendant_lamp','ceramic_arch')[v])
    if v==0:
        for j in range(8):box('Stone stair',(0,(j+1)*.09,-j*.29),(3.2,(j+1)*.18,.30),'stone_warm',.024)
        for x in (-1.45,1.45):tube('Stair rail',(x,.9,.2),(x,2.15,-2.1),.035,'bronze_satin',10)
    elif v==1:
        box('Rounded curb',(0,.16,0),(4.,.32,.37),'stone_warm',.065)
        for x in (-1.,1.):box('Joint gasket',(x,.163,.19),(.015,.25,.015),'gasket_charcoal',.004)
    elif v==2:
        for x in (-.6,.6):box('Bed side',(x,.40,0),(.13,.8,3.6),'ceramic_cool',.055)
        box('Earth',(0,.67,0),(1.05,.10,3.4),'soil_mulch',.015)
        tube('Drip irrigation',(-.35,.745,-1.6),(-.35,.745,1.6),.009,'gasket_charcoal',6)
    elif v==3:
        box('Glass entry leaf',(0,1.5,0),(1.25,3.,.036),'glass_clear',.009)
        tube('Door pull',(.43,1.02,.09),(.43,1.87,.09),.019,'bronze_satin',10)
        for y in (.32,2.65):box('Concealed hinge',(-.61,y,0),(.052,.16,.065),'bronze_satin',.012)
    elif v==4:
        box('Deep reveal',(0,1.8,0),(.33,3.6,.72),'ceramic_ivory',.08)
        box('Bronze reveal strip',(.18,1.8,.21),(.035,3.52,.07),'bronze_satin',.015)
    elif v==5:
        box('Window hood',(0,.11,0),(3.5,.22,1.14),'ceramic_ivory',.07)
        box('Underside light',(0,.006,.21),(3.,.018,.044),'lamp_warm',.006)
    elif v==6:
        path('Elbow service pipe',[(0,0,0),(0,1.2,0),(.08,1.35,0),(.3,1.42,0),(1.4,1.42,0)],.074,'bronze_oxidized',14)
        for y in (.3,1.):torus('Pipe clamp',(0,y,0),.080,.023,'bronze_satin')
    elif v==7:
        box('Cafe counter',(0,.52,0),(3.7,1.04,.94),'wood_oiled',.08)
        box('Stone serving top',(0,1.1,0),(3.85,.12,1.05),'stone_warm',.055)
        for x in (-1.25,-.75,-.25,.25,.75,1.25):tube('Cup',(x,1.17,0),(x,1.31,0),.055,'ceramic_ivory',14)
    elif v==8:
        tube('Pendant cable',(0,0,0),(0,-1.2,0),.009,'bronze_satin',8)
        sphere('Opal pendant',(0,-1.30,0),(.23,.27,.23),'lamp_warm',24,14)
        torus('Pendant rim',(0,-1.23,0),.225,.028,'bronze_satin')
    else:
        path('Continuous entry arch',[(3.2*math.cos(i*math.pi/36),3.2*math.sin(i*math.pi/36),0) for i in range(37)],.33,'ceramic_ivory',20)
        for x in (-3.2,3.2):box('Arch foot',(x,.15,0),(.86,.3,.8),'bronze_satin',.12)

# Trees use branching architecture and curved individual leaves, never spheres
# as visible foliage. Each species uses its own fixed stream and shape grammar.
def broad_tree(name,height,spread,seed,columnar=False,multi=False):
    root(name);rng=random.Random(seed);leaves=Leaves(name+'_leaves',segments=4,rounded=True)
    support=BranchMesh(name+'_terminal_branch_network')
    crown=Vector((spread*.10,height*(.68 if columnar else .74),-spread*.07))
    rx=spread*(1.12 if multi else 1.0)
    ry=height*(.34 if columnar else .265)
    rz=spread*(1.16 if multi else .90)
    # Fork heights and ascending limbs are independent of foliage distribution.
    # A filled asymmetric crown envelope replaces the former stacked whorls.
    trunk_tips=[];limbs=[]
    for trunk in range(3 if multi else 1):
        ox=(trunk-1)*.33 if multi else 0.;oz=rng.uniform(-.2,.2)
        lean=Vector((rng.uniform(-.30,.55),0,rng.uniform(-.35,.30)))
        trunk_height=height*rng.uniform(.59,.75)
        points=[]
        for k in range(12):
            t=k/11
            points.append(Vector((ox,0,oz))+Vector((.11*math.sin(t*4.1),trunk_height*t,.09*math.sin(t*3.2+trunk)))+lean*t)
        radii=[height*.027*(1-.76*k/11) for k in range(12)]
        path('Curved tapering trunk',points,radii,'bark_ridged',12)
        trunk_tips.append(points[-1])
        for branch in range(7 if not multi else 4):
            angle=branch*2.399963+trunk*.93+rng.uniform(-.42,.42)
            start=points[rng.randrange(5,10)]
            end=crown+Vector((math.cos(angle)*rx*rng.uniform(.42,.73),rng.uniform(-.42,.60)*ry,math.sin(angle)*rz*rng.uniform(.42,.73)))
            mid=start*.5+end*.5+Vector((0,.25,0))
            points_b=[start,start*.3+mid*.7,mid*.35+end*.65,end]
            thickness=height*rng.uniform(.007,.014)
            path('Ascending primary limb',points_b,[thickness,thickness*.76,thickness*.42,.016],'bark_ridged',9)
            limbs.append(end)
    cluster_count=154 if not columnar and not multi else (140 if columnar else 140)
    for cluster in range(cluster_count):
        # Uniform volume sampling with a mildly lobed, leaning envelope.
        # There is no height quantization or horizontal foliage shelf.
        direction=Vector((rng.gauss(0,1),rng.gauss(0,1),rng.gauss(0,1))).normalized()
        distance=rng.random()**(1/3)
        theta=math.atan2(direction.z,direction.x)
        lobe=1+.13*math.sin(3*theta+.7)+.07*math.cos(5*theta-1.2)
        p=crown+Vector((direction.x*rx*lobe,direction.y*ry,direction.z*rz*lobe))*distance
        p.x+=.13*rx*direction.y*direction.y
        p.y+=.10*ry*math.sin(theta*2.0+.4)*distance
        anchor=min(limbs,key=lambda q:(q-p).length_squared)
        halfway=anchor*.4+p*.6+Vector((0,rng.uniform(.02,.15),0))
        support.stem(anchor,halfway,.017,.009,5);support.stem(halfway,p,.009,.0038,5)
        # Smaller curved blades have independent asymmetric margins. More
        # overlapping sprays preserve the filled crown at close viewing distance.
        for shoot in range(12):
            angle=rng.uniform(0,2*math.pi)
            axis=Vector((math.cos(angle),rng.uniform(-.48,.62),math.sin(angle))).normalized()
            reach=rng.uniform(.42,.78)*(1.1 if multi else 1.)
            start=p+Vector((rng.uniform(-.22,.22),rng.uniform(-.24,.24),rng.uniform(-.22,.22)))
            tip=start+axis*reach
            support.stem(start,tip,.0034,.0012,3)
            side=axis.cross(Vector((0,1,0))).normalized()
            for leaf in range(10):
                t=.12+leaf*(.78/9)
                base=start+axis*(reach*t)
                sign=-1 if leaf%2 else 1
                length=rng.uniform(.14,.24)
                out=(axis*.28+side*(sign*.92)+Vector((0,rng.uniform(-.28,.26),0))).normalized()
                end=base+out*length
                mat=rng.choices((0,1,2),(3,7,1.5 if p.y>crown.y else .7))[0]
                leaves.leaf(base,end,length*rng.uniform(.28,.37),mat,length*rng.uniform(.10,.18),rng.uniform(-.85,.85))
    leaves.object();support.object()
broad_tree('canopy_broadleaf',11,3.3,104)
broad_tree('canopy_columnar',14,1.65,213,True)
broad_tree('tree_multistem',6,1.9,322,multi=True)

def palm(name,height,fan,seed):
    root(name);rng=random.Random(seed);leaves=Leaves(name+'_fronds')
    points=[(.36*math.sin(i*.19),height*i/20,.08*i/20) for i in range(21)]
    path('Curved palm trunk',points,.19,'bark_ridged',14)
    for i in range(int(height*8)):
        y=i/8;torus('Palm growth scar',(.36*math.sin(y/height*20*.19),y,.08*y/height),.194,.012,'wood_oiled',major=14,minor=4)
    for j in range(13):
        a=j*2.39996;L=rng.uniform(2.2,3.5);start=Vector(points[-1]);tip=start+Vector((math.cos(a)*L,-.5-rng.random(),math.sin(a)*L))
        mid=(start+tip)*.5+Vector((0,1.3,0));path('Frond rachis',[start,start*.25+mid*.75,mid,mid*.4+tip*.6,tip],.022,'leaf_middle',7)
        if fan:
            for k in range(15):
                angle=a+(k-7)*.085;leaves.leaf(mid,tip+Vector((math.cos(angle)*.6,0,math.sin(angle)*.6)),.085,2 if k%3==0 else 1,.13)
        else:
            for k in range(15):
                t=.18+k*.049;center=start*(1-t)*(1-t)+mid*(2*t*(1-t))+tip*t*t
                for s in (-1,1):
                    side=Vector((-math.sin(a),-.12,math.cos(a)))*s*(.6*math.sin(t*math.pi)+.12)
                    leaves.leaf(center,center+side+Vector((math.cos(a)*.32,-.26,math.sin(a)*.32)),.045,1 if k%3 else 2,.02)
    leaves.object()
palm('palm_fan',9,True,333);palm('palm_feather',11,False,555)

root('shrub_flowering');rng=random.Random(435);leaves=Leaves('Flowering shrub foliage')
for j in range(25):
    a=j*2.39996;end=Vector((math.cos(a)*rng.uniform(.35,.95),rng.uniform(.65,1.4),math.sin(a)*rng.uniform(.35,.95)))
    tube('Shrub branch',(0,0,0),end,.016,'bark_ridged',6)
    for k in range(22):
        pos=end* rng.uniform(.3,1)+Vector((rng.uniform(-.25,.25),rng.uniform(-.12,.12),rng.uniform(-.25,.25)))
        angle=rng.uniform(0,6.28);leaves.leaf(pos,pos+Vector((math.cos(angle)*.21,.03,math.sin(angle)*.21)),.060,1 if k%3 else 2,.025)
    for k in range(5):
        angle=k*math.pi*.4;leaves.leaf(end,end+Vector((math.cos(angle)*.13,.025,math.sin(angle)*.13)),.053,4,.03)
leaves.object()

root('fern_arching');leaves=Leaves('Fern divided pinnae')
for j in range(13):
    a=j*2.399963;L=.74+(j%4)*.15
    points=[(math.cos(a)*L*t,math.sin(t*math.pi*.85)*.76,math.sin(a)*L*t) for t in [k/12 for k in range(13)]]
    path('Fern central rachis',points,.007,'leaf_middle',5)
    for k in range(1,12):
        center=Vector(points[k]);t=k/12;w=.24*math.sin(t*math.pi)**.7
        for s in (-1,1):
            side=Vector((-math.sin(a)*s,.10,math.cos(a)*s))
            leaves.leaf(center,center+side*w+Vector((math.cos(a)*.075,0,math.sin(a)*.075)),.024,1 if k%3 else 2,.018)
leaves.object()
root('phormium');leaves=Leaves('Flax strap leaves')
for j in range(29):
    a=j*2.39996;L=.72+(j%7)*.12
    leaves.leaf((0,0,0),(math.cos(a)*L*.52,L*.75,math.sin(a)*L*.52),.035,3 if j%4==0 else 1,.22)
leaves.object()
root('climber_cascade');leaves=Leaves('Climbing vine leaves');rng=random.Random(942)
for j in range(11):
    x=(j-5)*.15;points=[(x+math.sin(k*.7+j)*.12,-k*.27,math.cos(k*.56+j)*.11) for k in range(15)]
    path('Hanging stem',points,.006,'bark_ridged',5)
    for k,p in enumerate(points[1:]):
        for s in (-1,1):
            tip=Vector(p)+Vector((s*.17,-.1,.14));leaves.leaf(p,tip,.095,0 if k%3==0 else 1,.035)
leaves.object()
root('groundcover');leaves=Leaves('Low ground cover');rng=random.Random(99)
for j in range(110):
    a=j*2.39996;r=math.sqrt(j/110)*.9;pos=(math.cos(a)*r,0,math.sin(a)*r)
    leaves.leaf(pos,(pos[0]+rng.uniform(-.14,.14),rng.uniform(.08,.20),pos[2]+rng.uniform(-.14,.14)),.043,1 if j%4 else 2,.035)
leaves.object()

def prepare_mesh(obj):
    bm=bmesh.new();bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=1e-7)
    bmesh.ops.dissolve_degenerate(bm,dist=1e-8,edges=list(bm.edges))
    bmesh.ops.triangulate(bm,faces=list(bm.faces))
    bmesh.ops.delete(bm,geom=[f for f in bm.faces if f.calc_area()<1e-8],context='FACES')
    bm.to_mesh(obj.data);bm.free();obj.data.update();uv(obj)

# Weld leaf poles before simplification. Otherwise the decimator sees degenerate
# disconnected strips and retains nearly the entire full-resolution canopy.
for obj in list(scene.objects):
    if obj.type=='MESH':prepare_mesh(obj)

# Medium vegetation removes subpixel curvature while retaining the same origin,
# materials and branching silhouette. Unique mesh resources remain instanced.
plant_names=('canopy_broadleaf','canopy_columnar','tree_multistem','palm_fan','palm_feather',
             'shrub_flowering','fern_arching','phormium','climber_cascade','groundcover')
for name in plant_names:
    original=bpy.data.objects[name]
    root(name+'_mid')
    for source in original.children:
        obj=source.copy();obj.data=source.data.copy();scene.collection.objects.link(obj);obj.parent=current
        bpy.context.view_layer.objects.active=obj
        if len(obj.data.polygons)>50:
            mod=obj.modifiers.new('Medium silhouette simplification','DECIMATE');mod.ratio=.16
            mod.use_collapse_triangulate=True
            bpy.ops.object.modifier_apply(modifier=mod.name)
        # Simplified laminae retain optical coverage at distances where each
        # individual leaf is subpixel; the full resource retains exact shapes.
        if name in ('canopy_broadleaf','canopy_columnar','tree_multistem') and '_leaves' in source.name:
            mesh=obj.data;parents=list(range(len(mesh.vertices)))
            def representative(i):
                while parents[i]!=i:parents[i]=parents[parents[i]];i=parents[i]
                return i
            for edge in mesh.edges:
                a,b=(representative(v) for v in edge.vertices);parents[a]=b
            components={}
            for vertex in mesh.vertices:components.setdefault(representative(vertex.index),[]).append(vertex)
            for vertices in components.values():
                if len(vertices)<3:continue
                centre=sum((v.co for v in vertices),Vector())/len(vertices)
                for vertex in vertices:vertex.co=centre+(vertex.co-centre)*1.24
        prepare_mesh(obj)

# Export precisely the private asset scene; never replace a running user's scene.
for obj in scene.objects:obj.select_set(True)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'human_tech_kit.blend'),check_existing=False)
bpy.ops.export_scene.gltf(filepath=str(out/'human_tech_kit.authoring.glb'),export_format='GLB',use_selection=True,
    use_active_scene=True,export_yup=True,export_apply=True,export_texcoords=True,export_normals=True,export_tangents=True,
    export_materials='EXPORT',export_extras=True,export_cameras=False,export_lights=False,
    export_animations=False,export_unused_images=False)
print('Authored',sum(o.type=='EMPTY' and o.get('resource',False) for o in scene.objects),'resources')
