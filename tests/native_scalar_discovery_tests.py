"""Relocate original scalar bodies and refuse inconsistent layouts/dependencies."""
from publication_fixtures import private_evidence
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_native_scalars import PATTERNS, discover, scan


class NativeScalarDiscoveryTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.rows = json.loads((Path(__file__).parent / 'fixtures/native_scalar_originals.json').read_text())

    def image(self, row, at=0x100, fields=None):
        image = bytearray(0x1000)
        body = bytearray.fromhex(row['code_hex'])
        pattern = next(p for p in PATTERNS if p['operation_candidate'] == row['operation_candidate'])
        if fields:
            for name, occurrences in pattern['fields'].items():
                for offset, width in occurrences:
                    body[offset:offset + width] = fields[name].to_bytes(width, 'little', signed=True)
        if pattern['external_operand']:
            role, offset = pattern['external_operand']
            target = 0x800 if role == 'sign_mask' else 0x900
            struct.pack_into('<i', body, offset, target - at - offset - 4)
            if role == 'sign_mask':
                image[target:target + 16] = struct.pack('<4I', *([0x7fffffff] * 4))
        image[at:at + len(body)] = body
        return image, pattern

    def find(self, image):
        return scan(image, [(0x100, 0x700), (0x900, 0x901)], [(0x800, 0x810)])

    def test_all_independent_originals_relocate_with_complete_bodies(self):
        for row in self.rows:
            for at in (0x100, 0x400):
                image, _ = self.image(row, at)
                found = self.find(image)
                self.assertEqual(len(found), 1)
                self.assertEqual(found[0]['handler_candidate_rva'], hex(at))
                self.assertEqual(found[0]['operation_candidate'], row['operation_candidate'])
                self.assertEqual(found[0]['body_hex'], image[at:at + len(bytes.fromhex(row['code_hex']))].hex())
                self.assertEqual(found[0]['storage_offset_candidates'], dict(
                    argument_count=0x70, stack_used_bytes=0x64, stack_pointer=0x58,
                    return_enabled=0x80, result_register=0x14))

    def test_new_offsets_and_inconsistent_repeated_fields(self):
        fields = dict(argument_count=0x74, stack_used_bytes=0x68, stack_pointer=0x60,
                      return_enabled=0x90, result_register=0x18)
        for row in self.rows:
            image, pattern = self.image(row, fields=fields)
            self.assertEqual(self.find(image)[0]['storage_offset_candidates'], fields)
            for occurrences in pattern['fields'].values():
                if len(occurrences) > 1:
                    changed = bytearray(image)
                    changed[0x100 + occurrences[-1][0]] += 4
                    self.assertEqual(self.find(changed), [])

    def test_invalid_negative_unaligned_overlapping_or_excessive_offsets(self):
        base = dict(argument_count=0x70, stack_used_bytes=0x64, stack_pointer=0x58,
                    return_enabled=0x80, result_register=0x14)
        for row in self.rows:
            for name, value in [('stack_pointer', -8), ('result_register', 0),
                                ('argument_count', 0x71), ('stack_pointer', 0x54),
                                ('result_register', 0x64), ('return_enabled', 0x59),
                                ('return_enabled', 0x8001)]:
                fields = {**base, name: value}
                image, _ = self.image(row, fields=fields)
                self.assertEqual(self.find(image), [], (row['operation_candidate'], name, value))

    def test_complete_fixed_control_flow_and_instructions_are_required(self):
        for row in self.rows:
            image, pattern = self.image(row)
            masked = {i for occurrences in pattern['fields'].values() for at, width in occurrences
                      for i in range(at, at + width)}
            if pattern['external_operand']:
                masked.update(range(pattern['external_operand'][1], pattern['external_operand'][1] + 4))
            for at in range(len(bytes.fromhex(row['code_hex']))):
                if at in masked:
                    continue
                changed = bytearray(image); changed[0x100 + at] ^= 1
                self.assertEqual(self.find(changed), [], (row['operation_candidate'], hex(at)))

    def test_external_dependency_sections_bounds_alignment_and_contents(self):
        for row in self.rows:
            image, pattern = self.image(row)
            if not pattern['external_operand']:
                continue
            role, offset = pattern['external_operand']
            for target in (-1, 0x1000, 0x110, 0x801, 0x80c):
                changed = bytearray(image)
                struct.pack_into('<i', changed, 0x100 + offset, target - 0x100 - offset - 4)
                self.assertEqual(self.find(changed), [])
            if role == 'sign_mask':
                image[0x800] ^= 1
                self.assertEqual(self.find(image), [])
            else:
                self.assertEqual(scan(image, [(0x100, 0x700)], [(0x800, 0x810)]), [])

    def test_truncated_nonexecutable_invalid_sections_and_candidate_cap(self):
        for row in self.rows:
            image, _ = self.image(row)
            length = len(bytes.fromhex(row['code_hex']))
            self.assertEqual(scan(image, [(0x100, 0x100 + length - 1), (0x900, 0x901)], [(0x800, 0x810)]), [])
            self.assertEqual(scan(image, [(0x400, 0x700)], [(0x100, 0x1000)]), [])
        for sections in [[(-1, 10)], [(0, 0x1001)], [(0, 100), (99, 110)]]:
            with self.assertRaises(ValueError):
                scan(bytearray(0x1000), sections, [])
            with self.assertRaises(ValueError):
                scan(bytearray(0x1000), [], sections)
        body = bytes.fromhex(next(r for r in self.rows if r['operation_candidate'] == 'minimum')['code_hex'])
        image = b'\x90'.join([body] * 257)
        with self.assertRaisesRegex(ValueError, '256'):
            scan(image, [(0, len(image))], [])

    def test_unary_crt_targets_do_not_supply_names_or_callable_acceptance(self):
        row=next(r for r in self.rows if r['operation_candidate']=='unary_float_call')
        image,pattern=self.image(row);second,_=self.image(row,at=0x400)
        image[0x400:0x400+118]=second[0x400:0x400+118]
        at=pattern['external_operand'][1]
        struct.pack_into('<i',image,0x400+at,0x901-0x400-at-4)
        found=scan(image,[(0x100,0x700),(0x900,0x902)],[(0x800,0x810)])
        self.assertEqual(len(found),2)
        self.assertEqual({r['operation_candidate'] for r in found},{'unary_float_call'})
        self.assertEqual({r['external_dependency']['target_rva'] for r in found},{'0x900','0x901'})
        self.assertTrue(all(r['external_dependency']['executed_or_callable_verified'] is False for r in found))
        self.assertTrue(all('verified_signature' not in r for r in found))
    def test_duplicate_candidates_remain_explicitly_ambiguous(self):
        row = next(r for r in self.rows if r['operation_candidate'] == 'maximum')
        image, _ = self.image(row)
        body = bytes.fromhex(row['code_hex']);image[0x400:0x400 + len(body)] = body
        pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
            OPTIONAL_HEADER=SimpleNamespace(SizeOfImage=len(image)),
            sections=[SimpleNamespace(VirtualAddress=0x100, Misc_VirtualSize=0x600,
                                      SizeOfRawData=0x600, Characteristics=0x60000000)],
            get_memory_mapped_image=lambda: image)
        report = discover(pe)
        self.assertEqual(report['ambiguous_operations'], ['maximum'])
        self.assertEqual([r['handler_candidate_rva'] for r in report['candidates']], ['0x100', '0x400'])
        pe.FILE_HEADER.Machine = 0x14c
        with self.assertRaises(ValueError):
            discover(pe)


if __name__ == '__main__':
    unittest.main()
