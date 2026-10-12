from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from compare_mdl import compare
from mdl_tests import fixture
from collision_inspect import inspect


class CompareModelTests(unittest.TestCase):
    def test_collision_tree_membership_and_bad_references(self):
        def node(start,end,ids):
            return struct.pack('<6fiiI',0,0,0,1,1,1,start,end,len(ids))+struct.pack('<'+'I'*len(ids),*ids)
        prefix=struct.pack('<3fI3fII',0,0,0,0,1,1,1,0,1)+struct.pack('<16f',*([0]*16))
        raw=prefix+struct.pack('<I',1)+node(-1,-1,[0])+struct.pack('<I',64)
        d=inspect(raw)
        self.assertTrue(d['all_triangles_referenced'])
        self.assertEqual(d['reachable_nodes'],1)
        bad=prefix+struct.pack('<I',1)+node(0,0,[])+struct.pack('<I',64)
        with self.assertRaisesRegex(ValueError,'cycle'):inspect(bad)
        bad=prefix+struct.pack('<I',1)+node(-1,-1,[1])+struct.pack('<I',64)
        with self.assertRaisesRegex(ValueError,'Invalid collision'):inspect(bad)

    def test_equal_copy_is_not_native_acceptance(self):
        result=compare(fixture(),fixture())
        self.assertTrue(result['byte_equal'])
        self.assertTrue(result['decoded_mesh_streams_equal'])
        self.assertFalse(result['native_loading_verified'])
        self.assertFalse(result['collision_semantics_verified'])

    def test_changed_geometry_is_reported(self):
        result=compare(fixture(),fixture(positions=(0,0,0,2,0,0,0,1,0)))
        self.assertFalse(result['byte_equal'])
        self.assertFalse(result['decoded_mesh_streams_equal'])
        self.assertFalse(result['normalized_stream_values_equal'])

    def test_snorm_endpoint_equivalence_does_not_hide_other_changes(self):
        a=fixture(normal=(-128,0,0,0));b=fixture(normal=(-127,0,0,0))
        result=compare(a,b)
        self.assertFalse(result['decoded_mesh_streams_equal'])
        self.assertTrue(result['normalized_stream_values_equal'])
        self.assertEqual(result['snorm_endpoint_byte_changes'],{'1':3})
        self.assertFalse(compare(a,fixture(normal=(-126,0,0,0)))['normalized_stream_values_equal'])


if __name__=='__main__':unittest.main()
