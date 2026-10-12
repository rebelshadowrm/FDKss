from publication_fixtures import private_evidence
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from discover_actor_cleanup_callers import capstone_decoder
from trace_engine_members import trace_members
from trace_engine_references import trace_references


class MemberTests(unittest.TestCase):
    @private_evidence
    def test_original_direct_global_request_setters_are_rmw(self):
        fixture=json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())['fixtures']
        for entry,at in [(0x4bd3a0,0x4bd54f),(0x2283d0,0x2284a4)]:
            fn=next(r['function'] for r in fixture if r['entry']==entry)
            end=int(fn['walk']['end_rva'],16); image=bytearray(0xe25f40)
            image[entry:end]=bytes.fromhex(fn['code_hex'])
            class Family:
                def contiguous(self,candidate):
                    return entry,end
            result=trace_references(bytes(image),[(entry,end)],Family(),[0xe25f24],capstone_decoder(bytes(image)))
            effects=[s for row in result['functions'] for s in row['sites'] if s['operation']=='read_write']
            self.assertEqual([(int(s['rva'],16),s['operand_bytes'],s['locked']) for s in effects],[(at,4,True)])
            self.assertFalse(result['runtime_profile_accepted'])

    def scan(self,code,members=(0xfba34,)):
        image=bytes(0x100)+bytes.fromhex(code)
        class Family:
            def contiguous(self,at):
                return 0x100,len(image)
        return trace_members(image,[(0x100,len(image))],Family(),members,capstone_decoder(image))

    @private_evidence
    def test_original_locked_request_clear_and_callback_reads(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())['fixtures']
        for entry,member,expected in [(0x227420,0xfba34,[(0x227572,True,True,True)]),
                                     (0x229db0,0xfba30,[(0x229dc8,True,False,False),(0x229dec,True,False,False)])]:
            fn=next(r['function'] for r in fixtures if r['entry']==entry)
            end=int(fn['walk']['end_rva'],16); image=bytearray(end)
            image[entry:end]=bytes.fromhex(fn['code_hex'])
            class Family:
                def contiguous(self,at):
                    return entry,end
            result=trace_members(bytes(image),[(entry,end)],Family(),[member],capstone_decoder(bytes(image)))
            sites=[s for row in result['functions'] for s in row['sites']]
            self.assertEqual([(int(s['rva'],16),s['read'],s['write'],s['locked']) for s in sites],expected)
            self.assertFalse(result['receiver_verified'])
            self.assertFalse(result['runtime_profile_accepted'])

    def test_immediate_and_skipped_instruction_candidates_refused(self):
        for code in ('b834ba0f00c3','eb08f083a334ba0f00fdc3'):
            result=self.scan(code)
            self.assertFalse(result['functions'])
            self.assertTrue(result['rejected'])

    def test_changed_member_and_incomplete_walk_refused(self):
        self.assertTrue(self.scan('8b8334ba0f00c3')['functions'])
        self.assertFalse(self.scan('8b8338ba0f00c3')['functions'])
        result=self.scan('8b8334ba0f00ffe0')
        self.assertFalse(result['functions'])
        self.assertEqual(result['rejected'][0]['reason'],'incomplete static walk')
        for members in ([],[True],[127],[0x100001],[0x100,0x100]):
            with self.assertRaises(ValueError):
                self.scan('c3',members)

    def test_memory_exchange_and_vector_widths_stay_explicit(self):
        result=self.scan('868130ba0f00660f7f8334ba0f00c3',[0xfba30,0xfba34])
        sites=result['functions'][0]['sites']
        self.assertTrue(sites[0]['atomic_memory_exchange'])
        self.assertFalse(sites[0]['locked'])
        self.assertEqual(sites[0]['operand_bytes'],1)
        self.assertEqual(sites[1]['operand_bytes'],16)
        self.assertTrue(sites[1]['write'])


if __name__=='__main__':
    unittest.main()
