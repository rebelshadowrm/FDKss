import json
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from export_commands import export


class CommandExportTests(unittest.TestCase):
    def query(self, source):
        if 'build_sha256' in source:
            return 'fixture'
        if 'script_command_groups' in source:
            return json.dumps([{'group': i, 'slot_count': 18, 'implemented_count': 17} for i in range(28)])
        group, cursor, limit = map(int, source.split('page",')[1].rstrip(')').split(','))
        rows = [{'group': group, 'command': i, 'handler_rva': 4096 + i * 16,
                 'raw_mask': 0xffffffff, 'raw_entry_hex': '00' * 24}
                for i in range(cursor, min(cursor + limit, 18)) if i != 1]
        return json.dumps({'next': 16 if cursor == 0 else -1, 'entries': rows})

    def test_holes_and_last_page(self):
        result = export(0, '', self.query)
        self.assertEqual(len(result['commands']), 28 * 17)
        self.assertEqual(result['commands'][-1]['command'], 17)

    def test_bad_cursor_duplicate_and_changed_count(self):
        for failure in ('cursor', 'duplicate', 'count'):
            def query(source):
                response = self.query(source)
                if 'script_command_page' in source:
                    data = json.loads(response)
                    if failure == 'cursor': data['next'] = 0
                    if failure == 'duplicate': data['entries'][1] = data['entries'][0]
                    if failure == 'count': data['entries'].pop()
                    return json.dumps(data)
                return response
            with self.subTest(failure=failure), self.assertRaises(ValueError):
                export(0, '', query)


if __name__ == '__main__':
    unittest.main()
