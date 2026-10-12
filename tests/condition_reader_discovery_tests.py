"""Candidate discovery must validate the native search loop and its exits."""
from publication_fixtures import private_evidence
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_condition_readers import pattern, scan


class ConditionReaderDiscoveryTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.rows = json.loads((Path(__file__).parent / 'fixtures/condition_reader_fragments.json').read_text())

    def image(self, row):
        data = bytearray(0x1000)
        body = bytes.fromhex(row['code_hex'])
        data[0x100:0x100 + len(body)] = body
        return data

    def test_relocated_original_fragments_and_distinct_offsets(self):
        for row in self.rows:
            found = scan(self.image(row), [(0x100, 0x1000)])
            self.assertEqual(len(found), 1)
            for field in ('character_link_offset', 'manager_link_offset', 'vector_pointer_offset',
                          'vector_count_offset', 'record_stride', 'code_hex'):
                self.assertEqual(found[0][field], row[field])
            self.assertEqual(found[0]['reader_fragment_rva'], '0x100')

    def test_changed_layout_is_discovered_without_profile_addresses(self):
        for row in self.rows:
            image = self.image(row)
            match = pattern(row['encoding'].startswith('rsi')).search(image)
            for field, offset in [('context', 0x400), ('link', 0x2200), ('manager', 0x680),
                                  ('pointer', 0xa00), ('count', 0xa08)]:
                struct.pack_into('<i', image, match.start(field), offset)
            found = scan(image, [(0x100, 0x1000)])
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0]['character_link_offset'], 0x2200)
            self.assertEqual(found[0]['manager_link_offset'], 0x680)
            self.assertEqual(found[0]['vector_pointer_offset'], 0xa00)

    def test_bad_branches_offsets_stride_and_section_bounds_are_rejected(self):
        for row in self.rows:
            original = self.image(row)
            match = pattern(row['encoding'].startswith('rsi')).search(original)
            positions = [match.end(field)-1 for field in ('no_actor', 'no_link', 'empty', 'loop', 'miss')]
            positions += [match.start('key')+2, match.end('hit')+3]
            for at in positions:
                image = original[:];image[at] ^= 1
                self.assertEqual(scan(image, [(0x100, 0x1000)]), [], (row['encoding'], at))
            for field, value in [('context', -8), ('link', 0x2201), ('manager', 0), ('count', 0x920)]:
                image = original[:];struct.pack_into('<i', image, match.start(field), value)
                self.assertEqual(scan(image, [(0x100, 0x1000)]), [])
            self.assertEqual(scan(original, [(0, 0x100)]), [])
            self.assertEqual(scan(original, [(0x100, match.end()-1)]), [])
            # Branch destinations must remain in an executable range.
            self.assertEqual(scan(original, [(0x100, match.end())]), [])
        with self.assertRaises(ValueError):
            scan(b'abc', [(0, 4)])

    def test_multiple_candidates_are_preserved(self):
        image = self.image(self.rows[0])
        body = bytes.fromhex(self.rows[1]['code_hex'])
        image[0x500:0x500+len(body)] = body
        found = scan(image, [(0x100, 0x1000)])
        self.assertEqual([row['reader_fragment_rva'] for row in found], ['0x100', '0x500'])
        self.assertEqual(len({row['character_link_offset'] for row in found}), 2)


if __name__ == '__main__':
    unittest.main()
