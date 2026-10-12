"""Discover relocated dispatchers; refuse unsafe tables and leaf boundaries."""
from publication_fixtures import private_evidence
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_status_getters import DISPATCH, scan


class StatusGetterDiscoveryTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.rows = json.loads((Path(__file__).parent / 'fixtures/status_getter_dispatchers.json').read_text())

    def image(self, row, location=0x100):
        body = bytes.fromhex(row['pin_hex'])
        source = int(row['getter_rva'], 16)
        match = DISPATCH.match(body)
        old_indices = struct.unpack('<i', match.group('selectors'))[0]
        old_table = struct.unpack('<i', match.group('targets'))[0]
        image = bytearray(0x1000)
        image[location:location + len(body)] = body
        # RIP-relative base and both image-relative tables must follow relocation.
        struct.pack_into('<i', image, location + match.start('base'), -location - match.end('base'))
        for field in ('selectors', 'targets'):
            old = struct.unpack('<i', match.group(field))[0]
            struct.pack_into('<i', image, location + match.start(field), old - source + location)
        for offset in range(old_table - source, old_indices - source, 4):
            old = struct.unpack_from('<I', body, offset)[0]
            struct.pack_into('<I', image, location + offset, old - source + location)
        return image

    def test_original_dispatchers_relocated_with_every_leaf_and_complete_span(self):
        for row in self.rows:
            for location in (0x100, 0x500):
                found = scan(self.image(row, location), [(location, location + row['pin_size'])])
                self.assertEqual(len(found), 1)
                candidate = found[0]
                self.assertEqual(candidate['getter_candidate_rva'], hex(location))
                self.assertEqual(candidate['candidate_span_size'], row['pin_size'])
                self.assertEqual(len(candidate['leaf_read_candidates']), 9)
                for path in candidate['leaf_read_candidates']:
                    expected = row['selected_branches'][str(path['selector'])]
                    self.assertEqual(path['status_offset'], expected['status_offset'])
                    self.assertEqual(path['body_hex'], expected['body_hex'])
                    self.assertEqual(int(path['target_rva'], 16) - location,
                                     int(expected['target_rva'], 16) - int(row['getter_rva'], 16))

    def test_changed_offsets_are_extracted_without_gameplay_names(self):
        for row in self.rows:
            image = self.image(row)
            first = scan(image, [(0x100, 0x1000)])[0]
            for path in first['leaf_read_candidates']:
                image[int(path['target_rva'], 16) + 2] += 4
            found = scan(image, [(0x100, 0x1000)])[0]
            self.assertEqual([p['status_offset'] for p in found['leaf_read_candidates']],
                             [p['status_offset'] + 4 for p in first['leaf_read_candidates']])
            self.assertTrue(all('meaning' not in path for path in found['leaf_read_candidates']))

    def test_new_selector_range_and_disp32_leaf_encoding(self):
        image = bytearray(0x1000)
        body = bytearray(bytes.fromhex(
            '83fa060f87000000004863c24c8d0500000000410fb6840000000000'
            '418b9480000000004903d0ffe2'))
        match = DISPATCH.match(body)
        targets = []
        for offset in range(0x100, 0x118, 4):
            targets.append(0x100 + len(body))
            body += b'\x8b\x81' + struct.pack('<i', offset) + b'\xc3'
        default = 0x100 + len(body)
        targets.append(default)
        body += b'\x33\xc0\xc3'
        while len(body) % 4:
            body += b'\x90'
        table = 0x100 + len(body)
        body += struct.pack('<7I', *targets)
        indices = 0x100 + len(body)
        body += bytes(range(7))
        struct.pack_into('<i', body, match.end('default') - 4, default - 0x100 - match.end('default'))
        struct.pack_into('<i', body, match.start('base'), -0x100 - match.end('base'))
        struct.pack_into('<i', body, match.start('targets'), table)
        struct.pack_into('<i', body, match.start('selectors'), indices)
        image[0x100:0x100 + len(body)] = body
        found = scan(image, [(0x100, 0x1000)])[0]
        self.assertEqual(found['selector_maximum'], 6)
        self.assertEqual([p['status_offset'] for p in found['leaf_read_candidates']], list(range(0x100, 0x118, 4)))
        self.assertEqual(found['unverified_selectors'], [6])
        # Negative, unaligned and unbounded offsets no longer identify six leaves.
        for invalid in (-4, 3, 0x8004):
            changed = image[:]
            struct.pack_into('<i', changed, targets[0] + 2, invalid)
            self.assertEqual(scan(changed, [(0x100, 0x1000)]), [])

    def test_malformed_base_tables_targets_default_and_leaf_overlaps_are_refused(self):
        for row in self.rows:
            original = self.image(row)
            match = DISPATCH.search(original)
            candidate = scan(original, [(0x100, 0x1000)])[0]
            table, indices, default = [int(candidate[name], 16) for name in
                                      ('target_table_rva', 'selector_table_rva', 'default_target_rva')]
            corruptions = [
                (match.start('base'), '<i', 0),
                (match.start('targets'), '<i', table + 1),
                (match.start('targets'), '<i', -4),
                (match.start('selectors'), '<i', 0x1000),
                (table, '<I', 0), (table, '<I', table),
                (table, '<I', 0xffffffff), (table + 4, '<I', struct.unpack_from('<I', original, table)[0]),
                (indices, '<B', 255), (default, '<B', 0x90),
                (match.end('default') - 1, '<B', original[match.end('default') - 1] ^ 1),
                # Another target in the middle of a decoded leaf is unsafe.
                (table + 4, '<I', int(candidate['leaf_read_candidates'][0]['target_rva'], 16) + 1),
            ]
            for position, fmt, value in corruptions:
                image = original[:]
                struct.pack_into(fmt, image, position, value)
                self.assertEqual(scan(image, [(0x100, 0x1000)]), [], (row['game'], position, value))
            # Destroy enough leaf reads to lose the identifying shape.
            image = original[:]
            for path in candidate['leaf_read_candidates'][:4]:
                image[int(path['target_rva'], 16) + 2] = 3
            self.assertEqual(scan(image, [(0x100, 0x1000)]), [])

    def test_executable_bounds_and_non_executable_fragments(self):
        for row in self.rows:
            image = self.image(row)
            self.assertEqual(scan(image, []), [])
            self.assertEqual(scan(image, [(0, 0x100)]), [])
            self.assertEqual(scan(image, [(0x100, 0x100 + row['pin_size'] - 1)]), [])
            table = int(scan(image, [(0x100, 0x1000)])[0]['target_table_rva'], 16)
            self.assertEqual(scan(image, [(0x100, table), (table, 0x1000)]), [])
        for ranges in ([(-1, 2)], [(0, 4)], [(1, 1)], [(0, 2), (1, 3)]):
            with self.assertRaises(ValueError):
                scan(b'abc', ranges)

    def test_multiple_candidates_remain_explicit_and_bounded(self):
        image = self.image(self.rows[0])
        other = self.image(self.rows[1], 0x500)
        image[0x500:] = other[0x500:]
        self.assertEqual([r['getter_candidate_rva'] for r in scan(image, [(0x100, 0x1000)])],
                         ['0x100', '0x500'])
        image = bytearray(0x100 + 257 * 0x200)
        for index in range(257):
            location = 0x100 + index * 0x200
            # A bigger image allows all independently relocated tables.
            body = self.image(self.rows[0], 0x100)[0x100:0x100 + self.rows[0]['pin_size']]
            match = DISPATCH.match(body)
            body = bytearray(body)
            struct.pack_into('<i', body, match.start('base'), -location - match.end('base'))
            for field in ('selectors', 'targets'):
                struct.pack_into('<i', body, match.start(field),
                                 struct.unpack('<i', match.group(field))[0] + location - 0x100)
            table = struct.unpack('<i', match.group('targets'))[0] - 0x100
            indices = struct.unpack('<i', match.group('selectors'))[0] - 0x100
            for offset in range(table, indices, 4):
                struct.pack_into('<I', body, offset, struct.unpack_from('<I', body, offset)[0] + location - 0x100)
            image[location:location + len(body)] = body
        with self.assertRaisesRegex(ValueError, '256 candidates'):
            scan(image, [(0x100, len(image))])


if __name__ == '__main__':
    unittest.main()
