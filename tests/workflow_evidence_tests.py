from publication_fixtures import private_evidence
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from check_checkpoint import check
from freeze_engine_evidence import freeze_functions, freeze_pe, freeze_tail_thunks
from inspect_job_manager import PROFILES, select_profile, worker_original, sample_manager


class CheckpointTests(unittest.TestCase):
    def test_recorded_counts_hashes_and_failed_acceptance_remain_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'index.json').write_text(json.dumps({'next_step': 'Inspect receiver retention.'}))
            (root/'plan.md').write_text('Next: Inspect receiver retention.')
            (root/'artifact').write_bytes(b'original')
            log = root/'tests.txt'
            for passed, encoding in [(12, 'utf-8'), (11, 'utf-16')]:
                log.write_text(f'{round(passed/12*100)}% tests passed, {12-passed} tests failed out of 12\n', encoding=encoding)
                manifest = {'version': 1, 'next_step': 'Inspect receiver retention.',
                            'artifacts': [{'path': 'artifact', 'sha256': hashlib.sha256(b'original').hexdigest()}],
                            'test_runs': [{'kind': 'ctest', 'path': 'tests.txt',
                                'sha256': hashlib.sha256(log.read_bytes()).hexdigest(), 'passed': passed, 'total': 12}]}
                (root/'manifest.json').write_text(json.dumps(manifest))
                result = check(root, 'plan.md', 'index.json', 'manifest.json')
                self.assertEqual(result['artifacts_checked'], 1)
                self.assertEqual(result['all_recorded_test_runs_passed'], passed == 12)
                self.assertFalse(result['runtime_acceptance_verified'])
                manifest['test_runs'][0]['passed'] = 10
                (root/'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaisesRegex(ValueError, 'counts differ'):
                    check(root, 'plan.md', 'index.json', 'manifest.json')
            (root/'artifact').write_bytes(b'replaced')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                check(root, 'plan.md', 'index.json', 'manifest.json')

    def test_missing_next_step_and_ambiguous_log_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'index').write_text(json.dumps({'next_step': 'new task'}))
            (root/'plan').write_text('old task')
            with self.assertRaisesRegex(ValueError, 'missing from plan'):
                check(root, 'plan', 'index')
            (root/'plan').write_text('new task')
            log = b'100% tests passed, 0 tests failed out of 2\n'*2
            (root/'log').write_bytes(log)
            manifest = dict(version=1, next_step='new task', test_runs=[dict(kind='ctest',
                            path='log', sha256=hashlib.sha256(log).hexdigest(), passed=2, total=2)])
            (root/'manifest').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'unambiguous'):
                check(root, 'plan', 'index', 'manifest')
            self.assertEqual((root/'plan').read_text(), 'new task')
            self.assertFalse(check(root, 'plan', 'index')['runtime_acceptance_verified'])


class JobManagerDiagnosticTests(unittest.TestCase):
    @private_evidence
    def test_diagnostic_rejects_unknown_or_stale_identity_and_worker_evidence(self):
        profile = PROFILES['ysx_pn.exe']
        identity = dict(path='ysx_pn.exe',sha256=profile['sha256'],pid=10,
                        creation_filetime=42,image_base=0x140000000)
        self.assertEqual(select_profile(identity),profile)
        for change in [dict(path='kai.exe'),dict(sha256='0'*64),dict(pid=True),
                       dict(creation_filetime=0),dict(image_base=None)]:
            with self.assertRaises(ValueError):
                select_profile(identity | change)
        fixture = json.loads((Path(__file__).parent/'fixtures'/profile['fixture']).read_text())
        self.assertTrue(worker_original(fixture,profile))
        with self.assertRaises(ValueError):
            worker_original(fixture | dict(executable_sha256='0'*64),profile)
        fn = next(r['function'] for r in fixture['fixtures'] if r['entry']==profile['worker'])
        fn['code_hex'] = '90'+fn['code_hex'][2:]
        with self.assertRaises(ValueError):
            worker_original(fixture,profile)

    def test_non_atomic_scalar_sample_refuses_replacement_and_never_follows_jobs(self):
        import struct
        profile = PROFILES['ysx_pn.exe']; base = 0x140000000; manager = 0x12340000
        raw = bytearray(profile['extent'])
        struct.pack_into('<Q',raw,0,base+profile['table'])
        struct.pack_into('<Q',raw,0x18,3)
        struct.pack_into('<Q',raw,0x60,2)
        struct.pack_into('<I',raw,0x90,5)
        reads = []
        def read(at,size):
            reads.append((at,size))
            if at==base+profile['root']:
                return struct.pack('<Q',manager)
            if at==manager:
                return bytes(raw[:size])
            self.fail('diagnostic followed an unapproved pointer')
        result = sample_manager(read,base,profile)
        self.assertEqual(result['raw_members']['pending_group_count_60'],2)
        self.assertTrue(result['non_atomic'])
        for key in ['lifetime_verified','same_address_reuse_excluded','phase_synchronization_verified','runtime_profile_accepted']:
            self.assertFalse(result[key])
        self.assertEqual(reads,[(base+profile['root'],8),(manager,0x150),
                                (base+profile['root'],8),(manager,8)])
        count = 0
        def replaced(at,size):
            nonlocal count
            if at==base+profile['root']:
                count += 1
                return struct.pack('<Q',manager if count==1 else manager+0x1000)
            return bytes(raw[:size])
        with self.assertRaisesRegex(ValueError,'changed'):
            sample_manager(replaced,base,profile)
        for pointer in [0,0xffffffffffffffff]:
            with self.assertRaises(ValueError):
                sample_manager(lambda at,size:struct.pack('<Q',pointer),base,profile)
        struct.pack_into('<Q',raw,0,base+profile['table']+8)
        with self.assertRaisesRegex(ValueError,'table/extent'):
            sample_manager(read,base,profile)


class FrozenEvidenceTests(unittest.TestCase):
    def test_forwarding_tail_thunk_preserves_exact_bytes_and_refuses_other_control_flow(self):
        # Exact PN callback thunk: load the item index, adjust closure receiver,
        # then jump into its complete registered implementation.
        code = bytes.fromhex('458b004883c108e9d4baffff')
        image = bytearray(0x76be8c)
        image[0x76be80:0x76be8c] = code
        row = freeze_tail_thunks(bytes(image), [(0x767960, len(image))], [0x76be80])[0]
        self.assertEqual(row['tail_target_rva'], 0x767960)
        self.assertEqual(row['code_hex'], code.hex())
        self.assertEqual([r['size'] for r in row['instructions']], [3,4,5])
        self.assertFalse(row['receiver_verified'])
        self.assertFalse(row['runtime_profile_accepted'])
        for bad in ('c3', 'ffe0', '7400e900000000', 'e800000000e900000000',
                    'cc', 'ebfe', 'e9ffffff7f', '0f05e900000000', '90'*32):
            data = bytes(0x100) + bytes.fromhex(bad) + bytes(64)
            with self.assertRaises(ValueError, msg=bad):
                freeze_tail_thunks(data, [(0x100,len(data))], [0x100])
        with self.assertRaises(ValueError):
            freeze_tail_thunks(bytes(image), [(0x767960,len(image))], [0x76be80]*2)
        with self.assertRaises(ValueError):
            freeze_tail_thunks(bytes(image), [(0x767960,0x76be85)], [0x76be80])

    @private_evidence
    def test_complete_original_roundtrip_and_primary_entry_refusal(self):
        fixture = json.loads((Path(__file__).parent/'fixtures/engine_scheduler_originals.json').read_text())
        fn = next(row['function'] for row in fixture['fixtures'] if row['entry'] == 0x229db0)
        a, b = 0x229db0, int(fn['walk']['end_rva'], 16)
        image = bytearray(b)
        image[a:b] = bytes.fromhex(fn['code_hex'])
        class Family:
            def contiguous(self, at):
                return a, b
        result = freeze_functions(bytes(image), [(a, b)], Family(), [a])
        self.assertEqual(result[0]['function']['code_hex'], fn['code_hex'])
        self.assertEqual(result[0]['function']['walk']['body_sha256'], fn['walk']['body_sha256'])
        self.assertFalse(result[0]['function']['runtime_profile_accepted'])
        with self.assertRaises(ValueError):
            freeze_functions(bytes(image), [(a, b)], Family(), [a+1])
        with self.assertRaises(ValueError):
            freeze_functions(bytes(image), [(a, b)], Family(), [a, a])

    def test_overlapping_decode_and_wrong_file_pin_refused(self):
        # Entry jumps inside the MOV of a linear decode. It must not become an
        # independently corroborated original family even if the branch decodes.
        image = bytes(0x100)+bytes.fromhex('eb01b89090c3c3')
        class Family:
            def contiguous(self, at):
                return 0x100, len(image)
        with self.assertRaises(ValueError):
            freeze_functions(image, [(0x100, len(image))], Family(), [0x100])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'not-game.exe'
            path.write_bytes(b'changed executable')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                freeze_pe(path, '0'*64, [0x100])
            with self.assertRaisesRegex(ValueError, 'invalid PE'):
                freeze_pe(path, hashlib.sha256(path.read_bytes()).hexdigest(), [0x100])


if __name__ == '__main__':
    unittest.main()
