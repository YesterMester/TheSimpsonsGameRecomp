"""Decoded vertex/index data retains native geometry before GPU upload."""
from array import array
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'launcher'))
from modkit_geometry import mesh_buffers


class PreviewGeometry(unittest.TestCase):
    def document(self):
        return {'geometries':[{'positions':[[1,2,3],[4,2,3],[1,6,3]],'triangles':[0,1,2],'normals':[[0,0,1]]*3}]}

    def test_original_coordinates_indices_and_normals_reach_the_gpu_buffer(self):
        vertices,indices,count,center,radius=mesh_buffers(self.document())
        decoded=array('f');decoded.frombytes(vertices)
        topology=array('I');topology.frombytes(indices)
        self.assertEqual(list(decoded),[1,2,3,0,0,1,4,2,3,0,0,1,1,6,3,0,0,1])
        self.assertEqual(list(topology),[0,1,2]);self.assertEqual(count,3)
        self.assertEqual(center,[2.5,4,3]);self.assertEqual(radius,2.5)

    def test_missing_normals_are_preview_only_and_source_data_is_untouched(self):
        document=self.document();document['geometries'][0]['normals']=[];before=copy.deepcopy(document)
        vertices,_,_,_,_=mesh_buffers(document)
        values=array('f');values.frombytes(vertices)
        self.assertEqual(list(values[3:6]),[0,0,1]);self.assertEqual(document,before)

    def test_invalid_topology_and_nonfinite_positions_never_reach_the_driver(self):
        for key,value in [('triangles',[0,1,99]),('triangles',[0,1]),('positions',[[float('nan'),0,0]]*3)]:
            document=self.document();document['geometries'][0][key]=value
            with self.assertRaises(ValueError):mesh_buffers(document)


if __name__=='__main__':unittest.main()
