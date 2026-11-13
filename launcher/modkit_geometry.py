"""Pure geometry preparation shared by offline preview workers and editor tools."""
from array import array
import math

def mesh_buffers(document):
    """Keep native coordinates/topology. Derived preview normals never enter mods."""
    vertices=array('f'); indices=array('I'); offset=0
    low=[math.inf]*3; high=[-math.inf]*3
    for geometry in document['geometries']:
        positions=geometry['positions']; triangles=geometry['triangles']; normals=geometry.get('normals',[])
        if offset+len(positions)>1000000 or len(indices)+len(triangles)>6000000:
            raise ValueError('This preview exceeds one million vertices or two million triangles')
        if len(triangles)%3 or any(not isinstance(i,int) or i<0 or i>=len(positions) for i in triangles):
            raise ValueError('The native triangle stream is invalid')
        if len(normals)!=len(positions):
            normals=[[0.,0.,0.] for _ in positions]
            for index in range(0,len(triangles),3):
                a,b,c=(positions[triangles[index+j]] for j in range(3))
                u=[b[k]-a[k] for k in range(3)];v=[c[k]-a[k] for k in range(3)]
                n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
                for j in range(3):
                    target=normals[triangles[index+j]]
                    for k in range(3):target[k]+=n[k]
        for position,normal in zip(positions,normals):
            if len(position)!=3 or not all(math.isfinite(float(n)) for n in position):
                raise ValueError('Native positions must be finite XYZ values')
            length=math.sqrt(sum(n*n for n in normal)) or 1
            vertices.extend(position);vertices.extend(n/length for n in normal)
            for k in range(3):low[k]=min(low[k],position[k]);high[k]=max(high[k],position[k])
        indices.extend(i+offset for i in triangles);offset+=len(positions)
    if not offset or not indices:raise ValueError('No decoded native triangles are available')
    center=[(low[k]+high[k])*.5 for k in range(3)]
    radius=max(.01,math.sqrt(sum((high[k]-low[k])**2 for k in range(3)))*.5)
    return vertices.tobytes(),indices.tobytes(),len(indices),center,radius

