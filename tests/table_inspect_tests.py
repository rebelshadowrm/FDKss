import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from table_inspect import decode, load_schemas, load_game_schemas, width


def table(record, tail=b'', name=b'Test'):
    return b'#TBL' + struct.pack('<I', 1) + name.ljust(64, b'\0') + struct.pack('<4I', 0xabc, 88, len(record), 1) + record + tail


def schemas(fields):
    return {'Test': ({'version': 1, 'schema': fields}, {'sha256': 'fixture'})}


class TableTests(unittest.TestCase):
    def test_explicit_peer_profiles_and_validated_layouts(self):
        selected,pin=load_game_schemas('daybreak1')
        output=decode(table(bytes(248),name=b'ItemTableData'),selected,'daybreak1',pin)
        header=output['headers'][0]
        self.assertEqual(header['status'],'decoded')
        self.assertEqual(header['schema']['source'],'KuroTools')
        self.assertEqual(header['schema']['explicit_game'],'Kuro1')
        selected,pin=load_game_schemas('daybreak2')
        output=decode(table(bytes(152),name=b'NPCParam'),selected,'daybreak2',pin)
        self.assertEqual(output['headers'][0]['schema']['source'],'FDKss validated overlay')
        self.assertEqual(output['headers'][0]['records'][0]['fields']['start_flags'],[])
        for game,declared in [('sky1','Sora1'),('sky2','Sora2'),('ysxpn','Ys_X_Proud'),('kyoto','Kyoto')]:
            selected,pin=load_game_schemas(game)
            self.assertTrue(selected)
            self.assertTrue(all(identity.get('explicit_game',declared)==declared for candidates in selected.values() for _,identity in candidates))

    def test_schema_refusal_retains_all_records_raw(self):
        raw=table(struct.pack('<Q',8))
        output=decode(raw,schemas({'text':'ptr_str_utf8'}),'ysx',strict_schema=False)
        self.assertEqual(output['headers'][0]['status'],'raw_schema_decode_refused')
        self.assertNotIn('fields',output['headers'][0]['records'][0])
        with self.assertRaises(ValueError):decode(raw,schemas({'text':'ptr_str_utf8'}),'ysx')

    def test_scalars_strings_composites_and_raw_preservation(self):
        fields = {'id': 'u32', 'name': 'ptr_str_utf8', 'pairs': {'repeat': 2, 'type': {'id': 'u32', 'value': 'f32'}},
                  'values': {'repeat': 2, 'type': 's32'}}
        record = struct.pack('<IQIfIfii', 17, 124, 1, 1.5, 2, -2.5, -1, 9)
        raw = table(record, '名前'.encode() + b'\0')
        output = decode(raw, schemas(fields), 'ysx')
        row = output['headers'][0]['records'][0]
        self.assertEqual(row['fields'], {'id': 17, 'name': '名前', 'pairs': [{'id': 1, 'value': 1.5}, {'id': 2, 'value': -2.5}], 'values': [-1, 9]})
        self.assertEqual(row['raw_hex'], record.hex())
        self.assertEqual(output['input_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(output['trailing_data_hex'], raw[124:].hex())

    def test_arrays_and_null_values(self):
        raw = table(struct.pack('<QI', 100, 3), struct.pack('<3I', 2, 0, 0xffffffff))
        self.assertEqual(decode(raw, schemas({'array': 'arr_u32'}), 'ysx')['headers'][0]['records'][0]['fields']['array'], [2, 0, 0xffffffff])
        raw = table(struct.pack('<Q', 0))
        self.assertIsNone(decode(raw, schemas({'name': 'ptr_str_utf8'}), 'ysx')['headers'][0]['records'][0]['fields']['name'])

    def test_unknown_and_mismatched_schemas_stay_raw(self):
        raw = table(struct.pack('<I', 7), b'unknown\0')
        for selected, status in [({}, 'raw_unknown_schema'), (schemas({'wrong': 'u64'}), 'raw_schema_size_mismatch')]:
            header = decode(raw, selected, 'ysx')['headers'][0]
            self.assertEqual(header['status'], status)
            self.assertNotIn('fields', header['records'][0])
            self.assertEqual(header['records'][0]['raw_hex'], '07000000')

    def test_refuses_malformed_ranges_pointers_and_values(self):
        good = table(struct.pack('<Q', 96), b'ok\0')
        samples = [good[:7], good[:87], b'BAD!' + good[4:]]
        for index, val in [(4, 4097), (76, 87), (80, 0), (84, 100001)]:
            data = bytearray(good); struct.pack_into('<I', data, index, val); samples.append(bytes(data))
        samples += [table(struct.pack('<Q', ptr), b'x\0') for ptr in (8, 95, 0xffffffffffffffff)]
        samples += [table(struct.pack('<Q', 96), b'unterminated'), table(struct.pack('<Q', 96), b'\xff\0')]
        for raw in samples:
            with self.subTest(raw=raw[:12]), self.assertRaises(ValueError):
                decode(raw, schemas({'name': 'ptr_str_utf8'}), 'ysx')
        with self.assertRaises(ValueError): decode(table(struct.pack('<f', float('nan'))), schemas({'f': 'f32'}), 'ysx')
        with self.assertRaises(ValueError): decode(good, schemas({'name': 'ptr_str_utf8'}), 'unknown')
        with self.assertRaises(ValueError): width({'repeat': 1025, 'type': 'u32'})
        with self.assertRaises(ValueError): width('unknown')

    def test_array_budget_and_overlap(self):
        for pointer, count in [(0, 1), (100, 1000001), (100, 2)]:
            with self.assertRaises(ValueError): decode(table(struct.pack('<QI', pointer, count), b'\0'*4), schemas({'x': 'arr_u32'}), 'ysx')
        raw = b'#TBL'+struct.pack('<I',2)
        raw += b'One'.ljust(64,b'\0')+struct.pack('<4I',0,168,4,1)
        raw += b'Two'.ljust(64,b'\0')+struct.pack('<4I',0,168,4,1)+b'\0'*4
        with self.assertRaises(ValueError): decode(raw, {}, 'ysx')

    def test_pinned_schema_identity(self):
        root=Path(__file__).resolve().parents[1]/'tools/schemas/falcom'
        result, manifest = load_schemas(root)
        self.assertEqual(len(result),39)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);(path/'ys_X').mkdir()
            (path/'manifest.json').write_text(json.dumps(dict(manifest, common={}, profiles={'ysx': {'Test.json':'wrong'}})))
            (path/'ys_X/Test.json').write_text('{}')
            with self.assertRaises(ValueError):load_schemas(path)

    def test_game_profiles_references_and_manifest_match(self):
        root=Path(__file__).resolve().parents[1]/'tools/schemas/falcom'
        for game,count in [('daybreak1',113),('daybreak2',74)]:
            selected,manifest=load_schemas(root,game)
            self.assertEqual(len(selected),count)
            self.assertGreater(width(selected['ItemTableData'][0]['schema']['effects']),0)
            with self.assertRaisesRegex(ValueError,'match selected game'):
                decode(table(struct.pack('<I',1)),{},'ysx',manifest)

    def test_new_types_and_incompatible_schema_refusal(self):
        raw=table(struct.pack('<bhq2sQI',-1,-2,-3,b'xy',113,2),struct.pack('<2H',1,65535))
        fields={'a':'s8','b':'s16','c':'s64','opaque':'d2','array':'arr_u16'}
        row=decode(raw,schemas(fields),'daybreak1')['headers'][0]['records'][0]['fields']
        self.assertEqual(row,{'a':-1,'b':-2,'c':-3,'opaque':{'raw_hex':'7879'},'array':[1,65535]})
        manifest={'selected_game':'daybreak2','known_incompatible':{'daybreak2':{'Test':'Observed incompatible layout'}}}
        h=decode(raw,schemas(fields),'daybreak2',manifest)['headers'][0]
        self.assertEqual(h['status'],'raw_schema_unvalidated_layout')
        self.assertNotIn('fields',h['records'][0])

    def test_repeated_section_names_keep_directory_identity(self):
        raw=b'#TBL'+struct.pack('<I',2)
        raw+=b'Test'.ljust(64,b'\0')+struct.pack('<4I',0,168,4,1)
        raw+=b'Test'.ljust(64,b'\0')+struct.pack('<4I',0,172,4,1)+struct.pack('<2I',1,2)
        h=decode(raw,schemas({'id':'u32'}),'daybreak1')['headers']
        self.assertEqual([x['name_occurrence'] for x in h],[0,1])
        self.assertEqual([x['records'][0]['fields']['id'] for x in h],[1,2])

    def test_shared_pointer_text_expansion_is_bounded(self):
        raw = table(struct.pack('<2Q', 104, 104), b'text\0')
        with patch('table_inspect.MAX_TEXT', 7), self.assertRaisesRegex(ValueError, 'string budget'):
            decode(raw, schemas({'a': 'ptr_str_utf8', 'b': 'ptr_str_utf8'}), 'ysx')


if __name__ == '__main__': unittest.main()
