import hashlib
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import export_resource


class ResourceExportTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / 'result.dat'
        self.output.write_bytes(b'previous export')
        game = self.root / 'game' / 'sora_2nd.exe'
        class Process:
            def __init__(self, pid): self.path = game
            def __enter__(self): return self
            def __exit__(self, *args): pass
        self.process = patch.object(export_resource, 'Process', Process)
        self.process.start()
        self.addCleanup(self.process.stop)

    def transport(self, data, changed=False, truncated=False):
        def execute(pid, mod, source):
            if 'game.build_sha256' in source:
                return 'd8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf'
            offset = int(re.search(r', (\d+), 4096\)', source).group(1))
            page = b'' if truncated else data[offset:offset + 4096]
            size = len(data) + (1 if changed and offset else 0)
            return page.hex() + '\t' + str(size)
        return patch.object(export_resource, 'execute', execute)

    def test_binary_pages_and_checksum(self):
        data = bytes(range(256)) * 40 + b'\n\x00\t'
        with self.transport(data):
            result = export_resource.export(1, 'Test', 'script/test.dat', self.output)
        self.assertEqual(self.output.read_bytes(), data)
        self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(result['size_bytes'], len(data))

    def test_changed_source_preserves_existing_output(self):
        with self.transport(b'x' * 5000, changed=True):
            with self.assertRaisesRegex(RuntimeError, 'length changed'):
                export_resource.export(1, 'Test', 'test.dat', self.output)
        self.assertEqual(self.output.read_bytes(), b'previous export')
        self.assertEqual(list(self.root.glob('*.tmp')), [])

    def test_truncated_or_oversize_source_preserves_output(self):
        with self.transport(b'x' * 5000, truncated=True):
            with self.assertRaisesRegex(RuntimeError, 'incomplete'):
                export_resource.export(1, 'Test', 'test.dat', self.output)
        with self.transport(b'x' * 5000):
            with self.assertRaisesRegex(ValueError, 'limit'):
                export_resource.export(1, 'Test', 'test.dat', self.output, max_bytes=100)
        self.assertEqual(self.output.read_bytes(), b'previous export')

    def test_game_installation_is_protected(self):
        with self.assertRaisesRegex(ValueError, 'outside the game'):
            export_resource.export(1, 'Test', 'test.dat', self.root / 'game' / 'test.dat')


if __name__ == '__main__':
    unittest.main()
