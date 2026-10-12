from publication_fixtures import private_evidence
import hashlib,json,re,struct,sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from dump_sdk import verified_native_resource_setters
class OriginalResourceHookContracts(unittest.TestCase):
    @private_evidence
    def test_sdk_keeps_call_and_observation_scopes_distinct(self):
        originals=json.loads((ROOT/'tests/fixtures/resource_hook_originals.json').read_text(encoding='utf-8'))
        for row in originals:
            body=bytes.fromhex(row['hex'])
            class Original:
                def get_data(self,rva,size):return body[rva-row['entry']:rva-row['entry']+size]
            metadata=verified_native_resource_setters(Original(),row['game'])[0]
            self.assertEqual([v['selector'] for v in metadata['selected_paths']],[7,9,11])
            self.assertEqual(metadata['observation']['selectors'],list(range(7,13)))
            self.assertTrue(metadata['observation']['observer_only'])
            class Changed:
                def get_data(self,rva,size):return bytes([body[0]^1])+body[1:]
            with self.assertRaises(ValueError):verified_native_resource_setters(Changed(),row['game'])
    @private_evidence
    def test_independent_bodies_dispatch_leaves_and_callers(self):
        profiles=(ROOT/'src/windows/resource_setter_profiles.inc').read_text(encoding='utf-8')
        originals=json.loads((ROOT/'tests/fixtures/resource_hook_originals.json').read_text(encoding='utf-8'))
        self.assertEqual({row['game'] for row in originals},{'sky1','sky2'})
        for row in originals:
            with self.subTest(game=row['game']):
                match=re.search(r'inline constexpr SetterProfile\s+'+row['game']+r'_setter\{(0x[0-9a-f]+),\s*((?:"[0-9a-f]+"\s*)+)\};',profiles)
                self.assertIsNotNone(match)
                body=bytes.fromhex(''.join(re.findall(r'"([0-9a-f]+)"',match[2])))
                self.assertEqual(int(match[1],16),row['entry'])
                self.assertEqual(body.hex(),row['hex'])
                self.assertEqual(hashlib.sha256(body).hexdigest(),row['sha256'])
                self.assertEqual(len(body),row['end']-row['entry'])
                self.assertEqual(body[:3],bytes.fromhex('83fa28' if row['game']=='sky1' else '83fa55'))
                self.assertEqual(body[3:5],b'\x0f\x87')
                self.assertEqual(body[9:14],bytes.fromhex('4889742410'))
                self.assertEqual([leaf['selector'] for leaf in row['selected_leaves']],list(range(7,13)))
                for leaf in row['selected_leaves']:
                    selector=leaf['selector'];index=selector
                    if row['dispatch_indices'] is not None:index=body[row['dispatch_indices']-row['entry']+selector]
                    target=struct.unpack_from('<I',body,row['dispatch_table']-row['entry']+index*4)[0]
                    self.assertEqual(target,leaf['rva'])
                    code=bytes.fromhex(leaf['hex']);offset=target-row['entry']
                    self.assertEqual(body[offset:offset+len(code)],code)
                    self.assertEqual(hashlib.sha256(code).hexdigest(),leaf['sha256'])
                    self.assertEqual(code[-1],0xc3)
                    self.assertEqual(leaf['operation'],'set' if selector%2 else 'add')
                    self.assertEqual(leaf['status_offset'],12+((selector-7)//2)*8)
                self.assertTrue({7,9,11}.issubset({v['selector'] for v in row['caller_windows']}))
                for caller in row['caller_windows']:
                    code=bytes.fromhex(caller['hex']);offset=caller['site']-caller['rva']
                    self.assertEqual(hashlib.sha256(code).hexdigest(),caller['sha256'])
                    self.assertEqual(code[offset],0xe8)
                    self.assertEqual(caller['site']+5+struct.unpack_from('<i',code,offset+1)[0],row['entry'])
if __name__=='__main__':unittest.main()
