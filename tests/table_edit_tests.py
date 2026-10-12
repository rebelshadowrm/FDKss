import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from table_edit import edit
from table_inspect_tests import table,schemas
from table_inspect import decode


class TableEditTests(unittest.TestCase):
    def test_scalar_string_and_array_edits_preserve_original_tail(self):
        fields={'id':'u32','text':'ptr_str_utf8','values':'arr_u16'}
        record=struct.pack('<IQ QI',7,112,116,2)
        raw=table(record,b'old\0'+struct.pack('<2H',1,2))
        operations=[{'directory_index':0,'record_index':0,'field':'id','value':9},
                    {'directory_index':0,'record_index':0,'field':'text','value':'replacement'},
                    {'directory_index':0,'record_index':0,'field':'values','value':[3,4,5]}]
        document={'input_sha256':hashlib.sha256(raw).hexdigest(),'edits':operations}
        output,report=edit(raw,schemas(fields),'ysx',None,document)
        self.assertEqual(output[112:120],raw[112:120])
        self.assertEqual(decode(output,schemas(fields),'ysx')['headers'][0]['records'][0]['fields'],
                         {'id':9,'text':'replacement','values':[3,4,5]})
        self.assertFalse(report['native_loading_verified'])
        self.assertEqual(raw,table(record,b'old\0'+struct.pack('<2H',1,2)))

    def test_identity_opaque_overflow_and_duplicate_targets_refused(self):
        raw=table(struct.pack('<I',1));selected=schemas({'id':'u32'})
        op={'directory_index':0,'record_index':0,'field':'id','value':2}
        document={'input_sha256':hashlib.sha256(raw).hexdigest(),'edits':[op]}
        for bad in [dict(document,input_sha256='wrong'),dict(document,edits=[op,op]),
                    dict(document,edits=[dict(op,value=-1)]),dict(document,edits=[dict(op,field='unknown')]),
                    dict(document,edits=[dict(op,record_index=1)])]:
            with self.assertRaises(ValueError):edit(raw,selected,'ysx',None,bad)
        with self.assertRaises(ValueError):edit(raw,schemas({'id':'d4'}),'ysx',None,document)

    def test_nested_float32_edits_are_compared_after_serialization(self):
        fields={'data':{'repeat':2,'type':{'x':'f32','id':'u32'}}}
        raw=table(struct.pack('<fIfI',0,1,0,2))
        op={'directory_index':0,'record_index':0,'field':'data','value':[{'x':1.1,'id':3},{'x':2.2,'id':4}]}
        output,report=edit(raw,schemas(fields),'ysx',None,{'input_sha256':hashlib.sha256(raw).hexdigest(),'edits':[op]})
        self.assertTrue(report['independent_reinspection_equal'])
        self.assertEqual(decode(output,schemas(fields),'ysx')['headers'][0]['records'][0]['fields']['data'][0]['x'],
                         struct.unpack('<f',struct.pack('<f',1.1))[0])


if __name__=='__main__':unittest.main()
