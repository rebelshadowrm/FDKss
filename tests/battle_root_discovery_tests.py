"""Battle root discovery must refuse ambiguous or malformed publications."""
from publication_fixtures import private_evidence
import hashlib
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_battle_roots import scan


class BattleRootTests(unittest.TestCase):
    @private_evidence
    def fixture(self, game):
        rows = json.loads((Path(__file__).parent / 'fixtures/battle_root_originals.json').read_text())
        row = next(r for r in rows if r['game'] == game)
        image = bytearray(row['image_size'])
        for fragment in row['fragments']:
            code = bytes.fromhex(fragment['hex'])
            self.assertEqual(hashlib.sha256(code).hexdigest(), fragment['sha256'])
            image[fragment['rva']:fragment['rva'] + len(code)] = code
        return image, row

    def run_scan(self, image, row):
        return scan(image, row['executable'], row['writable'], row['functions'], row['table'])

    def test_independent_original_fragments(self):
        for game in ('sky1', 'sky2'):
            image, row = self.fixture(game)
            candidates = self.run_scan(image, row)
            self.assertEqual(len(candidates), 1)
            self.assertEqual(int(candidates[0]['global_rva'], 16), row['root'])
            self.assertFalse(candidates[0]['runtime_adapter_approved'])

    def test_foreign_vtable_or_changed_list_initialization(self):
        image, row = self.fixture('sky2')
        row['table'] += 8
        self.assertEqual(self.run_scan(image, row), [])
        image, row = self.fixture('sky2')
        image[row['init'] + 3] ^= 1
        self.assertEqual(self.run_scan(image, row), [])

    def test_nonwritable_unaligned_or_truncated_publication(self):
        for change in ('section', 'alignment', 'truncated'):
            with self.subTest(change=change):
                image, row = self.fixture('sky1')
                if change == 'section':
                    row['writable'] = [[row['root'] + 8, row['root'] + 16]]
                elif change == 'alignment':
                    struct.pack_into('<i', image, row['publication'] + 16,
                                     row['root'] + 1 - row['publication'] - 20)
                else:
                    row['functions'][0][1] = row['publication'] + 19
                self.assertEqual(self.run_scan(image, row), [])

    def test_unregistered_constructor_and_wrong_call(self):
        image, row = self.fixture('sky2')
        row['functions'].pop()
        self.assertEqual(self.run_scan(image, row), [])
        image, row = self.fixture('sky2')
        image[row['publication'] + 4] ^= 1
        self.assertEqual(self.run_scan(image, row), [])

    def test_ambiguous_publication_refused(self):
        image, row = self.fixture('sky1')
        at = row['publication'] - 40
        code = bytearray(image[row['publication']:row['publication'] + 20])
        struct.pack_into('<i', code, 4, row['constructor'] - at - 8)
        struct.pack_into('<i', code, 16, row['root'] - at - 20)
        image[at:at + 20] = code
        self.assertEqual(self.run_scan(image, row), [])

    def test_invalid_section_bounds_refused(self):
        image, row = self.fixture('sky2')
        row['executable'][0][1] = len(image) + 1
        with self.assertRaises(ValueError):
            self.run_scan(image, row)

    @private_evidence
    def test_independent_collection_contract_fragments(self):
        rows = json.loads((Path(__file__).parent / 'fixtures/battle_collection_originals.json').read_text())
        self.assertEqual({r['game'] for r in rows}, {'sky1', 'sky2'})
        for row in rows:
            bodies = {}
            for fragment in row['fragments']:
                code = bytes.fromhex(fragment['code_hex'])
                self.assertEqual(hashlib.sha256(code).hexdigest(), fragment['body_sha256'])
                self.assertEqual(len(code), int(fragment['end_rva'], 16) - int(fragment['rva'], 16))
                bodies[fragment['name']] = code
            # Original insertion refuses append at 70 and increments the actual
            # list count. Constructor establishes both sides of the context link.
            self.assertIn(bytes.fromhex('83f846'), bodies['append'])
            self.assertIn(bytes.fromhex('ff8650240000'), bodies['append'])
            self.assertIn(bytes.fromhex('4889af28030000'), bodies['entry_constructor'])
            self.assertIn(bytes.fromhex('4889bd') + struct.pack('<I', row['context_backlink_offset']), bodies['entry_constructor'])
            self.assertIn(b'\xba' + struct.pack('<I', row['entry_allocation_size']), bodies['allocate'])
            context_load = '488b9928030000' if row['game'] == 'sky1' else '488b8928030000'
            self.assertIn(bytes.fromhex(context_load), bodies['entry_teardown'])
            self.assertIn(bytes.fromhex('33ed'), bodies['entry_teardown'])
            self.assertIn(bytes.fromhex('4889a8') + struct.pack('<I', row['context_backlink_offset']),
                          bodies['entry_teardown'])
            teardown = next(f for f in row['fragments'] if f['name'] == 'entry_teardown')
            clear = int(row['context_backlink_clear_rva'], 16) - int(teardown['rva'], 16)
            self.assertEqual(bodies['entry_teardown'][clear:clear+7],
                             bytes.fromhex('4889a8') + struct.pack('<I', row['context_backlink_offset']))


    @private_evidence
    def test_primary_owner_dispatch_route(self):
        from generate_battle_profiles import generate
        workspace=Path(__file__).resolve().parents[1]
        self.assertEqual((workspace/'src/windows/battle_profiles.inc').read_text(),generate(workspace))
        rows=json.loads((Path(__file__).parent/'fixtures/battle_collection_originals.json').read_text())
        roots={'sky1':0x8fba18,'sky2':0xc5d768}
        for row in rows:
            fragments={f['name']:f for f in row['fragments']};route=row['primary_update_route']
            def body(name):return bytes.fromhex(fragments[name]['code_hex'])
            def load(name,address,expected):
                code=body(name);at=int(address,16);offset=at-int(fragments[name]['rva'],16)
                self.assertEqual(code[offset:offset+3],bytes.fromhex('488b0d'))
                self.assertEqual(at+7+struct.unpack_from('<i',code,offset+3)[0],expected)
            def call(name,address,target):
                code=body(name);at=int(address,16);offset=at-int(fragments[name]['rva'],16)
                self.assertEqual(code[offset],0xe8)
                self.assertEqual(at+5+struct.unpack_from('<i',code,offset+1)[0],int(fragments[target]['rva'],16))
            load('gameplay_update',route['field_global_load'],int(route['field_global'],16))
            call('gameplay_update',route['field_update_call'],'field_update')
            call('field_update',route['field_dispatch_call'],'field_dispatch')
            load('field_active',route['battle_global_load'],roots[row['game']])
            call('field_active',route['primary_call'],'primary_dispatch')
            primary=body('primary_dispatch')
            self.assertIn(bytes.fromhex('486381a8000000'),primary)
            at=int(route['state_dispatch'],16)-int(fragments['primary_dispatch']['rva'],16)
            instruction='41ff54c510' if row['game']=='sky1' else 'ff54c510'
            self.assertEqual(primary[at:at+len(bytes.fromhex(instruction))],bytes.fromhex(instruction))
            entry=fragments['entry_update'];active=body('active_handler')
            at=int(route['entry_call'],16);offset=at-int(fragments['active_handler']['rva'],16)
            self.assertEqual(active[offset],0xe8)
            self.assertEqual(at+5+struct.unpack_from('<i',active,offset+1)[0],int(entry['rva'],16))

    @private_evidence
    def test_independent_update_route(self):
        rows = json.loads((Path(__file__).parent / 'fixtures/battle_collection_originals.json').read_text())
        roots = {'sky1': 0x8fba18, 'sky2': 0xc5d768}
        for row in rows:
            fragments = {f['name']: f for f in row['fragments']}
            callback = fragments['gameplay_update']
            code = bytes.fromhex(callback['code_hex'])
            start = int(callback['rva'], 16)
            load = int(row['update_route']['manager_global_load_rva'], 16)
            call = int(row['update_route']['manager_update_call_rva'], 16)
            self.assertEqual(code[load-start:load-start+3], bytes.fromhex('488b0d'))
            self.assertEqual(load+7+struct.unpack_from('<i', code, load-start+3)[0], roots[row['game']])
            self.assertEqual(code[call-start], 0xe8)
            self.assertEqual(call+5+struct.unpack_from('<i', code, call-start+1)[0],
                             int(fragments['manager_update']['rva'], 16))
            # Original update iterates the same pointer/count pair. App dispatch
            # reaches its current callback indirectly; this does not prove every
            # possible teardown is serialized, and must not upgrade that claim.
            manager = bytes.fromhex(fragments['manager_update']['code_hex'])
            self.assertIn(bytes.fromhex('4c8bb5482400008b8550240000'), manager)
            self.assertIn(bytes.fromhex('ffD2'), bytes.fromhex(fragments['application_update']['code_hex']))
            self.assertFalse(row['update_route']['update_thread_exclusion_verified'])


if __name__ == '__main__':
    unittest.main()
