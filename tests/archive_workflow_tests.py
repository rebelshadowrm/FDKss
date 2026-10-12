from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from archive_workflow import inputs,pack


def fpac(payload,name=b'asset/file.bin'):
    header=48+len(name)+1
    return struct.pack('<4sIII',b'FPAC',1,header,1)+struct.pack('<IIQQQ',zlib.crc32(name)^0xffffffff,0,48,len(payload),header)+name+b'\0'+payload


class ArchiveWorkflowTests(unittest.TestCase):
    def test_input_budget_and_workspace_refusal(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'assets';source.mkdir();(source/'a').write_bytes(b'data')
            tool=root/'tool';tool.write_bytes(b'fixture')
            with self.assertRaises(ValueError):pack(source,source/'output','p3a',tool)
            with self.assertRaises(ValueError):pack(source,root/'output','fpac',tool)
            with patch('archive_workflow.MAX_TOTAL',3),self.assertRaisesRegex(ValueError,'budget'):inputs(source)

    def test_fpac_output_content_and_originals(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'assets';(source/'asset').mkdir(parents=True);(source/'asset/file.bin').write_bytes(b'original')
            tool=root/'tool';tool.write_bytes(b'fixture')
            def invoke(argv,cwd,**kw):
                (Path(cwd)/'out.pac').write_bytes(fpac(b'original'))
                return type('R',(),{'returncode':0,'stdout':b'ok','stderr':b''})()
            with patch('archive_workflow.subprocess.run',side_effect=invoke):
                report=pack(source,root/'output','fpac',tool)
            self.assertEqual(report['state'],'verified')
            self.assertEqual(report['verified_files'],1)
            self.assertFalse(report['native_loading_verified'])
            self.assertEqual((source/'asset/file.bin').read_bytes(),b'original')

    def test_corrupt_external_output_remains_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'assets';(source/'asset').mkdir(parents=True);(source/'asset/file.bin').write_bytes(b'original')
            tool=root/'tool';tool.write_bytes(b'fixture')
            def invoke(argv,cwd,**kw):
                (Path(cwd)/'out.pac').write_bytes(fpac(b'changed'))
                return type('R',(),{'returncode':0,'stdout':b'','stderr':b''})()
            with patch('archive_workflow.subprocess.run',side_effect=invoke),self.assertRaisesRegex(ValueError,'content differs'):
                pack(source,root/'output','fpac',tool)
            self.assertEqual(json.loads((root/'output/manifest.json').read_text())['state'],'failed')
            self.assertEqual((source/'asset/file.bin').read_bytes(),b'original')


if __name__=='__main__':unittest.main()
