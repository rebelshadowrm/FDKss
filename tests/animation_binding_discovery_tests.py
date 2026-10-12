from publication_fixtures import private_evidence
import json,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from discover_animation_bindings import DISPATCH,scan

class AnimationBindingDiscoveryTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.fixtures=json.loads((Path(__file__).parent/'fixtures/animation_binding_fragments.json').read_text())
    def relocate(self,row,address=0x100,image=None):
        body=bytes.fromhex(row['code_hex'])
        if image is None:image=bytearray(address+len(body)+0x100)
        image[address:address+len(body)]=body
        delta=address-row['original_start']
        match=DISPATCH.search(image,address,address+32)
        self.assertIsNotNone(match)
        # The original LEA addresses image base; relocation deliberately keeps
        # that anchor at zero while rebasing the table and its absolute RVAs.
        struct.pack_into('<i',image,match.start('anchor'),-match.end('anchor'))
        table=row['original_table']+delta
        struct.pack_into('<i',image,match.start('table'),table)
        targets=struct.unpack_from('<12I',image,table)
        struct.pack_into('<12I',image,table,*(t+delta for t in targets))
        return image
    def test_relocated_original_fragments_match_independent_pins(self):
        for fixture in self.fixtures:
            for address in (0x100,0x430):
                image=self.relocate(fixture,address);rows=scan(image,[(address,len(image))])
                self.assertEqual(len(rows),1)
                row=rows[0]
                self.assertEqual([b['destination_offset'] for b in row['branches']],fixture['expected_offsets'])
                self.assertEqual(row['node_pointer_offset'],fixture['expected_pointer'])
                self.assertEqual(row['storage'],fixture['expected_storage'])
                self.assertFalse(row['callable_signature_verified'])
                self.assertEqual(row['dispatch_fragment_rva'],hex(address))
    def test_changed_layout_is_recovered_without_original_addresses(self):
        for fixture in self.fixtures:
            image=self.relocate(fixture);row=scan(image,[(0,len(image))])[0]
            bases=(0x180,0x200,0x300) if fixture['expected_pointer'] is None else (0x60,0x100,0x180)
            for group,base in enumerate(bases):
                for component in range(3):
                    at=int(row['branches'][group*3+component]['branch_rva'],16)
                    if image[at:at+3]==b'\x48\x83\xef':image[at+3]=(-base)&255
                    else:struct.pack_into('<i',image,at+3,base+component*4)
            if fixture['expected_pointer'] is not None:image[0x103]=0x48
            result=scan(image,[(0,len(image))]);self.assertEqual(len(result),1)
            self.assertEqual([b['destination_offset'] for b in result[0]['branches']],
                [base+i*4 for base in bases for i in range(3)]+list(bases))
            self.assertEqual(result[0]['node_pointer_offset'],None if fixture['expected_pointer'] is None else 0x48)
    def test_nonzero_image_anchor_and_separate_readable_table(self):
        for fixture in self.fixtures:
            image=self.relocate(fixture);match=DISPATCH.search(image)
            table=fixture['original_table']+0x100-fixture['original_start']
            targets=struct.unpack_from('<12I',image,table);anchor=0x80
            struct.pack_into('<i',image,match.start('anchor'),anchor-match.end('anchor'))
            struct.pack_into('<i',image,match.start('table'),table-anchor)
            struct.pack_into('<12I',image,table,*(target-anchor for target in targets))
            result=scan(image,[(0,table)],[(table,table+48)])
            self.assertEqual(len(result),1)
            self.assertEqual(result[0]['anchor_rva'],hex(anchor))
            self.assertEqual(result[0]['table_rva'],hex(table))
            self.assertEqual([b['destination_offset'] for b in result[0]['branches']],fixture['expected_offsets'])
    def test_corrupt_branches_tables_layouts_and_bounds_are_refused(self):
        for fixture in self.fixtures:
            image=self.relocate(fixture);row=scan(image,[(0,len(image))])[0]
            match=DISPATCH.search(image);table=int(row['table_rva'],16)
            branch=int(row['branches'][1]['branch_rva'],16)
            for position in (match.start()+2,match.start('anchor'),match.start('table'),branch,branch+7,branch+14):
                changed=image[:];changed[position]^=1
                self.assertEqual(scan(changed,[(0,len(changed))]),[],position)
            for position,value in ((match.start('default'),0x7fffffff),(branch+3,-4),(branch+3,0x8004),(branch+3,row['branches'][0]['destination_offset']),
                                  (table+9*4,int(row['branches'][1]['branch_rva'],16))):
                changed=image[:];struct.pack_into('<i',changed,position,value)
                self.assertEqual(scan(changed,[(0,len(changed))]),[],position)
            self.assertEqual(scan(image,[(0,match.end()-1)]),[])
            self.assertEqual(scan(image,[(0,len(image))],[(0,table+47)]),[])
            self.assertEqual(scan(image,[(0,branch),(branch+15,len(image))]),[])
            self.assertEqual(scan(image,[(0,len(image))],[]),[])
        with self.assertRaises(ValueError):scan(b'abc',[(0,4)])
        with self.assertRaises(ValueError):scan(b'abc',[(0,3)],[(-1,3)])
    def test_pointer_alignment_and_alias_table_entries_are_required(self):
        fixture=self.fixtures[1];image=self.relocate(fixture)
        for pointer in (0,1,0x7f,0x80):
            changed=image[:];changed[0x103]=pointer
            self.assertEqual(scan(changed,[(0,len(changed))]),[])
        row=scan(image,[(0,len(image))])[0];table=int(row['table_rva'],16)
        changed=image[:];struct.pack_into('<I',changed,table+10*4,int(row['branches'][0]['branch_rva'],16))
        self.assertEqual(scan(changed,[(0,len(changed))]),[])
    def test_multiple_candidates_preserved_and_bounded(self):
        image=bytearray(0x2000)
        self.relocate(self.fixtures[0],0x100,image);self.relocate(self.fixtures[1],0x1000,image)
        self.assertEqual(len(scan(image,[(0,len(image))])),2)
        image=bytearray(0x600*258)
        for i in range(257):self.relocate(self.fixtures[0],0x100+0x600*i,image)
        with self.assertRaisesRegex(ValueError,'256'):scan(image,[(0,len(image))])

if __name__=='__main__':unittest.main()
