from publication_fixtures import private_evidence
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_actor_cleanup_callers import capstone_decoder, walk_function
from trace_engine_calls import inspect_function, analyze_barrier
from trace_engine_ownership import analyze_aliases
from trace_engine_references import reference, trace_references


def original_function(code):
    image = bytes(0x100) + bytes.fromhex(code)
    class Families:
        def contiguous(self, at):
            return 0x100, len(image)
    return inspect_function(image, [(0x100,len(image))], Families(), 0x100, capstone_decoder(image))


class OwnershipTests(unittest.TestCase):
    @private_evidence
    def test_original_ys_global_callback_forwarding_and_group_completion(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        from freeze_engine_evidence import freeze_tail_thunks
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        for label, worker, callback, decrement, signal, implementation in [
                ('pn', 0x7a4c30, 0x7a4d83, 0x7a4e4e, 0x7a4efb, 0x767960),
                ('ysx', 0x6f2e40, 0x6f2f83, 0x6f304f, 0x6f30fb, 0x6b11e0)]:
            fixture = json.loads((Path(__file__).parent/'fixtures'/f'engine_global_jobs_{label}.json').read_text())
            thunk = fixture['tail_thunks'][0]
            pointer = fixture['pointer_words'][0]
            self.assertEqual(int.from_bytes(bytes.fromhex(pointer['code_hex']), 'little') -
                             fixture['preferred_image_base'], thunk['entry'])
            image = bytes(thunk['entry']) + bytes.fromhex(thunk['code_hex'])
            rebuilt = freeze_tail_thunks(image, [(implementation,len(image))], [thunk['entry']])[0]
            self.assertEqual(rebuilt, thunk)
            self.assertEqual(thunk['tail_target_rva'], implementation)
            fn = next(row['function'] for row in fixture['fixtures'] if row['entry'] == worker)
            decoded = {int(row['rva'],16): next(cs.disasm(bytes.fromhex(row['code_hex']),int(row['rva'],16)))
                       for row in fn['instructions']}
            self.assertEqual((decoded[callback].mnemonic, decoded[callback].op_str),
                             ('call','qword ptr [rax + 0x10]'))
            self.assertEqual((decoded[decrement].mnemonic, decoded[decrement].op_str),
                             ('dec','dword ptr [rax]'))
            ins = decoded[signal]
            self.assertEqual(fixture['imports'][hex(ins.address+ins.size+ins.operands[0].mem.disp)], 'SetEvent')
            order = analyze_barrier(fn, callback, decrement)
            self.assertTrue(order['barrier_reachable'])
            self.assertFalse(order['barrier_on_every_static_path'])
            self.assertFalse(order['runtime_synchronization_verified'])

    @private_evidence
    def test_original_ys_global_submission_wait_has_zero_and_absent_id_bypasses(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        for label, consumer, submit in [
                ('pn',0x766fc0,0x767344),
                ('ysx',0x6b0910,0x6b0c52)]:
            fixture = json.loads((Path(__file__).parent/'fixtures'/f'engine_global_jobs_{label}.json').read_text())
            fn = next(row['function'] for row in fixture['fixtures'] if row['entry'] == consumer)
            # Locate the original import rather than borrowing PN's inlined
            # wait offset for the different original-Ys compiler layout.
            waits = []
            for row in fn['instructions']:
                ins = next(cs.disasm(bytes.fromhex(row['code_hex']),int(row['rva'],16)))
                if ins.mnemonic == 'call' and ins.op_str.startswith('qword ptr [rip'):
                    if fixture['imports'].get(hex(ins.address+ins.size+ins.operands[0].mem.disp)) == 'WaitForSingleObject':
                        waits.append(ins.address)
            self.assertEqual(len(waits),1)
            order = analyze_barrier(fn,submit,waits[0])
            self.assertTrue(order['barrier_reachable'])
            self.assertFalse(order['barrier_on_every_static_path'])
            self.assertFalse(order['runtime_synchronization_verified'])

    @private_evidence
    def test_original_horizon_manager_extent_and_worker_mask_are_not_older_layout(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        from trace_engine_members import trace_members
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_global_jobs_horizon.json').read_text())
        functions = {row['entry']:row['function'] for row in fixture['fixtures']}
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        destructor = functions[0x679b70]
        row = next(r for r in destructor['instructions'] if r['rva']=='0x679c53')
        ins = next(cs.disasm(bytes.fromhex(row['code_hex']),0x679c53))
        self.assertEqual((ins.mnemonic,ins.op_str),('mov','edx, 0x190'))
        fn = functions[0x67a120]
        entry,end = 0x67a120,int(fn['walk']['end_rva'],16)
        image = bytes(entry)+bytes.fromhex(fn['code_hex'])
        class Family:
            def contiguous(self,at):
                return entry,end
        result = trace_members(image,[(entry,end)],Family(),[0xd0],capstone_decoder(image))
        sites = [site for row in result['functions'] for site in row['sites']]
        self.assertEqual({site['operand_bytes'] for site in sites},{4})
        self.assertTrue(any(site['write'] and site['locked'] for site in sites))
        borrowed = trace_members(image,[(entry,end)],Family(),[0x90],capstone_decoder(image))
        self.assertFalse(borrowed['functions'])
        self.assertFalse(result['receiver_verified'])
        self.assertFalse(result['runtime_profile_accepted'])

    @private_evidence
    def test_original_horizon_condition_wait_and_broadcast_follow_callback_bookkeeping(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_global_jobs_horizon.json').read_text())
        functions = {row['entry']:row['function'] for row in fixture['fixtures']}
        names = {}
        for word in fixture['pointer_words'][1:]:
            ins = next(cs.disasm(bytes.fromhex(word['code_hex']),word['rva']))
            self.assertEqual(ins.mnemonic,'jmp')
            self.assertEqual(ins.op_str.split()[0],'qword')
            names[word['rva']] = fixture['imports'][hex(ins.address+ins.size+ins.operands[0].mem.disp)]
        self.assertEqual(names[0xa9aec7],'_Cnd_wait')
        self.assertEqual(names[0xa9aecd],'_Cnd_broadcast')
        worker = functions[0x67a850]
        for at, expected in [(0x67a973,('call','qword ptr [rax + 0x10]')),
                             (0x67aa3e,('dec','dword ptr [rax]')),
                             (0x67ab66,('call','0xa9aecd'))]:
            row = next(r for r in worker['instructions'] if int(r['rva'],16)==at)
            ins = next(cs.disasm(bytes.fromhex(row['code_hex']),at))
            self.assertEqual((ins.mnemonic,ins.op_str),expected)
        order = analyze_barrier(worker,0x67a973,0x67aa3e)
        self.assertTrue(order['barrier_reachable'])
        self.assertFalse(order['barrier_on_every_static_path'])
        wait = functions[0x67aca0]
        order = analyze_barrier(wait,0x67aca0,0x67ad30)
        self.assertTrue(order['barrier_reachable'])
        self.assertFalse(order['barrier_on_every_static_path'])
        self.assertFalse(order['runtime_synchronization_verified'])

    @private_evidence
    def test_original_ys_global_fdk_job_manager_is_separate_from_embedded_scene_queue(self):
        from trace_engine_members import trace_members
        for name in ['engine_scheduler_originals.json', 'engine_scheduler_contrasts.json']:
            fixture = json.loads((Path(__file__).parent/'fixtures'/name).read_text())
            manager = fixture['global_fdk_job_manager']
            word = manager['vtable_word']
            self.assertEqual(int.from_bytes(bytes.fromhex(word['code_hex']), 'little') -
                             word['preferred_image_base'], manager['destructor_entry'])
            self.assertFalse(manager['embedded_scene_queue_same_type_verified'])
            fn = next(row['function'] for row in fixture['fixtures'] if row['entry'] == manager['reset_entry'])
            entry, end = manager['reset_entry'], int(fn['walk']['end_rva'], 16)
            image = bytearray(end)
            image[entry:end] = bytes.fromhex(fn['code_hex'])
            class Family:
                def contiguous(self, at):
                    return entry, end
            result = trace_members(bytes(image), [(entry, end)], Family(), [0x90], capstone_decoder(bytes(image)))
            sites = [site for row in result['functions'] for site in row['sites']]
            self.assertTrue(sites)
            self.assertEqual({site['operand_bytes'] for site in sites}, {4})
            self.assertTrue(any(site['write'] and site['locked'] for site in sites))
            self.assertFalse(result['receiver_verified'])
            self.assertFalse(result['runtime_profile_accepted'])

    @private_evidence
    def test_original_daybreak_worker_completion_follows_callback_on_normal_route(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_contrasts.json').read_text())
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        for row, callback, decrement, remove_count, signal in zip(fixture['contrasts'],
                [0x3f7ea1, 0x57d7f3], [0x3f7f6c, 0x57d8bf],
                [0x3f7fdb, 0x57d92b], [0x3f801b, 0x57d96b]):
            fn = row['fixtures'][4]['function']
            decoded = {int(original['rva'], 16): next(cs.disasm(bytes.fromhex(original['code_hex']),
                        int(original['rva'], 16))) for original in fn['instructions']}
            self.assertEqual((decoded[callback].mnemonic, decoded[callback].op_str),
                             ('call', 'qword ptr [rax + 0x10]'))
            self.assertEqual((decoded[decrement].mnemonic, decoded[decrement].op_str),
                             ('dec', 'dword ptr [rax]'))
            self.assertEqual((decoded[remove_count].mnemonic, decoded[remove_count].op_str),
                             ('dec', 'qword ptr [rbx + 0x60]'))
            ins = decoded[signal]
            self.assertEqual(row['imports'].get(hex(ins.address + ins.size + ins.operands[0].mem.disp)),
                             'SetEvent')
            ordered = analyze_barrier(fn, callback, decrement)
            self.assertTrue(ordered['barrier_reachable'])
            # Mutex failures stop before bookkeeping; visiting the callback is
            # not universal completion or proof that its captured receiver lives.
            self.assertFalse(ordered['barrier_on_every_static_path'])
            self.assertFalse(ordered['runtime_synchronization_verified'])

    @private_evidence
    def test_original_daybreak_completion_wait_is_conditional_and_differs_from_ys_poll(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL
        from capstone.x86_const import X86_OP_MEM
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_contrasts.json').read_text())
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        for row, wait_at in zip(fixture['contrasts'], [0x3f8490, 0x57ddf2]):
            fn = row['fixtures'][3]['function']
            names = []
            for original in fn['instructions']:
                ins = next(cs.disasm(bytes.fromhex(original['code_hex']), int(original['rva'], 16)))
                if ins.group(CS_GRP_CALL) and ins.operands[0].type == X86_OP_MEM:
                    operand = ins.operands[0].mem
                    if ins.reg_name(operand.base) == 'rip':
                        names.append(row['imports'].get(hex(ins.address + ins.size + operand.disp)))
            self.assertIn('WaitForSingleObject', names)
            # Zero ID / absent pending ID exit without waiting; after a wake the
            # loop rechecks the collection. Handle ownership and producer/record
            # removal semantics still require proof beyond this wait call.
            entry = int(fn['walk']['function_rva'], 16)
            result = analyze_barrier(fn, entry, wait_at)
            self.assertTrue(result['barrier_reachable'])
            self.assertFalse(result['barrier_on_every_static_path'])
            self.assertFalse(result['runtime_synchronization_verified'])

    @private_evidence
    def test_original_daybreak_job_managers_do_not_borrow_ys_embedded_queue_layout(self):
        from trace_engine_members import trace_members
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_contrasts.json').read_text())
        pn = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())
        pn_reset = next(row['function'] for row in pn['fixtures'] if row['entry'] == 0x2be510)
        bodies = [(pn_reset, False)] + [(row['fixtures'][1]['function'], True) for row in fixture['contrasts']]
        for fn, daybreak in bodies:
            entry, end = int(fn['walk']['function_rva'], 16), int(fn['walk']['end_rva'], 16)
            image = bytearray(end)
            image[entry:end] = bytes.fromhex(fn['code_hex'])
            class Family:
                def contiguous(self, at):
                    return entry, end
            result = trace_members(bytes(image), [(entry, end)], Family(), [0x90], capstone_decoder(bytes(image)))
            sites = [site for row in result['functions'] for site in row['sites']]
            self.assertTrue(sites)
            self.assertEqual({site['operand_bytes'] for site in sites}, {4 if daybreak else 8})
            self.assertEqual(any(site['locked'] and site['write'] for site in sites), daybreak)
            self.assertFalse(result['receiver_verified'])
            self.assertFalse(result['runtime_profile_accepted'])

    @private_evidence
    def test_original_ys_scene_job_keeps_offsets_and_shutdown_order_distinct(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        from trace_engine_members import trace_members
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_contrasts.json').read_text())
        functions = {row['entry']: row['function'] for row in fixture['fixtures']}
        callback = fixture['callback_word']
        self.assertEqual(int.from_bytes(bytes.fromhex(callback['code_hex']), 'little') -
                         callback['preferred_image_base'], 0x1d3e10)
        fn = functions[0x1d3e10]
        entry, end = 0x1d3e10, int(fn['walk']['end_rva'], 16)
        image = bytearray(end)
        image[entry:end] = bytes.fromhex(fn['code_hex'])
        class Family:
            def contiguous(self, at):
                return entry, end
        original = trace_members(bytes(image), [(entry, end)], Family(), [0xfb368], capstone_decoder(bytes(image)))
        self.assertEqual([site['rva'] for row in original['functions'] for site in row['sites']],
                         ['0x1d3e28', '0x1d3e4c'])
        borrowed = trace_members(bytes(image), [(entry, end)], Family(), [0xfba30], capstone_decoder(bytes(image)))
        self.assertFalse(borrowed['functions'])
        self.assertFalse(original['receiver_verified'])
        self.assertFalse(original['runtime_profile_accepted'])
        contrasts = json.loads((Path(__file__).parent/'fixtures/engine_ownership_contrasts.json').read_text())
        ys_shutdown = next(row['function'] for row in contrasts['fixtures'] if row['name'] == 'ysx-0x1cd9c0')
        pn = json.loads((Path(__file__).parent/'fixtures/engine_ownership_originals.json').read_text())
        pn_shutdown = next(row['function'] for row in pn['fixtures']
                           if row['function']['walk']['function_rva'] == '0x224260')
        # Compare decoded original instruction order only. Conditional/null,
        # exceptional and self-join paths still preclude universal lifetime proof.
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        for body, root, reset_at, reset, scene_clear, reset_before in [
                (pn_shutdown, 0xd2a4f0, 0x2242d1, 0x2be510, 0x224593, True),
                (ys_shutdown, 0xc0ffb0, 0x1cdc56, 0x271050, 0x1cdc4f, False)]:
            row = next(row for row in body['instructions'] if int(row['rva'], 16) == reset_at)
            instruction = next(cs.disasm(bytes.fromhex(row['code_hex']), reset_at))
            self.assertEqual((instruction.mnemonic, instruction.op_str), ('call', hex(reset)))
            self.assertEqual(reset_at < scene_clear, reset_before)
            self.assertFalse(analyze_aliases(body, [(root + 0x480, 8)], root)['lifetime_verified'])
        conditional_join = analyze_barrier(functions[0x271050], 0x27107d, 0x2710b9)
        self.assertFalse(conditional_join['barrier_on_every_static_path'])
        self.assertFalse(conditional_join['runtime_synchronization_verified'])

    @private_evidence
    def test_original_state_eight_captures_scene_job_and_submission_has_two_modes(self):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        cs = Cs(CS_ARCH_X86, CS_MODE_64)
        cs.detail = True
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())
        functions = {row['entry']: row['function'] for row in fixture['fixtures']}
        ownership = json.loads((Path(__file__).parent/'fixtures/engine_ownership_originals.json').read_text())
        startup = next(row['function'] for row in ownership['fixtures']
                       if row['function']['walk']['function_rva'] == '0x21f980')

        def instruction(fn, at):
            row = next(row for row in fn['instructions'] if int(row['rva'], 16) == at)
            decoded = list(cs.disasm(bytes.fromhex(row['code_hex']), at))
            self.assertEqual(len(decoded), 1)
            self.assertEqual(decoded[0].size, row['size'])
            return decoded[0].mnemonic, decoded[0].op_str

        # Independently preserved startup captures the CApp receiver and the
        # member-function address for state 8; a raw pointer-word scan misses it.
        self.assertEqual(instruction(startup, 0x223a53), ('lea', 'rbx, [rsi + 0x110]'))
        captured = next(row for row in startup['instructions'] if row['rva'] == '0x223c27')
        lea = next(cs.disasm(bytes.fromhex(captured['code_hex']), 0x223c27))
        self.assertEqual(lea.mnemonic, 'lea')
        self.assertEqual(lea.address + lea.size + lea.operands[1].mem.disp, 0x227420)
        self.assertEqual(instruction(startup, 0x223c3c), ('mov', 'qword ptr [rsp + 0x58], rsi'))
        self.assertEqual(instruction(startup, 0x223c50), ('mov', 'edx, 8'))
        self.assertEqual(instruction(startup, 0x223c58), ('call', '0x2290e0'))
        self.assertEqual(instruction(functions[0x2290e0], 0x229104), ('shl', 'rdi, 6'))
        self.assertEqual(instruction(functions[0x2290e0], 0x229108), ('add', 'rdi, rcx'))
        self.assertEqual(instruction(functions[0x2290e0], 0x22912c), ('mov', 'rdx, rdi'))
        self.assertEqual(instruction(functions[0x2290e0], 0x229134), ('call', '0x3f840'))
        self.assertEqual(instruction(functions[0x22a7a0], 0x22a807), ('mov', 'dword ptr [rcx + 4], esi'))
        self.assertEqual(instruction(functions[0x22a820], 0x22a879), ('mov', 'dword ptr [rbx], eax'))
        self.assertEqual(instruction(functions[0x229010], 0x22907d), ('shl', 'rax, 6'))
        self.assertEqual(instruction(functions[0x229010], 0x229087), ('mov', 'rcx, qword ptr [rax + rbx + 0x38]'))
        self.assertEqual(instruction(functions[0x229db0], 0x229e19), ('jmp', '0x2285e0'))
        self.assertEqual(instruction(functions[0x2285e0], 0x2285ed), ('mov', 'rsi, qword ptr [rcx + 0x480]'))
        self.assertEqual(instruction(functions[0x2285e0], 0x228684), ('call', '0x371470'))
        # Submission is queued on one path, inline on another (same worker or
        # descriptor flag). Neither mode supplies a universal receiver lease.
        submit = functions[0x2be5d0]
        self.assertEqual(instruction(submit, 0x2be740), ('je', '0x2be79b'))
        self.assertEqual(instruction(submit, 0x2be77d), ('mov', 'qword ptr [rcx], rbx'))
        self.assertEqual(instruction(submit, 0x2be7d2), ('call', 'qword ptr [rax + 0x10]'))
        self.assertFalse(analyze_barrier(submit, 0x2be65e, 0x2be7d2)['barrier_on_every_static_path'])
        self.assertFalse(analyze_aliases(functions[0x2285e0], [(0xd2a970, 8)], 0xd2a4f0)['lifetime_verified'])

    @private_evidence
    def test_original_additional_scene_job_is_polled_and_not_a_pool_join(self):
        from capstone import Cs,CS_ARCH_X86,CS_MODE_64,CS_GRP_CALL
        from capstone.x86_const import X86_OP_MEM
        fixture=json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())
        functions={r['entry']:r['function'] for r in fixture['fixtures']}
        callback=fixture['callback_word']
        self.assertEqual(int.from_bytes(bytes.fromhex(callback['code_hex']),'little')-callback['preferred_image_base'],0x229db0)
        ordered=analyze_barrier(functions[0x227420],0x2274f6,0x227555)
        self.assertTrue(ordered['barrier_on_every_static_path'])
        self.assertFalse(ordered['runtime_synchronization_verified'])
        decoder=Cs(CS_ARCH_X86,CS_MODE_64); decoder.detail=True
        imports_by_entry={}
        for entry in (0x2be510,0x2bec70):
            names=[]
            for ins in decoder.disasm(bytes.fromhex(functions[entry]['code_hex']),entry):
                if ins.group(CS_GRP_CALL) and ins.operands[0].type==X86_OP_MEM and ins.reg_name(ins.operands[0].mem.base)=='rip':
                    target=ins.address+ins.size+ins.operands[0].mem.disp
                    names.append(fixture['imports'].get(hex(target)))
            imports_by_entry[entry]=names
        self.assertIn('_Thrd_join',imports_by_entry[0x2be510])
        self.assertFalse(set(imports_by_entry[0x2bec70]) & {'_Thrd_join','WaitForSingleObject','WaitForMultipleObjects'})
        # Thread-ID zero and self-join failure paths bypass the join; neither
        # this conditional shutdown nor polling is universal synchronization.
        joined=analyze_barrier(functions[0x2be510],0x2be53d,0x2be56d)
        self.assertFalse(joined['barrier_on_every_static_path'])
        self.assertEqual(set(joined['bypass_exit_rvas']),{'0x2be5b9','0x2be5cf'})
        self.assertFalse(joined['runtime_synchronization_verified'])
        originals=json.loads((Path(__file__).parent/'fixtures/engine_call_originals.json').read_text())['fixtures']
        frame=next(r for r in originals if r['game']=='ysxpn' and r['begin']==0x761520)
        image=bytearray(frame['end']); image[frame['begin']:frame['end']]=bytes.fromhex(frame['code_hex'])
        class FrameFamily:
            def contiguous(self,at):
                return frame['begin'],frame['end']
        fn=inspect_function(bytes(image),[(frame['begin'],frame['end'])],FrameFamily(),frame['begin'],capstone_decoder(bytes(image)))
        gate=analyze_barrier(fn,0x76173d,0x76176f)
        self.assertTrue(gate['barrier_on_every_static_path'])
        self.assertFalse(gate['runtime_synchronization_verified'])

    @private_evidence
    def test_original_ys_and_daybreak_contrasts_keep_build_differences(self):
        fixture=json.loads((Path(__file__).parent/'fixtures/engine_ownership_contrasts.json').read_text())
        for row in fixture['fixtures']:
            result=analyze_aliases(row['function'],row['watches'],row['receiver_hypothesis_rva'])
            observed={(int(e['rva'],16),e['operand_bytes']) for e in result['effects'] if e['write']}
            self.assertEqual(observed,{tuple(pair) for pair in row['expected_writes']},row['name'])
            self.assertFalse(result['runtime_profile_accepted'])
            # Applying the Ys slot assumptions to a Daybreak shutdown body
            # yields no known writer, rather than relabeling its other members.
            if row['name'].startswith('daybreak'):
                root=row['receiver_hypothesis_rva']
                changed=analyze_aliases(row['function'],[(root+0x480,16)],root)
                self.assertFalse(changed['effects'])
                self.assertFalse(changed['lifetime_verified'])

    @private_evidence
    def test_independent_original_scene_writers(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_ownership_originals.json').read_text())
        for row in fixture['fixtures']:
            result = analyze_aliases(row['function'], [(0xd2a970,8),(0xd2a978,8),(0xd2a280,8)], row.get('receiver_hypothesis_rva'))
            observed = {(int(effect['rva'],16),effect['operand_bytes']) for effect in result['effects'] if effect['write']}
            self.assertEqual(observed, {tuple(pair) for pair in row['expected_writes']}, row['name'])
            self.assertFalse(result['runtime_profile_accepted'])
            self.assertFalse(result['lifetime_verified'])
            self.assertFalse(result['complete_producer_coverage'])

    def test_branch_alias_union_and_partial_register_invalidation(self):
        # One branch retains RCX in RBX; the other clears EBX. Both reach write.
        row=original_function('488bd985d2740233db48894310c3')
        effects=analyze_aliases(row,[(0x1010,8)],0x1000)['effects']
        self.assertEqual(len(effects),1)
        self.assertTrue(effects[0]['ambiguous'])
        self.assertEqual(effects[0]['address_rvas'],['0x1010'])
        row=original_function('488bd983c30848894310c3')
        self.assertFalse(analyze_aliases(row,[(0x1018,8)],0x1000)['effects'])

    def test_calls_clobber_volatile_aliases_and_preserve_nonvolatile(self):
        row=original_function('488bd9e8000000004889411048894310c3')
        result=analyze_aliases(row,[(0x1010,8)],0x1000)
        self.assertEqual(len(result['effects']),1)
        self.assertIn('[rbx',result['effects'][0]['instruction'])
        self.assertEqual(result['escapes'][0]['register_addresses'],{'rcx':['0x1000']})

    def test_rip_address_transfer_and_index_refusal(self):
        row=original_function('488d1df90e00004889431048890453c3')
        result=analyze_aliases(row,[(0x1010,8)])
        self.assertEqual(len(result['effects']),1)
        self.assertEqual(result['effects'][0]['address_rvas'],['0x1010'])
        self.assertTrue(any('index range' in r['reason'] for r in result['unresolved']))

    def test_loop_alternative_bound_terminates_without_lifetime_claim(self):
        row=original_function('4883c10885d275f8488901c3')
        result=analyze_aliases(row,[(0x1008,8)],0x1000)
        self.assertLess(result['fixed_point_steps'],100)
        self.assertTrue(result['unresolved'])
        self.assertFalse(result['lifetime_verified'])

    @private_evidence
    def test_original_vector_writer_and_supported_widths(self):
        for prefix,opcode,width in [('66','7f',16),('f3','7f',16),('','29',16),('66','29',16),('f3','11',4),('f2','11',8),('','11',16)]:
            code=bytes.fromhex(prefix+'0f'+opcode+'05'+'00000000')
            effect=reference(code,0,len(code))
            self.assertEqual((effect['operation'],effect['operand_bytes'],effect['target_rva']),('write',width,len(code)))
            self.assertIsNone(reference(code,0,len(code)-1))
        for code in ('0f7f0500000000','f20f7f0500000000','f30f290500000000','f348890500000000'):
            raw=bytes.fromhex(code)
            self.assertIsNone(reference(raw,0,len(raw)))
        fixture=json.loads((Path(__file__).parent/'fixtures/engine_ownership_originals.json').read_text())
        row=next(r for r in fixture['fixtures'] if r['name']=='pn-static-capp-initializer')['function']
        start=int(row['walk']['function_rva'],16)
        image=bytes(start)+bytes.fromhex(row['code_hex'])
        effect=reference(image,0x21f0d6,len(image))
        self.assertEqual((effect['target_rva'],effect['operand_bytes'],effect['operation']),(0xd2a970,16,'write'))
        # The controller root lies in the upper half of the SIMD store. Exact
        # target matching misses it; watched ranges preserve the overlapping write.
        image=bytearray(0xd2a990)
        end=int(row['walk']['end_rva'],16)
        image[start:end]=bytes.fromhex(row['code_hex'])
        class OriginalFamily:
            def contiguous(self,at):
                return start,end
        decode=capstone_decoder(bytes(image))
        exact=trace_references(bytes(image),[(start,end)],OriginalFamily(),[0xd2a978],decode)
        watched=trace_references(bytes(image),[(start,end)],OriginalFamily(),[],decode,[(0xd2a978,8)])
        self.assertFalse(exact['functions'])
        sites=[site for f in watched['functions'] for site in f['sites']]
        self.assertEqual([(s['rva'],s['operation']) for s in sites],[('0x21f0d6','write')])

    def test_missing_receiver_and_tampered_or_incomplete_evidence_refused(self):
        row=original_function('488bd948894310c3')
        self.assertFalse(analyze_aliases(row,[(0x1010,8)])['effects'])
        for mutate in (lambda r:r['walk'].update(complete_static_walk=False),
                       lambda r:r.update(code_hex='90'+r['code_hex'][2:]),
                       lambda r:r['instructions'][0].update(size=1),
                       lambda r:r['walk']['edges'].pop(),
                       lambda r:r['instructions'].pop()):
            altered=copy.deepcopy(row); mutate(altered)
            with self.assertRaises(ValueError):
                analyze_aliases(altered,[(0x1010,8)],0x1000)
        for watches in ([],[(True,8)],[(0,0)],[(0x8000000,1)]):
            with self.assertRaises(ValueError):
                analyze_aliases(row,watches)


if __name__=='__main__':
    unittest.main()
