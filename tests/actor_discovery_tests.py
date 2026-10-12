"""Offline adaptation candidates must reject ambiguous RTTI and changed getter bodies."""
from publication_fixtures import private_evidence
import copy
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_actor_types import discover, discover_constructors, discover_byte_setters, discover_base_destructors, CONSTRUCTOR_TEMPLATE
from discover_actor_types import describe_cleanup, discover_cleanup_helpers


class EngineCleanupEffectTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.originals = json.loads((Path(__file__).parent / 'fixtures/engine_cleanup_originals.json').read_text())

    def fixture(self, original):
        image = bytearray(0x1000)
        body = bytes.fromhex(original['code_hex'])
        image[0x300:0x300 + len(body)] = body
        return image, [(0x100, 0x900)]

    def test_six_independent_full_originals_preserve_layout_differences(self):
        for original in self.originals:
            image, executable = self.fixture(original)
            row = describe_cleanup(image, executable, 0x300)
            expected = original['expected']
            for key in ('body_sha256', 'body_size', 'linked_object_offset',
                        'vector_pointer_offset', 'vector_count_offset', 'cleared_receiver_offsets'):
                self.assertEqual(row[key], expected[key], (original['game'], key))
            self.assertFalse(row['receiver_type_verified'])
            self.assertFalse(row['lifetime_invalidation_verified'])
            self.assertNotIn('cleared_actor_offsets', row)
            found = discover_cleanup_helpers(image, executable)
            self.assertEqual(found['candidates'], [row])
            self.assertFalse(found['runtime_profile_accepted'])

    def test_every_branch_delete_flag_and_full_tail_byte_is_guarded(self):
        mutable = {i for at in (28, 50, 57, 64, 123, 130) for i in range(at, at + 4)}
        for original in self.originals:
            image, executable = self.fixture(original)
            for at in range(len(bytes.fromhex(original['code_hex']))):
                if at in mutable:
                    continue
                changed = image[:]; changed[0x300 + at] ^= 1
                self.assertIsNone(describe_cleanup(changed, executable, 0x300), (original['game'], at))
            self.assertIsNone(describe_cleanup(image, [(0x100, 0x390)], 0x300))

    def test_layout_consistency_signed_bounds_and_no_owner_inference(self):
        image, executable = self.fixture(self.originals[0])
        for at, value in ((50, 0x160), (130, 0x178), (28, -8), (57, 0x100008),
                          (123, 0x161), (123, 0x178), (64, 0)):
            changed = image[:]; struct.pack_into('<i', changed, 0x300 + at, value)
            self.assertIsNone(describe_cleanup(changed, executable, 0x300), (at, value))
        changed = image[:]
        for at, value in ((28, 0x300), (50, 0x300), (57, 0x320), (64, 0x328),
                          (123, 0x400), (130, 0x328)):
            struct.pack_into('<i', changed, 0x300 + at, value)
        row = describe_cleanup(changed, executable, 0x300)
        self.assertEqual(row['cleared_receiver_offsets'], [0x300, 0x400, 0x328])
        self.assertFalse(row['receiver_type_verified'])
        with self.assertRaises(ValueError):
            discover_cleanup_helpers(image, [(0x100, 0x1001)])


class ActorDestructorDiscoveryTests(unittest.TestCase):
    @private_evidence
    def setUp(self):
        self.originals=json.loads((Path(__file__).parent/'fixtures/actor_destructor_originals.json').read_text())
    def fixture(self,row,entry=0x300,base=0x180000000):
        image=bytearray(0x2000);body=bytearray.fromhex(row['code_hex']);call_index=0
        for fixup in row['fixups']:
            offset=fixup['offset'];kind=fixup['kind']
            if kind=='vtable':target=0x1000
            elif kind=='global':target=0x1100
            else:target=0x100+call_index*0x40;call_index+=1
            struct.pack_into('<i',body,offset,target-entry-offset-4)
        image[entry:entry+len(body)]=body
        struct.pack_into('<Q',image,0x1000,base+entry)
        classes=[dict(name='.?AVActor@fdk@@',vtables=[dict(rva='0x1000',subobject_offset=0,
            constructor_displacement=0,methods=[dict(slot=0,rva=hex(entry))])])]
        return image,classes
    def scan(self,image,classes,base=0x180000000,executable=None,readable=None):
        return discover_base_destructors(image,[(0x100,0x900)] if executable is None else executable,
            classes,base,[(0x1000,0x1200)] if readable is None else readable)
    def test_relocated_full_original_bodies_and_image_bases(self):
        for original in self.originals:
            for entry,base in ((0x300,0x180000000),(0x500,0x140000000)):
                image,classes=self.fixture(original,entry,base);found=self.scan(image,classes,base)['candidates']
                self.assertEqual(len(found),1);row=found[0]
                self.assertEqual(row['encoding'],original['encoding'])
                self.assertEqual(row['body_size'],len(bytes.fromhex(original['code_hex'])))
                self.assertEqual(row['handler_rva'],hex(entry));self.assertEqual(row['vtable_rva'],'0x1000')
                self.assertEqual(row['deletion_flag_mask'],1);self.assertFalse(row['callable_signature_verified'])
                self.assertEqual(len(row['dependencies']),len(original['fixups']))
    def test_every_fixed_instruction_byte_is_guarded(self):
        for original in self.originals:
            image,classes=self.fixture(original)
            mutable={i for f in original['fixups'] for i in range(f['offset'],f['offset']+4)}
            for offset in range(len(bytes.fromhex(original['code_hex']))):
                if offset in mutable:continue
                changed=image[:];changed[0x300+offset]^=1
                self.assertEqual(self.scan(changed,classes)['candidates'],[],(original['game'],offset))
    def test_external_calls_and_vtable_targets_must_be_valid(self):
        for original in self.originals:
            image,classes=self.fixture(original)
            for fixup in original['fixups']:
                offset=fixup['offset']
                targets=(0x300,0x999,0x1000) if fixup['kind']=='call' else (0x1008,) if fixup['kind']=='vtable' else (0x1101,0x2000)
                for target in targets:
                    changed=image[:];struct.pack_into('<i',changed,0x300+offset,target-0x300-offset-4)
                    self.assertEqual(self.scan(changed,classes)['candidates'],[],(original['game'],fixup['kind'],target))
    def test_metadata_identity_and_ambiguity_are_refused(self):
        for original in self.originals:
            image,classes=self.fixture(original)
            changed=image[:];struct.pack_into('<Q',changed,0x1000,0x180000308)
            self.assertFalse(self.scan(changed,classes)['candidates'])
            for mutation in ('class','table','slot','name','subobject','displacement'):
                changed=copy.deepcopy(classes);table=changed[0]['vtables'][0]
                if mutation=='class':changed.append(copy.deepcopy(changed[0]))
                elif mutation=='table':changed[0]['vtables'].append(copy.deepcopy(table))
                elif mutation=='slot':table['methods'].append(copy.deepcopy(table['methods'][0]))
                elif mutation=='name':changed[0]['name']='.?AVUnknown@@'
                elif mutation=='subobject':table['subobject_offset']=8
                else:table['constructor_displacement']=8
                self.assertFalse(self.scan(image,changed)['candidates'],mutation)
            classes[0]['vtables'][0]['methods']=[]
            self.assertFalse(self.scan(image,classes)['candidates'])
    def test_complete_executable_and_readable_bounds(self):
        for original in self.originals:
            image,classes=self.fixture(original);size=len(bytes.fromhex(original['code_hex']))
            self.assertFalse(self.scan(image,classes,executable=[(0x100,0x300+size-1)])['candidates'])
            self.assertFalse(self.scan(image,classes,readable=[(0x1000,0x1007)])['candidates'])
            self.assertFalse(self.scan(image,classes,readable=[])['candidates'])
            if original['game']=='sky2':
                self.assertFalse(self.scan(image,classes,readable=[(0x1000,0x1008),(0x1100,0x1107)])['candidates'])
            with self.assertRaises(ValueError):self.scan(image,classes,executable=[(-1,0x900)])
            with self.assertRaises(ValueError):self.scan(image,classes,readable=[(0,len(image)+1)])
    @private_evidence
    def test_cleanup_dependency_matches_independent_full_body_and_offsets(self):
        pins=json.loads((Path(__file__).parent/'fixtures/actor_cleanup_profiles.json').read_text())
        for original in self.originals:
            image,classes=self.fixture(original);body=bytes.fromhex(pins[original['game']]['code_hex'])
            image[0x100:0x100+len(body)]=body
            found=self.scan(image,classes)['candidates'][0]['cleanup_dependency']
            self.assertIsNotNone(found)
            self.assertEqual(found['handler_rva'],'0x100');self.assertEqual(found['code_hex'],body.hex())
            self.assertEqual(found['body_sha256'],pins[original['game']]['body_sha256'])
            self.assertEqual((found['linked_object_offset'],found['vector_pointer_offset'],found['vector_count_offset']),(0x158,0x178,0x180))
            self.assertEqual(found['cleared_actor_offsets'],[0x158,0x160,0x180])
            self.assertEqual((found['virtual_resource_delete_slot'],found['resource_delete_flags']),(0,1))
            self.assertFalse(found['lifetime_invalidation_verified'])
    @private_evidence
    def test_changed_or_truncated_cleanup_is_kept_unverified(self):
        pins=json.loads((Path(__file__).parent/'fixtures/actor_cleanup_profiles.json').read_text())
        for original in self.originals:
            image,classes=self.fixture(original);body=bytes.fromhex(pins[original['game']]['code_hex'])
            image[0x100:0x100+len(body)]=body
            for offset in range(len(body)):
                changed=image[:];changed[0x100+offset]^=1
                found=self.scan(changed,classes)['candidates']
                self.assertEqual(len(found),1)
                self.assertIsNone(found[0]['cleanup_dependency'])
            found=self.scan(image,classes,executable=[(0x100,0x100+len(body)-1),(0x1c0,0x900)])['candidates']
            self.assertEqual(len(found),1);self.assertIsNone(found[0]['cleanup_dependency'])


class ActorDiscoveryTests(unittest.TestCase):
    def test_byte_setters_recover_slots_offsets_and_short_encoding(self):
        for base,rva,slot,offset in [(0x140000000,0x180,17,0x16c),(0x180000000,0x240,18,0x238),
                                     (0x170000000,0x200,9,0x7f)]:
            self.setUp();self.base=base
            self.image[rva:rva+7]=b'\x88\x91'+struct.pack('<i',offset)+b'\xc3'
            if offset==0x7f:self.image[rva:rva+4]=b'\x88\x51\x7f\xc3'
            self.table['methods']=[dict(slot=slot,rva=hex(rva))]
            struct.pack_into('<Q',self.image,0x800+slot*8,base+rva)
            report=discover_byte_setters(bytes(self.image),[(0x100,0x400)],[self.entry],base)
            row=report['candidates'][0]
            self.assertEqual((row['slot'],row['object_offset'],row['handler_rva']),(slot,offset,hex(rva)))
            self.assertEqual(row['body_size'],4 if offset==0x7f else 7)
            self.assertEqual(row['storage'],'uint8_t')

    def test_byte_setters_refuse_changed_code_bounds_and_metadata(self):
        self.image[0x180:0x187]=b'\x88\x91\x6c\x01\x00\x00\xc3'
        self.table['methods']=[dict(slot=17,rva='0x180')]
        struct.pack_into('<Q',self.image,0x800+17*8,self.base+0x180)
        report=lambda:discover_byte_setters(bytes(self.image),[(0x100,0x400)],[self.entry],self.base)
        original=self.image[:]
        for at in (0x180,0x181,0x186,0x800+17*8):
            self.image=original[:];self.image[at]^=1
            self.assertFalse(report()['candidates'])
        for offset in (0,-1,0x100001):
            self.image=original[:];struct.pack_into('<i',self.image,0x182,offset)
            self.assertFalse(report()['candidates'])
        self.image=original[:]
        self.assertFalse(discover_byte_setters(bytes(self.image),[(0x100,0x186)],[self.entry],self.base)['candidates'])
        self.table['methods'].append(copy.deepcopy(self.table['methods'][0]))
        self.assertFalse(report()['candidates']);self.assertTrue(report()['rejected'])
        self.table['methods'].pop()
        self.entry['vtables'].append(copy.deepcopy(self.table))
        self.assertFalse(report()['candidates'])
        self.entry['vtables'].pop()
        self.entry['bases'][0]['vbtable_displacement']=0
        self.assertFalse(report()['candidates'])

    def test_byte_setter_class_entries_keep_shared_handlers_and_cap(self):
        self.image[0x180:0x187]=b'\x88\x91\x6c\x01\x00\x00\xc3'
        self.table['methods']=[dict(slot=17,rva='0x180')]
        struct.pack_into('<Q',self.image,0x800+17*8,self.base+0x180)
        second=copy.deepcopy(self.entry);second['name']='.?AVOther@@'
        report=discover_byte_setters(bytes(self.image),[(0x100,0x400)],[self.entry,second],self.base)
        self.assertEqual(len(report['candidates']),2)
        with self.assertRaises(ValueError):
            discover_byte_setters(bytes(self.image),[(0x100,0x400)],[self.entry]*4097,self.base)
    def test_constructor_fixups_guards_and_raw_call_candidates(self):
        self.setUp()
        base = dict(name='.?AVActor@fdk@@', vtables=[dict(rva='0x900', subobject_offset=0, constructor_displacement=0)])
        body = bytearray(CONSTRUCTOR_TEMPLATE)
        struct.pack_into('<i', body, 13, 0x900 - (0x200 + 17))
        for operand, target in [(34, 0x110), (59, 0x140)]:
            struct.pack_into('<i', body, operand, target - (0x200 + operand + 4))
        self.image[0x200:0x200 + len(body)] = body
        self.image[0x170:0x175] = b'\xe8' + struct.pack('<i', 0x200 - 0x175)
        report = lambda: discover_constructors(bytes(self.image), [(0x100, 0x400)], [base])
        row = report()['candidates'][0]
        self.assertEqual(row['handler_rva'], '0x200')
        self.assertEqual(row['called_handler_rvas'], ['0x110', '0x140'])
        self.assertEqual(row['direct_call_candidates'], ['0x170'])
        self.assertEqual(row['body_size'], 139)
        original = self.image[:]
        for at in (0x200, 0x200 + 13, 0x200 + 138, 0x200 + 96):
            with self.subTest(at=at):
                self.image = original[:]; self.image[at] ^= 1
                self.assertFalse(report()['candidates'])
        self.image = original[:]
        struct.pack_into('<i', self.image, 0x200 + 34, 0x800 - (0x200 + 38))
        self.assertFalse(report()['candidates'])
        self.image = original[:]
        alternate = bytearray(body)
        struct.pack_into('<i', alternate, 13, 0x900 - (0x290 + 17))
        for operand, target in [(34, 0x110), (59, 0x140)]:
            struct.pack_into('<i', alternate, operand, target - (0x290 + operand + 4))
        self.image[0x290:0x290 + len(alternate)] = alternate
        self.assertTrue(report()['ambiguous'])
        self.assertEqual(len(report()['candidates']), 2)
        self.image = original[:]
        self.assertFalse(discover_constructors(bytes(self.image), [(0x100, 0x28a)], [base])['candidates'])
        base['vtables'].append(copy.deepcopy(base['vtables'][0]))
        self.assertFalse(report()['candidates'])

    def setUp(self):
        self.image = bytearray(4096)
        self.base = 0x140000000
        self.pe = SimpleNamespace(FILE_HEADER=SimpleNamespace(Machine=0x8664),
                                  OPTIONAL_HEADER=SimpleNamespace(ImageBase=self.base),
                                  sections=[SimpleNamespace(VirtualAddress=0x100, Misc_VirtualSize=0x300, Characteristics=0x20000000)],
                                  get_memory_mapped_image=lambda: bytes(self.image))
        self.image[0x110:0x118] = b'\x48\x8d\x05' + struct.pack('<i', 0x500-0x117) + b'\xc3'
        self.image[0x120:0x126] = b'\xb8' + struct.pack('<I', 0xabcdef12) + b'\xc3'
        self.image[0x130:0x133] = b'\x33\xc0\xc3'
        self.image[0x140:0x145] = b'\x48\x8d\x41\x58\xc3'
        self.image[0x500:0x506] = b'Chara\0'
        self.entry = dict(name='.?AVCandidate@@', bases=[], vtables=[])
        self.entry['bases'] = [{'class': '.?AVActor@fdk@@', 'member_displacement': 0, 'vbtable_displacement': -1}]
        self.table = dict(rva='0x800', subobject_offset=0, constructor_displacement=0,
                          methods=[dict(slot=slot, rva=hex(rva)) for slot,rva in [(1,0x110),(2,0x120),(3,0x130),(6,0x140)]])
        for method in self.table['methods']:
            struct.pack_into('<Q', self.image, 0x800+method['slot']*8, self.base+int(method['rva'],16))
        self.entry['vtables'] = [self.table]

    def report(self):
        return discover(self.pe, [self.entry])

    def test_complete_candidate_and_unsigned_values(self):
        report = self.report()
        self.assertEqual(len(report['candidates']), 1)
        self.assertFalse(report['rejected'])
        row = report['candidates'][0]
        self.assertEqual(row['type_name']['value'], 'Chara')
        self.assertEqual(row['type_id']['value'], 0xabcdef12)
        self.assertEqual(row['type_flags']['value'], 0)
        self.assertEqual(row['transform']['object_offset'], 88)
        self.assertIn('candidates only', report['limitations'][0])

    def test_changed_bodies_pointer_and_unterminated_name(self):
        original = self.image[:]
        for at in (0x110,0x125,0x130,0x143,0x800+2*8):
            with self.subTest(offset=at):
                self.image = original[:]
                self.image[at] ^= 1
                self.assertFalse(self.report()['candidates'])
        self.image = original[:]
        self.image[0x500:0x600] = b'x'*256
        self.assertFalse(self.report()['candidates'])
        self.image = original[:]
        self.image[0x500] = 0xff
        self.assertFalse(self.report()['candidates'])

    def test_ambiguous_primary_missing_slot_and_virtual_actor_base(self):
        self.entry['vtables'].append(copy.deepcopy(self.table))
        self.assertFalse(self.report()['candidates'])
        self.entry['vtables'] = [self.table]
        self.table['methods'].pop()
        self.assertFalse(self.report()['candidates'])
        self.entry['bases'][0]['vbtable_displacement'] = 0
        self.assertFalse(self.report()['candidates'])
        self.assertFalse(self.report()['rejected'])

    def test_executable_and_architecture_bounds(self):
        self.pe.sections[0].Characteristics = 0
        self.assertFalse(self.report()['candidates'])
        self.pe.FILE_HEADER.Machine = 0xaa64
        with self.assertRaises(ValueError):
            self.report()


if __name__ == '__main__':
    unittest.main()
