from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from fdk_json import decode, encode


class BinaryJsonTests(unittest.TestCase):
    def test_editable_roundtrip(self):
        original = {'Actor': [{'name': 'light', 'position': [1.25, -2, 3], 'enabled': True}],
                    'unicode': '\u00e9\u7a7a', 'empty': [], 'object': {}}
        binary = encode(original)
        self.assertEqual(decode(binary), original)
        edited = decode(binary)
        edited['Actor'][0]['position'][0] += 0.5
        self.assertEqual(decode(encode(edited))['Actor'][0]['position'][0], 1.75)
        for scalar in ('string', 1.25, False, [], {}):
            self.assertEqual(decode(encode(scalar)), scalar)

    def test_invalid_values(self):
        for value in (None, float('nan'), float('inf'), 2**53 + 1, 'has\0nul', {1: 'bad key'}):
            with self.subTest(value=value), self.assertRaises(ValueError): encode(value)
        nested = []; nested.append(nested)
        with self.assertRaises(ValueError): encode(nested)

    def test_bad_offsets_checksums_and_cycles(self):
        samples = [b'JSON', encode({'x': 1})[:-1]]
        crc = bytearray(encode({'x': 1})); crc[16] ^= 1; samples.append(crc)
        offset = bytearray(encode({'x': 1})); struct.pack_into('<I', offset, 8, len(offset)); samples.append(offset)
        cycle = bytearray(encode([1])); root, = struct.unpack_from('<I', cycle, 8)
        struct.pack_into('<I', cycle, root + 9, root); samples.append(cycle)
        count = bytearray(encode([1])); struct.pack_into('<I', count, root + 5, 0xffffffff); samples.append(count)
        for sample in samples:
            with self.subTest(sample=bytes(sample[:16])), self.assertRaises(ValueError): decode(sample)


if __name__ == '__main__':
    unittest.main()
