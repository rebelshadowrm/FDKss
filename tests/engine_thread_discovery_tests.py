"""Thread predicates describe effects without approving an Actor owner."""
from publication_fixtures import private_evidence
import copy
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace as NS
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_engine_threads import discover, describe


class EngineThreadTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.originals = json.loads((Path(__file__).parent / 'fixtures/engine_thread_originals.json').read_text())

    def fixture(self, original):
        cls = copy.deepcopy(original['class_metadata'])
        table = next(t for t in cls['vtables'] if t['subobject_offset'] == t['constructor_displacement'] == 0)
        vt, api = int(table['rva'], 16), int(original['iat_rva'], 16)
        bodies = {int(at, 16): bytes.fromhex(body) for at, body in original['bodies'].items()}
        globals_ = [(r['global_rva'], 4) for r in original['expected']]
        globals_ += [(r['pointer_global_rva'], 8) for r in original['expected'] if 'pointer_global_rva' in r]
        image = bytearray(max(vt + len(bytes.fromhex(original['vtable_hex'])), api + 8,
                              *(at + len(body) for at, body in bodies.items()), *(at + size for at, size in globals_)) + 1)
        sections = []
        for at, body in bodies.items():
            image[at:at + len(body)] = body
            sections.append(NS(VirtualAddress=at, Misc_VirtualSize=len(body), Characteristics=0x60000000))
        image[vt:vt + len(bytes.fromhex(original['vtable_hex']))] = bytes.fromhex(original['vtable_hex'])
        for at, size in globals_ + [(vt, len(bytes.fromhex(original['vtable_hex']))), (api, 8)]:
            sections.append(NS(VirtualAddress=at, Misc_VirtualSize=size, Characteristics=0x40000000))
        imported = NS(dll=b'KERNEL32.dll', imports=[NS(name=b'GetCurrentThreadId', address=original['image_base'] + api)])
        pe = NS(FILE_HEADER=NS(Machine=0x8664), OPTIONAL_HEADER=NS(Magic=0x20b, ImageBase=original['image_base']),
                sections=sections, DIRECTORY_ENTRY_IMPORT=[imported], get_memory_mapped_image=lambda: bytes(image))
        return pe, image, cls

    def test_six_originals_slots_and_member_layouts(self):
        self.assertEqual(len(self.originals), 6)
        for original in self.originals:
            with self.subTest(game=original['game']):
                pe, _, cls = self.fixture(original)
                result = discover(pe, [cls])
                self.assertEqual(result['rejected'], [])
                rows = {r['slot']: r for r in result['candidates']}
                self.assertEqual(set(rows), {r['slot'] for r in original['expected']})
                for expected in original['expected']:
                    row = rows[expected['slot']]
                    self.assertEqual(row['thread_id_global_rva'], hex(expected['global_rva']))
                    if 'member_offset' in expected:
                        self.assertEqual(row['member_offset'], expected['member_offset'])
                        self.assertEqual(row['pointer_global_rva'], hex(expected['pointer_global_rva']))
                    else:
                        self.assertEqual(row['kind'], 'current_thread_matches_global')
                    self.assertFalse(row['runtime_profile_accepted'])
                    self.assertNotIn('owner', row)

    def test_changed_branches_returns_and_imports_refuse(self):
        original = self.originals[2]
        first = next(r for r in original['expected'] if 'member_offset' in r)
        for offset in [17, 18, 24, 31, 32, 37, 44, 45, 51]:
            pe, image, cls = self.fixture(original)
            image[first['handler_rva'] + offset] ^= 1
            self.assertNotIn(first['slot'], [r['slot'] for r in discover(pe, [cls])['candidates']], offset)
        for change in ['dll', 'name', 'iat', 'missing']:
            pe, _, cls = self.fixture(original)
            if change == 'dll': pe.DIRECTORY_ENTRY_IMPORT[0].dll = b'foreign.dll'
            elif change == 'name': pe.DIRECTORY_ENTRY_IMPORT[0].imports[0].name = b'GetCurrentProcessId'
            elif change == 'iat': pe.DIRECTORY_ENTRY_IMPORT[0].imports[0].address += 8
            else: pe.DIRECTORY_ENTRY_IMPORT = []
            self.assertEqual(discover(pe, [cls])['candidates'], [], change)

    def test_dependency_section_and_member_bounds(self):
        original = self.originals[2]
        expected = next(r for r in original['expected'] if 'member_offset' in r)
        for at, limit in [(expected['handler_rva'], 51), (expected['global_rva'], 3),
                          (expected['pointer_global_rva'], 7), (int(original['iat_rva'], 16), 7)]:
            pe, _, cls = self.fixture(original)
            # Shared globals may have duplicate section descriptions in the fixture.
            for section in pe.sections:
                if section.VirtualAddress == at: section.Misc_VirtualSize = limit
            self.assertNotIn(expected['slot'], [r['slot'] for r in discover(pe, [cls])['candidates']])
        for offset in [-1, 0, 0x100001]:
            pe, image, cls = self.fixture(original)
            struct.pack_into('<i', image, expected['handler_rva'] + 33, offset)
            self.assertNotIn(expected['slot'], [r['slot'] for r in discover(pe, [cls])['candidates']])

    def test_changed_pointer_ambiguity_and_architecture(self):
        pe, image, cls = self.fixture(self.originals[0])
        image[int(cls['vtables'][0]['rva'], 16)] ^= 1
        self.assertTrue(discover(pe, [cls])['rejected'])
        pe, _, cls = self.fixture(self.originals[0])
        self.assertEqual(discover(pe, [cls, copy.deepcopy(cls)])['candidates'], [])
        pe.FILE_HEADER.Machine = 0x14c
        with self.assertRaises(ValueError): discover(pe, [cls])


if __name__ == '__main__':
    unittest.main()
