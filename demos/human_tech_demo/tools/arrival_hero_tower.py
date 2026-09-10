"""First Arrival graphite oval tower and its rounded occupied socket."""
import math
from arrival_tower_geometry import Builder, oval, rounded_rect, inset

SPEC={
    'arrival_hero':{'center':[72.82,301.39], 'base_y':13.2,'yaw':-.27,
                    'rx':28.80,'rz':16.74,'height':195.0,'floors':45,
                    'lean_world':[-2.50,-1.17],
                    'features':['elliptical plan','inclined rounded crown',
                                'continuous recessed graphite curtain wall',
                                'fine bronze vertical framing','rounded occupied socket']},
    'arrival_hero_socket':{'center':[72.82,301.39],'base_y':1.2,'yaw':-.27,
                           'rx':36.5,'rz':26,'height':12,'floors':3},
}


def build():
    p=SPEC['arrival_hero'];b=Builder('arrival_hero');h=p['height'];floors=p['floors']
    shaft=181.86;fh=shaft/floors;b.root['engine_room_h']=fh
    yaw=p['yaw'];dx,dz=p['lean_world']
    lean=(dx*math.cos(yaw)+dz*math.sin(yaw),-dx*math.sin(yaw)+dz*math.cos(yaw))
    def outline(y,extra=0):
        t=min(1,max(0,y/shaft));ease=t*t*(3-2*t)
        scale=1-.14*ease
        return oval(p['rx']*scale+extra,p['rz']*scale+extra,128,(lean[0]*ease,lean[1]*ease))
    def point(a,y,extra=0):
        t=min(1,max(0,y/shaft));ease=t*t*(3-2*t);s=1-.14*ease
        return ((p['rx']*s+extra)*math.cos(a)+lean[0]*ease,y,
                (p['rz']*s+extra)*math.sin(a)+lean[1]*ease)
    for floor in range(floors):
        y=floor*fh;top=y+fh
        b.profile('Continuous graphite glazing',[(y+.24,outline(y+.24)),(top-.12,outline(top-.12))],'glass_dark')
        b.profile('Insulated fine spandrel',[(y,outline(y)),(y+.24,outline(y+.24))],'graphite')
        b.profile('Recessed slab return',[(top-.12,outline(top-.12)),(top,outline(top))],'graphite')
        b.slab('Occupied floor plate',outline(y,-.025),y+.035,.19,'graphite')
        # Paired lift/core walls and cross-wall carry every changing floor.
        t=(y+top)*.5/h
        for sign in (-1,1):
            b.box('Continuous core',(lean[0]*t+sign*5,(y+top)*.5,lean[1]*t),(.25,fh*.5,7),'graphite',0)
        b.box('Core crosswall',(lean[0]*t,(y+top)*.5,lean[1]*t-5),(5.25,fh*.5,.25),'graphite',0)
        for panel in range(96):
            a=2*math.pi*panel/96
            b.beam('Curtain wall pressure plate',point(a,y+.20,.045),point(a,top-.10,.045),
                   .11 if panel%4==0 else .045,.12 if panel%4==0 else .065,
                   'bronze_dark' if panel%4 else 'bronze')
        b.ring('Fine bronze floor reveal',outline(top,.035),top-.11,.075,.05,
               'bronze_dark' if floor%4 else 'bronze')
    # A shallow inclined closure follows the body, with no projecting roof disc.
    lower=outline(shaft);n=len(lower);vertices=[];fractions=(0,.3,.55,.75,.9,1)
    for q in fractions:
        scale=.86-.22*(1-math.sqrt(max(0,1-q*q)))
        for i in range(n):
            a=2*math.pi*i/n;top=h-10*(.5+.5*math.cos(a))
            vertices.append((p['rx']*scale*math.cos(a)+lean[0],shaft+q*(top-shaft),
                             p['rz']*scale*math.sin(a)+lean[1]))
    upper=vertices[-n:];faces=[];uvs=[];distance=[0]
    for i in range(n):distance.append(distance[-1]+math.dist(lower[i],lower[(i+1)%n]))
    for row in range(len(fractions)-1):
        for i in range(n):
            j=(i+1)%n;lo=row*n;hi=(row+1)*n;faces.append((lo+j,lo+i,hi+i,hi+j))
            uvs.append([(distance[i+1],vertices[lo+j][1]),(distance[i],vertices[lo+i][1]),
                        (distance[i],vertices[hi+i][1]),(distance[i+1],vertices[hi+j][1])])
    b.mesh('Curved inclined crown glazing',vertices,faces,'glass_dark',uvs,True)
    # Slightly recessed roof pan closes the full oblique perimeter.
    roof=[(x*.996,y-.13,z*.996) for x,y,z in upper]
    b.mesh('Closed sloping roof',roof,[tuple(reversed(range(n)))],'roof_dark')
    for i in range(n):
        j=(i+1)%n
        b.beam('Rolled crown rim',upper[i],upper[j],.28,.24,'ivory_edge')
        if i%4==0:
            for row in range(len(fractions)-1):
                b.beam('Crown continuation',vertices[row*n+i],vertices[(row+1)*n+i],.095,.12,'bronze_dark')
    # Roof access is low and set well back from the visible outer rim.
    hatch=[]
    for dy in (-.06,.02):
        for x,z in ((-2,-1.5),(-2,1.5),(2,1.5),(2,-1.5)):
            hatch.append((x+lean[0],h-5-5*x/(p['rx']*.64)+dy,z+lean[1]))
    b.mesh('Flush crown maintenance hatch',hatch,[(3,2,1,0),(4,5,6,7),
           (0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)],'graphite')

    socket=Builder('arrival_hero_socket');socket.root['engine_room_h']=4.0
    footprint=rounded_rect(36.5,26,8.2,14)
    socket.slab('Ground-bearing mineral foundation',footprint,.0,.55,'stone')
    for floor in range(3):
        y=floor*4
        socket.slab('Occupied socket slab',footprint,y+.05,.24,'ivory')
        glazing=inset(footprint,.65)
        socket.profile('Rounded occupied socket glazing',[(y+.28,glazing),(y+3.55,glazing)],'glass_bronze')
        socket.ring('Socket shadow fascia',footprint,y+3.85,.8,.28,'bronze_dark')
        socket.ring('Rounded pale slab edge',footprint,y+4,.9,.16,'ivory')
        for i,(x,z) in enumerate(glazing):
            if i%2==0:socket.box('Socket facade pier',(x,y+2,z),(.15,1.85,.15),'ivory',.025)
        socket.box('Bearing core',(0,y+2,0),(6,2,7),'graphite',0)
    socket.slab('Continuous bearing roof',footprint,12,.36,'ivory')
    socket.ring('Plinth planted edge coping',inset(footprint,.6),12.28,.45,.28,'ivory')
    # The roof deck is deliberately legible; adjacent lot landscaping supplies
    # connected plant masses outside the shaft's support footprint.
    return [b,socket]
