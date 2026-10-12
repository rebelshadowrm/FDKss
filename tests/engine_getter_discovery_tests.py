"""Cross-build original getter evidence and refusal at the discovery boundary."""
from publication_fixtures import private_evidence
import copy
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_engine_getters import discover


class EngineGetterTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.originals = json.loads((Path(__file__).parent / 'fixtures/engine_getter_originals.json').read_text())

    def fixture(self, original):
        cls = copy.deepcopy(original['class_metadata'])
        table = cls['vtables'][0]; at = int(table['rva'], 16)
        bodies = {int(k, 16): bytes.fromhex(v) for k, v in original['bodies'].items()}
        strings = {int(k, 16): bytes.fromhex(v) for k, v in original['strings'].items()}
        image = bytearray(max(at + 8 * len(table['methods']), *(k + len(v) for k, v in strings.items()),
                              *(k + len(v) for k, v in bodies.items())) + 16)
        for target, body in bodies.items():
            image[target:target + len(body)] = body
        for target, body in strings.items():
            image[target:target + len(body)] = body
        image[at:at + len(bytes.fromhex(original['vtable_hex']))] = bytes.fromhex(original['vtable_hex'])
        section = lambda a, n, flags: SimpleNamespace(VirtualAddress=a, Misc_VirtualSize=n, Characteristics=flags)
        sections = [section(k, len(v), 0x60000000) for k, v in bodies.items()]
        sections += [section(k, len(v), 0x40000000) for k, v in strings.items()]
        sections += [section(at, 8 * len(table['methods']), 0x40000000)]
        pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                             OPTIONAL_HEADER=SimpleNamespace(ImageBase=original['image_base'], Magic=0x20b),
                             sections=sections, get_memory_mapped_image=lambda: bytes(image))
        return pe, image, cls

    def test_all_six_originals_discover_slots_without_game_or_layout_selection(self):
        self.assertEqual(len(self.originals), 6)
        for original in self.originals:
            with self.subTest(game=original['game']):
                pe, _, cls = self.fixture(original)
                report = discover(pe, [cls])
                self.assertEqual(report['rejected'], [])
                row = report['candidates'][0]; expected = original['expected']
                getters = {g['slot']: g for g in row['getters']}
                self.assertEqual(getters[expected['name_slot']]['value'], expected['name'])
                self.assertEqual(getters[expected['id_slot']]['value'], expected['id'])
                self.assertEqual(getters[expected['member_slot']]['object_offset'], expected['member_offset'])
                self.assertFalse(row['runtime_profile_accepted'])
                self.assertFalse(row['table_boundary_verified'])
                self.assertNotIn('transform', getters[expected['member_slot']])

    def test_changed_getter_tail_is_omitted_and_pointer_corruption_rejects_table(self):
        for original in self.originals:
            pe, image, cls = self.fixture(original)
            slot = original['expected']['member_slot']
            method = next(m for m in cls['vtables'][0]['methods'] if m['slot'] == slot)
            image[int(method['rva'], 16) + 4] = 0x90
            row = discover(pe, [cls])['candidates'][0]
            self.assertNotIn(slot, [g['slot'] for g in row['getters']])
            image[int(cls['vtables'][0]['rva'], 16) + slot * 8] ^= 1
            self.assertFalse(discover(pe, [cls])['candidates'])

    def test_ambiguous_classes_tables_slots_and_virtual_bases(self):
        pe, _, cls = self.fixture(self.originals[0])
        self.assertFalse(discover(pe, [cls, copy.deepcopy(cls)])['candidates'])
        for change in ('table', 'slot', 'out_of_bound', 'unaligned'):
            altered = copy.deepcopy(cls); table = altered['vtables'][0]
            if change == 'table': altered['vtables'].append(copy.deepcopy(table))
            elif change == 'slot': table['methods'].append(copy.deepcopy(table['methods'][0]))
            elif change == 'out_of_bound': table['methods'][0]['slot'] = 512
            else: table['rva'] = hex(int(table['rva'], 16) + 1)
            self.assertFalse(discover(pe, [altered])['candidates'], change)
        for key, value in (('member_displacement', 8), ('vbtable_displacement', 0)):
            altered = copy.deepcopy(cls)
            for base in altered['bases']:
                if base['class'] == '.?AVActor@fdk@@': base[key] = value
            self.assertFalse(discover(pe, [altered])['candidates'])

    def test_code_strings_and_pointer_words_must_fit_sections(self):
        original = self.originals[0]
        for kind in ('code', 'string', 'table'):
            pe, _, cls = self.fixture(original)
            expected = original['expected']
            if kind == 'code':
                target = int(next(m['rva'] for m in cls['vtables'][0]['methods'] if m['slot'] == expected['member_slot']), 16)
            elif kind == 'string': target = int(next(iter(original['strings'])), 16)
            else: target = int(cls['vtables'][0]['rva'], 16)
            section = next(s for s in pe.sections if s.VirtualAddress == target)
            section.Misc_VirtualSize = 4 if kind == 'code' else section.Misc_VirtualSize - 1
            report = discover(pe, [cls])
            if kind == 'table': self.assertFalse(report['candidates'])
            else:
                getters = report['candidates'][0]['getters']
                missing = expected['member_slot'] if kind == 'code' else expected['name_slot']
                self.assertNotIn(missing, [g['slot'] for g in getters])

    def test_signed_displacements_and_disp32_encoding(self):
        pe, image, cls = self.fixture(self.originals[0])
        slot = self.originals[0]['expected']['member_slot']
        at = int(next(m['rva'] for m in cls['vtables'][0]['methods'] if m['slot'] == slot), 16)
        for body, expected in ((bytes.fromhex('488d4180c3'), None),
                               (bytes.fromhex('488d4100c3'), None),
                               (bytes.fromhex('488d8100010000c3'), 256),
                               (bytes.fromhex('488d8100002000c3'), None)):
            image[at:at + 8] = body.ljust(8, b'\xcc')
            getters = discover(pe, [cls])['candidates'][0]['getters']
            match = next((g for g in getters if g['slot'] == slot), None)
            self.assertEqual(match['object_offset'] if match else None, expected)

    def test_architecture_and_invalid_image_ranges_refused(self):
        pe, _, cls = self.fixture(self.originals[0])
        pe.FILE_HEADER.Machine = 0xaa64
        with self.assertRaises(ValueError): discover(pe, [cls])
        pe.FILE_HEADER.Machine = 0x8664; pe.OPTIONAL_HEADER.Magic = 0x10b
        with self.assertRaises(ValueError): discover(pe, [cls])
        pe.OPTIONAL_HEADER.Magic = 0x20b; pe.sections[0].VirtualAddress = -1
        with self.assertRaises(ValueError): discover(pe, [cls])


if __name__ == '__main__':
    unittest.main()
