import struct
import sys
from pathlib import Path
import unittest
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from script_sdk import inspect


def program():
    name = b'TestFunction'
    header = struct.pack('<4sIIIII', b'#scp', 24, 1, 56, 1, 0)
    function = struct.pack('<IBBBBIIIIII', 68, 1, 2, 3, 0, 0, 64, 0, 0,
                           zlib.crc32(name) ^ 0xffffffff, 0xc0000045)
    return header + function + struct.pack('<III', 0xc0000052, 12, 0x80000002) + b'\0' + name + b'\0GlobalThing\0'


class ScriptTests(unittest.TestCase):
    def test_metadata(self):
        result = inspect(program())
        self.assertEqual(result['function_count'], 1)
        self.assertEqual(result['raw_variable_metadata'], [0xc0000052, 12])
        self.assertEqual(result['variable_records'], [dict(index=0, name='GlobalThing', raw_name=0xc0000052, raw_value=12)])
        record = result['functions'][0]
        self.assertEqual(record['name'], 'TestFunction')
        self.assertEqual(record['raw_input_metadata'], [0x80000002])
        self.assertEqual((record['input_count'], record['default_count'], record['flags0'], record['flags1']), (1, 0, 2, 3))

    def test_reject_corruption(self):
        for offset, value in ((0, 0), (8, 0xff), (24, 0xff), (52, 0), (48, 0)):
            data = bytearray(program())
            data[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect(data)
        with self.assertRaises(ValueError):
            inspect(program()[:-1])

    def test_variable_records_are_bounded_pairs(self):
        for offset, value in [(56, 0xffffffff), (56, 0x40000000), (12, len(program()) - 4), (16, 0xffffffff)]:
            data = bytearray(program())
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                inspect(data)
        data = bytearray(program())
        struct.pack_into('<I', data, 16, 0)
        struct.pack_into('<I', data, 20, 1)
        self.assertEqual(inspect(data)['variable_records'][0]['name'], 'GlobalThing')


if __name__ == '__main__':
    unittest.main()
