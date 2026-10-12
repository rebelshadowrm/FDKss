"""Comparison artifacts must preserve originals and explicit build differences."""
from publication_fixtures import private_evidence
import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compare_engine import compare
import engine_thread_discovery_tests as fixtures


class EngineComparisonTests(unittest.TestCase):
    @private_evidence
    def test_original_thread_layouts_remain_distinct_in_matrix_and_files(self):
        factory = fixtures.EngineThreadTests()
        factory.setUp()
        models = {}
        for label, original in [('old', factory.originals[0]), ('new', factory.originals[2])]:
            pe, _, cls = factory.fixture(original)
            pe._classes = [cls]
            models[label.encode()] = pe
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = []
            for label in ['old', 'new']:
                path = root / (label + '.exe'); path.write_bytes(label.encode()); paths.append((label, path))
            with patch.dict(sys.modules, {'pefile': NS(PE=lambda data: models[data])}), patch('compare_engine.recover', lambda pe: pe._classes):
                result = compare(paths, root / 'out')
            row = result['thread_matrix'][0]['builds']
            self.assertEqual((row['old'][0]['slot'], row['old'][0]['member_offset']), (13, 0x68))
            self.assertEqual((row['new'][0]['slot'], row['new'][0]['member_offset']), (12, 0xe0))
            self.assertFalse(result['runtime_support_inferred'])
            for label, path in paths:
                self.assertEqual(path.read_bytes(), label.encode())
                report = json.loads((root / 'out' / (label + '-threads.json')).read_text())
                self.assertEqual(report['build_sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
                self.assertFalse(report['runtime_profile_accepted'])

    def test_every_output_suffix_is_checked_before_reading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            other = root / 'other.exe'; other.write_bytes(b'other')
            for name in ['comparison.json', 'one-rtti.json', 'one-getters.json', 'one-threads.json']:
                source = root / name; source.write_bytes(b'preserved original')
                with patch.dict(sys.modules, {'pefile': NS()}):
                    with self.assertRaisesRegex(ValueError, 'overwrite'):
                        compare([('one', source), ('two', other)], root)
                self.assertEqual(source.read_bytes(), b'preserved original')

    def test_invalid_labels_and_later_parse_failure_publish_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); output = root / 'out'
            a, b = root / 'a.exe', root / 'b.exe'
            a.write_bytes(b'a'); b.write_bytes(b'b')
            for labels in [['One', 'one'], ['../one', 'two']]:
                with patch.dict(sys.modules, {'pefile': NS()}), self.assertRaises(ValueError):
                    compare(list(zip(labels, [a, b])), output)
                self.assertFalse(output.exists())
            pe = NS(FILE_HEADER=NS(Machine=0x8664),
                    OPTIONAL_HEADER=NS(ImageBase=0x140000000,Magic=0x20b),
                    sections=[],get_memory_mapped_image=lambda: bytes(64),_classes=[])
            def load(data):
                if data == b'b': raise ValueError('invalid second original')
                return pe
            with patch.dict(sys.modules, {'pefile': NS(PE=load)}), patch('compare_engine.recover', lambda pe: pe._classes):
                with self.assertRaisesRegex(ValueError, 'second'):
                    compare([('one', a), ('two', b)], output)
            self.assertFalse(output.exists())
            self.assertEqual((a.read_bytes(), b.read_bytes()), (b'a', b'b'))


if __name__ == '__main__':
    unittest.main()
