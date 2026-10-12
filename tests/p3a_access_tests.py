from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import xxhash
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from p3a_access import inventory, extract, extract_entry, safe_name


def archive(payload=b'example', name=b'asset/example.bin'):
    header=struct.pack('<8sIIQ',b'PH3ARCV\0',0,1200,1)
    header+=struct.pack('<Q',xxhash.xxh64(header).intdigest())
    ext=struct.pack('<II',16,304)
    ext=struct.pack('<Q',xxhash.xxh64(ext).intdigest())+ext
    entry=name.ljust(256,b'\0')+struct.pack('<6Q',0,len(payload),len(payload),352,
                         xxhash.xxh64(payload).intdigest(),xxhash.xxh64(payload).intdigest())
    return header+ext+entry+payload


class P3ATests(unittest.TestCase):
    def test_selected_entry_checksum_and_independent_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source_dir=root/'source';source_dir.mkdir()
            source=source_dir/'data.p3a';source.write_bytes(archive())
            report=extract_entry(source,'asset/example.bin',root/'output/data.bin')
            self.assertEqual((root/'output/data.bin').read_bytes(),b'example')
            self.assertTrue(report['originals_unchanged'])
            with self.assertRaises(ValueError):extract_entry(source,'asset/example.bin',source_dir/'out')
            with self.assertRaises(ValueError):extract_entry(source,'missing',root/'missing')
            changed=bytearray(archive());changed[-1]^=1;source.write_bytes(changed)
            with self.assertRaisesRegex(ValueError,'checksum'):extract_entry(source,'asset/example.bin',root/'bad')

    def test_directory_and_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'sample.p3a';path.write_bytes(archive())
            out=inventory(path)
            self.assertEqual(out['entries'][0]['name'],'asset/example.bin')
            self.assertEqual(out['expanded_bytes'],7)
            with self.assertRaises(ValueError):inventory(path,6)
            for raw in [archive()[:30],archive()[:70],b'BAD!'+archive()[4:]]:
                path.write_bytes(raw)
                with self.assertRaises(ValueError):inventory(path)

    def test_rejects_entry_bounds_hashes_and_unknown_layouts(self):
        base=archive()
        samples=[]
        for offset,value in [(48+256,4),(48+264,1000),(48+272,129*1024**2),(48+280,0)]:
            data=bytearray(base);struct.pack_into('<Q',data,offset,value);samples.append(bytes(data))
        for offset in (24,32):
            data=bytearray(base);data[offset]^=1;samples.append(bytes(data))
        data=bytearray(base);struct.pack_into('<I',data,44,312);samples.append(bytes(data))
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'a.p3a'
            for raw in samples:
                path.write_bytes(raw)
                with self.assertRaises(ValueError):inventory(path)

    def test_rejects_unsafe_names(self):
        for name in [b'../outside',b'/absolute',b'C:/file',b'file\\path',b'file/./part',b'CON.txt',b'a./b']:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):safe_name(name.ljust(256,b'\0'))
        with self.assertRaises(ValueError):safe_name(b'a'*256)

    def test_validated_output_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'source').mkdir()
            source=root/'source/archive.p3a';source.write_bytes(archive());tool=root/'tool.exe';tool.write_bytes(b'fixture')
            calls=[]
            def run(argv,cwd,**kwargs):
                calls.append(argv[1])
                if argv[1]=='extract':
                    dest=Path(cwd)/'asset/example.bin';dest.parent.mkdir();dest.write_bytes(b'example')
                return type('Result',(),{'returncode':0,'stdout':b'ok','stderr':b''})()
            with patch('p3a_access.subprocess.run',side_effect=run):
                out=extract(source,tool,root/'out')
            self.assertEqual(calls,['verify','extract'])
            self.assertTrue(out['source_unchanged'])
            self.assertEqual(source.read_bytes(),archive())
            self.assertEqual(out['state'],'offline_content_verified')
            with self.assertRaises(ValueError):extract(source,tool,root/'out')
            with self.assertRaises(ValueError):extract(source,tool,root/'source/out')

    def test_content_mismatch_records_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'source').mkdir()
            source=root/'source/a.p3a';source.write_bytes(archive());tool=root/'tool';tool.write_bytes(b'tool')
            def run(argv,cwd,**kwargs):
                if argv[1]=='extract':
                    dest=Path(cwd)/'asset/example.bin';dest.parent.mkdir();dest.write_bytes(b'corrupt')
                return type('Result',(),{'returncode':0,'stdout':b'', 'stderr':b''})()
            with patch('p3a_access.subprocess.run',side_effect=run),self.assertRaisesRegex(ValueError,'content checksum'):
                extract(source,tool,root/'out')
            self.assertIn('"state": "failed"',(root/'out/manifest.json').read_text())
            self.assertEqual(source.read_bytes(),archive())


if __name__=='__main__':unittest.main()
