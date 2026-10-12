"""Computed properties must not become fictitious native fields in SDK headers."""
import sys
import tempfile
import unittest
import re
import struct
from types import SimpleNamespace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from dump_sdk import verified_properties, verified_methods, verified_native_scalars, verified_native_selections, verified_native_square_roots, verified_native_status_getters, write_sdk


class LayoutSdkTests(unittest.TestCase):
    def test_native_status_getter_profile_and_table_pins(self):
        for game in ('sky1','sky2'):
            source=(ROOT/'src/windows/player_resources_profiles.inc').read_text()
            match=re.search(r'inline constexpr Profile\s+'+game+r'\{(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),\s*"([0-9a-f]+)"\};',source)
            entry=int(match[3],16);code=bytes.fromhex(match[4]);buffer=bytearray(code)
            pe=SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                sections=[SimpleNamespace(Characteristics=0x20000000,VirtualAddress=entry,Misc_VirtualSize=len(code))],
                get_data=lambda at,size:bytes(buffer[at-entry:at-entry+size]))
            metadata=verified_native_status_getters(pe,game)[0]
            self.assertEqual([x['selector'] for x in metadata['selected_paths']],[0,7,9,11,80,81,82,83,84])
            self.assertTrue(metadata['verified_signature'])
            self.assertNotIn('class_name',metadata)
            for at in (0,len(code)//2,len(code)-1):
                buffer[at]^=1
                with self.assertRaises(ValueError):verified_native_status_getters(pe,game)
                buffer[at]^=1
            pe.sections[0].Characteristics=0x40000000
            with self.assertRaises(ValueError):verified_native_status_getters(pe,game)

    def test_model_metadata_uses_full_name_and_64bit_counts(self):
        for game in ('sky1','sky2'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as directory:
                properties=verified_properties(ROOT/f'include/fdkss/{game}_properties.inc')
                fields={p['name']:p for p in properties if p['root']=='player_model'}
                self.assertEqual(fields['name']['storage'],'char64')
                self.assertEqual(fields['mesh_count']['offset'],0x258)
                self.assertEqual(fields['node_count']['storage'],'uint64_t')
                self.assertTrue(all(not p['writable'] for p in fields.values()))
                write_sdk(Path(directory),properties,[],game)
                header=(Path(directory)/f'{game}_layouts.hpp').read_text()
                self.assertIn('char name[64];',header)
                self.assertIn('offsetof(player_model_prefix, node_count) == 0x288',header)
                self.assertIn('---@field player_model FDK_player_model',(Path(directory)/'fdk.lua').read_text())
    def test_native_scalar_metadata_guards_complete_code_and_sign_mask(self):
        header = (ROOT / 'src/windows/native_math.h').read_text()
        for game, symbol, handler, mask in [('sky1', 'first', 0x3d7620, 0x817cf0), ('sky2', 'second', 0x4c4510, 0xb30f70)]:
            with self.subTest(game=game):
                code = bytearray.fromhex(re.search(symbol + r'_code\[\]="([0-9a-f]+)"', header)[1])
                sign_mask = bytearray(struct.pack('<4I', *([0x7fffffff] * 4)))
                pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                    sections=[SimpleNamespace(Characteristics=0x20000000, VirtualAddress=handler, Misc_VirtualSize=139)],
                    get_data=lambda at, size: bytes(code[:size]) if at == handler else bytes(sign_mask[:size]) if at == mask else b'')
                row = verified_native_scalars(pe, game)[0]
                self.assertTrue(row['verified_signature'])
                self.assertEqual((row['group'], row['command'], row['body_size']), (0, 39, 139))
                for at in [0, 138]:
                    code[at] ^= 1
                    with self.assertRaises(ValueError): verified_native_scalars(pe, game)
                    code[at] ^= 1
                sign_mask[12] ^= 1
                with self.assertRaises(ValueError): verified_native_scalars(pe, game)
                sign_mask[12] ^= 1
                pe.sections[0].Misc_VirtualSize = 138
                with self.assertRaises(ValueError): verified_native_scalars(pe, game)

    def test_native_selection_metadata_guards_both_full_bodies(self):
        pins = (ROOT / 'src/windows/native_select_profiles.inc').read_text()
        for game, entries in [('sky1', (0x3d7310, 0x3d7430)), ('sky2', (0x4c4200, 0x4c4320))]:
            codes = {entry: bytearray.fromhex(re.search(name + r'_code\[\]="([0-9a-f]+)"', pins)[1])
                     for name, entry in zip(('min', 'max'), entries)}
            pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                sections=[SimpleNamespace(Characteristics=0x20000000, VirtualAddress=entry, Misc_VirtualSize=280) for entry in entries],
                get_data=lambda at, size: bytes(codes.get(at, b'')[:size]))
            rows = verified_native_selections(pe, game)
            self.assertEqual([(row['binding'], row['command'], row['input_count']) for row in rows],
                             [('engine.native_min', 25, 2), ('engine.native_max', 26, 2)])
            for entry in entries:
                for offset in (0, 279):
                    codes[entry][offset] ^= 1
                    with self.assertRaises(ValueError): verified_native_selections(pe, game)
                    codes[entry][offset] ^= 1
            pe.sections[1].Misc_VirtualSize = 279
            with self.assertRaises(ValueError): verified_native_selections(pe, game)

    def test_native_sqrt_metadata_guards_full_body_and_nonnegative_scope(self):
        pins = (ROOT / 'src/windows/native_sqrt_profiles.inc').read_text()
        for game, entry in [('sky1', 0x3d71c0), ('sky2', 0x4c40b0)]:
            code = bytearray.fromhex(re.search('sqrt_' + game + r'_code\[\]="([0-9a-f]+)"', pins)[1])
            pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                sections=[SimpleNamespace(Characteristics=0x20000000, VirtualAddress=entry, Misc_VirtualSize=138)],
                get_data=lambda at, size: bytes(code[:size]) if at == entry else b'')
            row = verified_native_square_roots(pe, game)[0]
            self.assertEqual((row['binding'], row['command'], row['body_size']), ('engine.native_sqrt', 23, 138))
            self.assertIn('Negative domain', row['limitations'])
            for offset in (0, 137):
                code[offset] ^= 1
                with self.assertRaises(ValueError): verified_native_square_roots(pe, game)
                code[offset] ^= 1
            pe.sections[0].Misc_VirtualSize = 137
            with self.assertRaises(ValueError): verified_native_square_roots(pe, game)

    def test_raw_actor_state_widths_and_readonly_layout(self):
        for game in ('sky1', 'sky2'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as directory:
                properties = verified_properties(ROOT / f'include/fdkss/{game}_properties.inc')
                state = {p['name']: p for p in properties if p['name'].startswith('native_state_')}
                self.assertEqual(state['native_state_byte']['storage'], 'uint8_t')
                self.assertEqual(state['native_state_byte']['offset'], 0x16c)
                self.assertEqual(state['native_state_flags']['storage'], 'uint32_t')
                self.assertEqual(state['native_state_flags']['offset'], 0x170)
                self.assertFalse(any(p['writable'] for p in state.values()))
                write_sdk(Path(directory), properties, [], game)
                header = (Path(directory) / f'{game}_layouts.hpp').read_text()
                self.assertIn('std::uint8_t native_state_byte;', header)
                self.assertIn('std::byte reserved_16d[3];', header)
                self.assertIn('offsetof(actor_prefix, native_state_flags) == 0x170', header)

    def test_local_actor_scale_layout_is_readonly_and_distinct_from_world_matrix(self):
        for game in ('sky1', 'sky2'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as directory:
                properties = verified_properties(ROOT / f'include/fdkss/{game}_properties.inc')
                scale = [p for p in properties if p['name'] in ('scale_x', 'scale_y', 'scale_z')]
                self.assertEqual([(p['name'], p['offset'], p['storage'], p['writable']) for p in scale],
                                 [('scale_x', 0x138, 'float', False), ('scale_y', 0x13c, 'float', False), ('scale_z', 0x140, 'float', False)])
                write_sdk(Path(directory), properties, [], game)
                header = (Path(directory) / f'{game}_layouts.hpp').read_text()
                lua = (Path(directory) / 'fdk.lua').read_text()
                for name, offset in [('scale_x', '0x138'), ('scale_y', '0x13c'), ('scale_z', '0x140')]:
                    self.assertIn(f'offsetof(actor_prefix, {name}) == {offset}', header)
                    self.assertIn(f'---@field {name} number', lua)

    def test_pointer_getter_documents_copied_lua_results(self):
        for game in ('sky1', 'sky2'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as directory:
                methods = verified_methods(ROOT / f'include/fdkss/{game}_methods.inc')
                transform = next(m for m in methods if m['name'] == 'get_transform')
                self.assertEqual(transform['cpp_return_type'], 'const float*')
                self.assertEqual(transform['lua_result_count'], 16)
                self.assertEqual(transform['slot'], 6)
                write_sdk(Path(directory), [], methods, game)
                header = (Path(directory) / f'{game}_layouts.hpp').read_text()
                self.assertIn('using get_transform_fn = const float* (*)(void* actor);', header)
                self.assertIn('method:"get_transform"|"type_name"|"type_id"|"type_flags"):', (Path(directory) / 'fdk.lua').read_text())
                self.assertEqual(len(methods), 69)
                self.assertEqual(len({(m['class'], m['name']) for m in methods}), 69)
                self.assertIn('using set_native_state_byte_fn = void (*)(void* actor, std::uint8_t value);', header)

    def test_resource_properties_are_derived_without_native_field_layout(self):
        for game in ('sky1','sky2'):
            with tempfile.TemporaryDirectory() as directory:
                properties=verified_properties(ROOT/f'include/fdkss/{game}_properties.inc')
                resources=[p for p in properties if p['storage']=='computed_resource']
                self.assertEqual(len(resources),6)
                self.assertEqual({p['name'] for p in resources if p['writable']},{'hp','ep','cp'})
                self.assertTrue(all(p['computed'] and p['offset'] is None for p in resources))
                self.assertEqual({p['source_offset'] for p in resources},{0x1d38 if game=='sky1' else 0x1d90})
                write_sdk(Path(directory),properties,[],game)
                header=(Path(directory)/f'{game}_layouts.hpp').read_text()
                prefixes='\n'.join(re.findall(r'struct \w+_prefix \{.*?\};',header,re.S))
                self.assertNotIn('max_hp',prefixes)
                self.assertNotIn('std::uint32_t hp;',header)
                self.assertNotIn('status_prefix',header)
                self.assertIn('struct BattleSnapshotRecord { std::uint32_t status_word, level, hp, max_hp, ep, max_ep, cp, max_cp; };',header)
                self.assertIn('static_assert(sizeof(BattleSnapshotRecord) == 32);',header)
                self.assertIn('---@field battle_snapshot fun():FDKBattleSnapshot|nil',(Path(directory)/'fdk.lua').read_text())
                self.assertIn('---@field hp integer|nil Writable. Computed through current character status link.',(Path(directory)/'fdk.lua').read_text())

    def test_computed_yaw_is_api_metadata_only(self):
        for game in ('sky1', 'sky2'):
            with self.subTest(game=game), tempfile.TemporaryDirectory() as directory:
                properties = verified_properties(ROOT / f'include/fdkss/{game}_properties.inc')
                yaw = next(p for p in properties if p['name'] == 'yaw_radians')
                self.assertIsNone(yaw['offset'])
                self.assertEqual(yaw['source_offset'], 0x108)
                self.assertTrue(yaw['computed'] and yaw['writable'])
                write_sdk(Path(directory), properties, [], game)
                header = (Path(directory) / f'{game}_layouts.hpp').read_text()
                self.assertNotIn('yaw_radians', header)
                self.assertIn('offsetof(actor_prefix, rotation_x) == 0x108', header)
                self.assertIn('yaw_radians number|nil Writable. Computed;', (Path(directory) / 'fdk.lua').read_text())


if __name__ == '__main__':
    unittest.main()
