import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from pac import extract, inventory


def archive(name=b'asset/example.bin', payload=b'\0example\xff'):
    name_offset = 48
    header_size = name_offset + len(name) + 1
    return (struct.pack('<4sIII', b'FPAC', 1, header_size, 1) +
            struct.pack('<IIQQQ', zlib.crc32(name) ^ 0xFFFFFFFF, 0, name_offset, len(payload), header_size) +
            name + b'\0' + payload)


class PacTests(unittest.TestCase):
    def test_duplicate_inventory_and_explicit_index_extraction(self):
        name=b'asset/example.bin';header=80+len(name)+1
        raw=struct.pack('<4sIII',b'FPAC',2,header,1)
        raw+=b''.join(struct.pack('<IIQQQ',zlib.crc32(name)^0xffffffff,0,80,1,header+i) for i in range(2))
        raw+=name+b'\0AB'
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'duplicate.pac';path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,'Duplicate'):inventory(path)
            self.assertEqual(len(inventory(path,allow_duplicates=True)),2)
            with self.assertRaises(ValueError):extract(path,name.decode(),Path(directory)/'ambiguous')
            report=extract(path,name.decode(),Path(directory)/'second',entry_index=1)
            self.assertEqual((Path(directory)/'second').read_bytes(),b'B')
            self.assertEqual(report['directory_index'],1)
            for index in [-1,2]:
                with self.assertRaises(ValueError):extract(path,name.decode(),Path(directory)/'bad',entry_index=index)
            with self.assertRaises(ValueError):extract(path,'wrong/name',Path(directory)/'bad',entry_index=1)

    def test_roundtrip_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'sample.pac'
            original = archive()
            source.write_bytes(original)
            entries = inventory(source)
            self.assertEqual(entries[0].name, 'asset/example.bin')
            report = extract(source, 'asset/example.bin', root / 'nested/file.bin')
            self.assertEqual((root / 'nested/file.bin').read_bytes(), b'\0example\xff')
            self.assertEqual(report['sha256'], hashlib.sha256(b'\0example\xff').hexdigest())
            self.assertEqual(source.read_bytes(), original)
            with self.assertRaises(ValueError): extract(source, 'asset/example.bin', source)
            alias = root / 'alias.pac'
            os.link(source, alias)
            with self.assertRaises(ValueError): extract(source, 'asset/example.bin', alias)
            self.assertEqual(source.read_bytes(), original)
            with self.assertRaises(ValueError): extract(source, 'missing', root / 'missing.bin')

    def test_malformed_directory_and_paths(self):
        samples = [archive()[:10], archive(b'../escape'), archive(b'C:/escape'), archive(b'/escape'),
                   archive(b'asset//name'), archive(b'asset/./name')]
        checksum = bytearray(archive()); checksum[16] ^= 1; samples.append(checksum)
        offset = bytearray(archive()); struct.pack_into('<Q', offset, 40, 0xFFFFFFFFFFFFFFFF); samples.append(offset)
        name_offset = bytearray(archive()); struct.pack_into('<Q', name_offset, 24, 12); samples.append(name_offset)
        for sample in samples:
            with self.subTest(sample=bytes(sample[:16])), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'bad.pac'; path.write_bytes(sample)
                with self.assertRaises(ValueError): inventory(path)


if __name__ == '__main__':
    unittest.main()
