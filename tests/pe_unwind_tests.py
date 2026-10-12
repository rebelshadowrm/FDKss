"""Original Sky unwind families and refusal of unsafe metadata boundaries."""
from publication_fixtures import private_evidence
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from pe_unwind import FunctionFamilies


class UnwindTests(unittest.TestCase):
    @private_evidence
    def originals(self):
        return json.loads((Path(__file__).parent / 'fixtures/battle_unwind_originals.json').read_text())

    def restore(self, row):
        image = bytearray(row['image_size'])
        for blob in row['unwind']:
            data = bytes.fromhex(blob['code_hex'])
            self.assertEqual(hashlib.sha256(data).hexdigest(), blob['sha256'])
            image[blob['rva']:blob['rva']+len(data)] = data
        return image

    def test_original_families_from_every_part(self):
        for row in self.originals():
            with self.subTest(game=row['game'], name=row['name']):
                families = FunctionFamilies(self.restore(row), row['records'], row['executable'])
                for begin, end, _ in row['records']:
                    self.assertEqual(families.containing(end-1),
                                     (tuple(row['primary']), tuple(map(tuple, row['records']))))
                    self.assertEqual(families.contiguous(begin), tuple(row['expected_span']))
        first = next(r for r in self.originals() if r['game']=='sky1' and r['name']=='entry_update')
        self.assertEqual(first['expected_span'], [0x5ca90, 0x5ce49])
        self.assertEqual(len(first['records']), 5)
        self.assertEqual(first['records'][0][1]-first['records'][0][0], 16)

    def test_unregistered_or_tampered_parent(self):
        row = next(r for r in self.originals() if r['name']=='entry_update' and r['game']=='sky1')
        image = self.restore(row)
        child = row['records'][1]
        tail = child[2]+4+((image[child[2]+2]+1)&~1)*2
        struct.pack_into('<I', image, tail+8, row['primary'][2]+4)
        with self.assertRaisesRegex(ValueError, 'exact runtime-function'):
            FunctionFamilies(image, row['records'], row['executable'])
        with self.assertRaises(ValueError):
            FunctionFamilies(self.restore(row), row['records'][1:], row['executable'])

    def test_versions_flags_and_truncation(self):
        row = next(r for r in self.originals() if r['name']=='remove' and r['game']=='sky1')
        unwind = row['records'][1][2]
        for flags in (0, 3, 0x29, 0x41):
            image = self.restore(row);image[unwind] = flags
            with self.assertRaises(ValueError):
                FunctionFamilies(image, row['records'], row['executable'])
        image = self.restore(row)
        truncated = image[:unwind+4]
        # Keep only executable bounds actually inside the truncated image.
        with self.assertRaisesRegex(ValueError, 'truncated'):
            FunctionFamilies(truncated, row['records'], [(row['primary'][0], row['expected_span'][1])])

    def synthetic(self):
        image = bytearray(0x1000)
        records = [(0x100,0x110,0x800),(0x120,0x130,0x820),(0x300,0x310,0x840)]
        image[0x800:0x808] = bytes.fromhex('0104010004020000')
        image[0x820:0x828] = bytes.fromhex('2105020005340300')
        struct.pack_into('<III', image, 0x828, *records[0])
        image[0x840:0x844] = bytes.fromhex('01000000')
        return image, records, [(0x100,0x400)]

    def test_noncontiguous_ranges_do_not_include_gaps_or_neighbor(self):
        image, records, executable = self.synthetic()
        families = FunctionFamilies(image, records, executable)
        self.assertEqual(families.containing(0x125), (records[0], tuple(records[:2])))
        self.assertEqual(families.contiguous(0x305), (0x300,0x310))
        with self.assertRaisesRegex(ValueError, 'noncontiguous'):
            families.contiguous(0x100)
        for missing in (0xff,0x110,0x119,0x130,0x310):
            with self.assertRaises(ValueError):
                families.containing(missing)

    def test_cycles_are_refused(self):
        image, records, executable = self.synthetic()
        image[0x800:0x804] = bytes.fromhex('21000000')
        struct.pack_into('<III', image, 0x804, *records[1])
        with self.assertRaisesRegex(ValueError, 'cyclic'):
            FunctionFamilies(image, records, executable)

    def test_bad_runtime_table_and_sections(self):
        image, records, executable = self.synthetic()
        for rows in (records+[records[0]], [records[0],(0x108,0x130,0x820)],
                     [(0x100,0x110,0xfff)], [(0x100,0x110,0x801)], [(0x100,0x1101,0x800)]):
            with self.assertRaises(ValueError):
                FunctionFamilies(image, rows, executable)
        for sections in ([(0x101,0x400)],[(0x100,0x1001)]):
            with self.assertRaises(ValueError):
                FunctionFamilies(image, records, sections)


if __name__ == '__main__':
    unittest.main()
