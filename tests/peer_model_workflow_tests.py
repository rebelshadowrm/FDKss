from pathlib import Path
import hashlib
import struct
import sys
import unittest
import zstandard

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import asset_decode
from peer_model_workflow import animation_compare,section_data,restore_animation_words
from shader_compare import material_signature
from mdl_animation_tests import fixture


def pure_animation(version=4):
    name=b'\x04Root'
    hierarchy=struct.pack('<I',1)+name+struct.pack('<II3f4fI3f3f3fI',0,0xffffffff,0,0,0,0,0,0,1,0,0,0,0,1,1,1,0,0,0,0)
    motion=fixture()
    return b'MDL '+struct.pack('<II',version,42)+struct.pack('<II',2,len(hierarchy))+hierarchy+motion[12:]


class PeerModelWorkflowTests(unittest.TestCase):
    def test_d9ba_bounded_decode_and_identity(self):
        original=pure_animation();payload=zstandard.ZstdCompressor(write_checksum=True).compress(original)
        payload+=b'\0'*((-len(payload))%8)
        encoded=b'D9BA'+struct.pack('<I',len(payload))+payload
        decoded,wrappers=asset_decode.decode(encoded)
        self.assertEqual(decoded,original)
        self.assertEqual(wrappers[0]['input_sha256'],hashlib.sha256(encoded).hexdigest())
        for invalid in [encoded[:-1],b'F9BA'+bytes(8),b'C9BA'+bytes(8),b'D9BA'+struct.pack('<I',len(payload))+payload[:-1]+b'x']:
            with self.assertRaises(ValueError):asset_decode.decode(invalid)

    def test_animation_comparison_exposes_version_and_real_changes(self):
        a,b=pure_animation(4),pure_animation(1)
        report=animation_compare(a,b)
        self.assertEqual(report['rebuilt_version'],1)
        self.assertTrue(report['animation_records_equal'])
        self.assertTrue(report['sections_byte_equal'])
        self.assertFalse(report['native_loading_verified'])
        changed=bytearray(a);changed[-12:-8]=struct.pack('<f',1.0)
        self.assertFalse(animation_compare(a,changed)['sections_byte_equal'])
        with self.assertRaises(ValueError):section_data(a[:-3])

    def test_material_switch_signature_is_order_and_value_specific(self):
        base={'shader':'map','switches':[{'name':'ONE','value':1},{'name':'TWO','value':2}]}
        changed={'shader':'map','switches':[{'name':'ONE','value':0},{'name':'TWO','value':2}]}
        reversed_order={'shader':'map','switches':list(reversed(base['switches']))}
        self.assertNotEqual(material_signature(base),material_signature(changed))
        self.assertNotEqual(material_signature(base),material_signature(reversed_order))

    def test_imported_animation_unknown_words_restore_by_binding(self):
        original=pure_animation();modified=bytearray(original)
        # Locate the track's kind/word tuple through the independently parsed section.
        _,sections=section_data(original)
        marker=b'\x05track\x04Root'+struct.pack('<4I',9,3,7,1)
        offset=original.index(marker)+len(b'\x05track\x04Root')
        struct.pack_into('<2I',modified,offset+4,0,1)
        restored,count=restore_animation_words(original,modified)
        self.assertEqual(restored,original)
        self.assertEqual(count,1)


if __name__=='__main__':unittest.main()
