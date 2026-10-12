"""Generic engine caller evidence must not promote raw byte matches."""
from publication_fixtures import private_evidence
import struct
import sys
import unittest
import hashlib
import json
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from trace_engine_calls import analyze_barrier, inspect_function, trace


class Families:
    def __init__(self, end=16, failure=None):
        self.end, self.failure = end, failure

    def contiguous(self, at):
        if self.failure:
            raise ValueError(self.failure)
        return (0, self.end)


class EngineCallTraceTests(unittest.TestCase):
    @private_evidence
    def test_original_ys_frame_wake_to_completion_wait_order(self):
        fixtures = json.loads((Path(__file__).parent / 'fixtures/engine_call_originals.json').read_text())['fixtures']
        row = next(row for row in fixtures if row['game'] == 'ysx' and row['begin'] == 0x6a8cf0)
        a, b = row['begin'], row['end']; image = bytearray(b)
        image[a:b] = bytes.fromhex(row['code_hex'])
        instructions = {i['rva']: i for i in row['instructions']}
        class OriginalFamily:
            def contiguous(self, at):
                return a, b
        function = inspect_function(bytes(image), [(a, b)], OriginalFamily(), a, lambda at, end: instructions.get(at))
        result = analyze_barrier(function, 0x6a8f52, 0x6a9209)
        self.assertTrue(result['barrier_on_every_static_path'])
        self.assertFalse(result['runtime_synchronization_verified'])
        self.assertFalse(analyze_barrier(function, 0x6a8f52, a)['barrier_reachable'])

    @private_evidence
    def test_original_frame_wake_to_completion_wait_order(self):
        fixtures = json.loads((Path(__file__).parent / 'fixtures/engine_call_originals.json').read_text())['fixtures']
        row = next(row for row in fixtures if row['game'] == 'ysxpn' and row['begin'] == 0x761520)
        a, b = row['begin'], row['end']
        image = bytearray(b); image[a:b] = bytes.fromhex(row['code_hex'])
        instructions = {i['rva']: i for i in row['instructions']}
        class OriginalFamily:
            def contiguous(self, at):
                return a, b
        function = inspect_function(bytes(image), [(a, b)], OriginalFamily(), a,
                                    lambda at, end: instructions.get(at))
        # Independently traced RIP-import calls: ReleaseSemaphore(Framework+e8)
        # then WaitForSingleObject(Framework+f0), not inferred from API names.
        result = analyze_barrier(function, 0x76176f, 0x761899)
        self.assertTrue(result['barrier_on_every_static_path'])
        self.assertEqual(result['bypass_exit_rvas'], [])
        self.assertFalse(result['runtime_synchronization_verified'])
        self.assertFalse(analyze_barrier(function, 0x76176f, 0x7616cb)['barrier_reachable'])
        with self.assertRaises(ValueError):
            analyze_barrier(function, 0x761770, 0x761899)

    def test_barrier_bypass_cycles_external_exits_and_incomplete_walk(self):
        def function(rows):
            image = bytes(64)
            instructions = {at: dict(size=size, kind=kind, target=target)
                            for at, size, kind, target in rows}
            return inspect_function(image, [(0, 64)], Families(12), 0,
                                    lambda at, end: instructions.get(at))
        bypass = function([(0, 2, 'conditional', 8), (2, 2, 'ordinary', None),
                           (4, 1, 'return', None), (8, 1, 'return', None)])
        result = analyze_barrier(bypass, 0, 4)
        self.assertTrue(result['barrier_reachable'])
        self.assertEqual(result['bypass_exit_rvas'], ['0x8'])
        self.assertFalse(result['barrier_on_every_static_path'])
        cyclic = function([(0, 2, 'conditional', 4), (2, 2, 'jump', 0), (4, 1, 'return', None)])
        result = analyze_barrier(cyclic, 0, 4)
        self.assertTrue(result['all_finite_static_exit_paths_cross_barrier'])
        self.assertTrue(result['avoiding_barrier_cycle_possible'])
        self.assertFalse(result['barrier_on_every_static_path'])
        external = function([(0, 2, 'conditional', 8), (2, 2, 'jump', 60), (8, 1, 'return', None)])
        self.assertEqual(analyze_barrier(external, 0, 8)['bypass_exit_rvas'], ['0x2'])
        incomplete = function([(0, 2, 'jump', None)])
        with self.assertRaises(ValueError):
            analyze_barrier(incomplete, 0, 8)

    @private_evidence
    def test_original_constructor_cleanup_and_scene_routes(self):
        fixtures = json.loads((Path(__file__).parent / 'fixtures/engine_call_originals.json').read_text())['fixtures']
        self.assertEqual({row['game'] for row in fixtures}, {'ysxpn', 'ysx', 'daybreak1', 'daybreak2'})
        for row in fixtures:
            with self.subTest(game=row['game'], begin=hex(row['begin'])):
                body = bytes.fromhex(row['code_hex'])
                self.assertEqual(hashlib.sha256(body).hexdigest(), row['body_sha256'])
                a, b = row['begin'], row['end']
                image = bytearray(max(b, max(row['targets']) + 1))
                image[a:b] = body
                instructions = {}
                for instruction in row['instructions']:
                    at, size = instruction['rva'], instruction['size']
                    self.assertEqual(image[at:at + size].hex(), instruction['code_hex'])
                    instructions[at] = instruction
                class OriginalFamily:
                    def contiguous(self, at):
                        if not a <= at < b:
                            raise ValueError('outside frozen family')
                        return a, b
                sections = [(a, b)] + [(at, at + 1) for at in row['targets'] if not a <= at < b]
                result = trace(bytes(image), sections, OriginalFamily(), row['targets'],
                               lambda at, end: instructions.get(at))
                self.assertEqual(len(result['callers']), 1)
                self.assertEqual(result['rejected'], [])
                self.assertEqual({int(site['target_rva'], 16) for site in result['callers'][0]['sites']}, set(row['targets']))
                inspected = inspect_function(bytes(image), sections, OriginalFamily(), a,
                                             lambda at, end: instructions.get(at))
                self.assertTrue(inspected['walk']['complete_static_walk'])
                self.assertEqual(inspected['code_hex'], row['code_hex'])
                self.assertEqual(inspected['walk']['body_sha256'], row['body_sha256'])

    def scan(self, rows, calls=(0,), opcode=0xe8, families=None):
        image = bytearray(64)
        for at in calls:
            image[at] = opcode
            struct.pack_into('<i', image, at + 1, 60 - at - 5)
        instructions = {at: dict(size=size, kind=kind, target=target)
                        for at, size, kind, target in rows}
        return trace(bytes(image), [(0, 64)], families or Families(), [60],
                     lambda at, end: instructions.get(at))

    def test_decoded_calls_and_tail_edges(self):
        for kind, opcode in [('call', 0xe8), ('jump', 0xe9)]:
            rows = [(0, 5, kind, 60)] + ([(5, 1, 'return', None)] if kind == 'call' else [])
            result = self.scan(rows, opcode=opcode)
            self.assertEqual(len(result['callers']), 1)
            self.assertEqual(result['callers'][0]['sites'][0]['target_rva'], '0x3c')
            self.assertEqual(result['callers'][0]['sites'][0]['kind'], 'direct_call' if kind == 'call' else 'tail_jump')
            self.assertFalse(result['runtime_profile_accepted'])

    def test_skipped_bytes_and_instruction_interiors(self):
        for rows in [[(0, 1, 'return', None)],
                     [(0, 6, 'ordinary', None), (6, 1, 'return', None)],
                     [(0, 2, 'jump', 7), (7, 1, 'return', None)]]:
            result = self.scan(rows, calls=(1,))
            self.assertEqual(result['callers'], [])
            self.assertEqual(result['rejected'][0]['reason'], 'no entry-reachable decoded target edge')

    def test_incomplete_walk_never_accepted_even_with_target(self):
        for rows in [[(0, 5, 'call', 60), (5, 2, 'jump', None)],
                     [(0, 5, 'call', 60), (5, 2, 'conditional', 40), (7, 1, 'return', None)]]:
            result = self.scan(rows)
            self.assertEqual(result['callers'], [])
            self.assertEqual(result['rejected'][0]['reason'], 'incomplete static walk')
            self.assertEqual(len(result['rejected'][0]['sites']), 1)

    def test_family_rejections_and_truncated_section_tail(self):
        for families in [Families(failure='noncontiguous family'), Families(end=4)]:
            result = self.scan([], families=families)
            self.assertEqual(result['callers'], [])
            self.assertEqual(result['rejected'][0]['rva'], '0x0')
        image = b'\0' * 60 + b'\xe8\0\0\0'
        self.assertEqual(trace(image, [(0, 64)], Families(), [60], lambda at, end: None)['rejected'], [])

    def test_unique_bounded_executable_targets(self):
        for sections, targets in [([], [60]), ([(0, 65)], [60]), ([(0, 64)], []),
                                  ([(0, 64)], [60, 60]), ([(0, 64)], [64]),
                                  ([(0, 64)], [True]), ([(0, 64)], list(range(65)))]:
            with self.subTest(sections=sections, targets=targets):
                with self.assertRaises(ValueError):
                    trace(bytes(64), sections, Families(), targets, lambda at, end: None)

    def test_two_targets_share_one_walk(self):
        image = bytearray(64)
        for at, target in [(0, 60), (5, 61)]:
            image[at] = 0xe8
            struct.pack_into('<i', image, at + 1, target - at - 5)
        rows = {0: dict(size=5, kind='call', target=60), 5: dict(size=5, kind='call', target=61),
                10: dict(size=1, kind='return', target=None)}
        seen = []
        def decode(at, end):
            seen.append(at)
            return rows.get(at)
        result = trace(bytes(image), [(0, 64)], Families(), [60, 61], decode)
        self.assertEqual(seen, [0, 5, 10])
        self.assertEqual(len(result['callers'][0]['sites']), 2)

    def test_exact_primary_entry_required_for_inspection(self):
        for entry, families, sections in [(1, Families(), [(0, 64)]),
                                          (0, Families(failure='separated family'), [(0, 64)]),
                                          (0, Families(), [(1, 64)])]:
            with self.assertRaises(ValueError):
                inspect_function(bytes(64), sections, families, entry, lambda at, end: None)
        rows = {0: dict(size=1, kind='return', target=None)}
        result = inspect_function(bytes(64), [(0, 64)], Families(), 0, lambda at, end: rows.get(at))
        self.assertTrue(result['walk']['complete_static_walk'])
        self.assertEqual([r['rva'] for r in result['instructions']], ['0x0'])
        self.assertEqual(len(bytes.fromhex(result['code_hex'])), 16)
        self.assertFalse(result['runtime_profile_accepted'])
        result = inspect_function(bytes(64), [(0, 64)], Families(), 0, lambda at, end: None)
        self.assertFalse(result['walk']['complete_static_walk'])


if __name__ == '__main__':
    unittest.main()
