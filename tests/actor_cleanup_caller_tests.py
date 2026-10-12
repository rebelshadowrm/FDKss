"""Static caller walks must reject ambiguous edges and unreachable byte matches."""
from publication_fixtures import private_evidence
import sys
import hashlib
import json
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from discover_actor_cleanup_callers import walk_function


class ActorCleanupCallerTests(unittest.TestCase):
    @private_evidence
    def test_frozen_original_function_edges_in_both_builds(self):
        fixture = json.loads((Path(__file__).parent / 'fixtures/actor_cleanup_caller_originals.json').read_text())
        self.assertEqual({row['game'] for row in fixture['fixtures']}, {'sky1', 'sky2'})
        for row in fixture['fixtures']:
            with self.subTest(game=row['game'], function=row['function_rva']):
                image = bytes.fromhex(row['code_hex'])
                self.assertEqual(hashlib.sha256(image).hexdigest(), row['body_sha256'])
                instructions = {}
                for instruction in row['instructions']:
                    at, size = instruction['offset'], instruction['size']
                    self.assertEqual(image[at:at + size].hex(), instruction['code_hex'])
                    instructions[at] = dict(size=size, kind=instruction['kind'], target=instruction['target_offset'])
                result = walk_function(image, 0, len(image), row['helper_offset'],
                                       lambda at, end: instructions.get(at))
                self.assertEqual(result['complete_static_walk'], row['complete_static_walk'])
                self.assertEqual([site['kind'] for site in result['sites']], [row['expected_site_kind']])

    def walk(self, rows, end=32, budget=65536):
        image = bytes(range(64))
        instructions = {at: dict(size=size, kind=kind, target=target) for at, size, kind, target in rows}
        return walk_function(image, 0, end, 60, lambda at, stop: instructions.get(at), budget)

    def test_unreachable_call_after_return_is_excluded(self):
        result = self.walk([(0, 1, 'return', None), (1, 5, 'call', 60)])
        self.assertTrue(result['complete_static_walk'])
        self.assertEqual(result['sites'], [])

    def test_jump_skips_embedded_call_bytes(self):
        result = self.walk([(0, 2, 'jump', 7), (2, 5, 'call', 60), (7, 1, 'return', None)])
        self.assertTrue(result['complete_static_walk'])
        self.assertEqual(result['sites'], [])

    def test_both_conditional_edges_and_call_continuation(self):
        result = self.walk([(0, 2, 'conditional', 8), (2, 5, 'call', 60),
                            (7, 1, 'return', None), (8, 5, 'jump', 60)])
        self.assertTrue(result['complete_static_walk'])
        self.assertEqual([(row['rva'], row['kind']) for row in result['sites']],
                         [('0x2', 'direct_call'), ('0x8', 'tail_jump')])

    def test_cycle_terminates_and_preserves_one_call(self):
        result = self.walk([(0, 5, 'call', 60), (5, 2, 'conditional', 0), (7, 1, 'return', None)])
        self.assertTrue(result['complete_static_walk'])
        self.assertEqual(result['instruction_count'], 3)
        self.assertEqual(len(result['sites']), 1)

    def test_branch_into_instruction_interior_invalidates_walk(self):
        result = self.walk([(0, 2, 'conditional', 3), (2, 5, 'call', 60),
                            (3, 1, 'return', None), (7, 1, 'return', None)])
        self.assertFalse(result['complete_static_walk'])
        self.assertIn('overlapping', result['unresolved'][0]['reason'])
        result = self.walk([(0, 5, 'call', 60), (5, 2, 'jump', 1)])
        self.assertFalse(result['complete_static_walk'])
        self.assertIn('interior', result['unresolved'][0]['reason'])

    def test_indirect_or_external_conditional_edges_remain_unproven(self):
        for rows in ([(0, 2, 'jump', None)],
                     [(0, 2, 'conditional', 40), (2, 1, 'return', None)]):
            with self.subTest(rows=rows):
                self.assertFalse(self.walk(rows)['complete_static_walk'])

    def test_truncation_decode_failure_and_fallthrough_are_rejected(self):
        for rows, end in (([(0, 5, 'call', 60)], 4), ([], 32), ([(0, 1, 'ordinary', None)], 1)):
            with self.subTest(rows=rows):
                self.assertFalse(self.walk(rows, end)['complete_static_walk'])

    def test_bounds_budget_and_decoder_contract(self):
        with self.assertRaises(ValueError):
            self.walk([], end=65)
        with self.assertRaises(ValueError):
            self.walk([], budget=0)
        result = self.walk([(0, 1, 'ordinary', None), (1, 1, 'return', None)], budget=1)
        self.assertFalse(result['complete_static_walk'])
        self.assertIn('budget', result['unresolved'][0]['reason'])
        with self.assertRaises(ValueError):
            self.walk([(0, 1, 'invented', None)])


if __name__ == '__main__':
    unittest.main()
