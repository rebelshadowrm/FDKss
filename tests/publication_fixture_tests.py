import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from publication_fixtures import private_evidence, verify_local_evidence


class EvidencePolicyTests(unittest.TestCase):
    def test_default_skip_is_explicit_and_opt_in_requires_verification(self):
        @private_evidence
        def operation():
            return 'checked operation'
        with patch.dict(os.environ,{'FDKSS_PRIVATE_EVIDENCE':'0'}):
            with self.assertRaises(unittest.SkipTest):
                operation()
        with patch.dict(os.environ,{'FDKSS_PRIVATE_EVIDENCE':'1'}):
            with patch('publication_fixtures.verify_local_evidence',side_effect=AssertionError('missing')):
                with self.assertRaises(AssertionError):
                    operation()
            with patch('publication_fixtures.verify_local_evidence') as verify:
                self.assertEqual(operation(),'checked operation')
                verify.assert_called_once()

    def test_missing_and_changed_evidence_never_pass_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'fixtures').mkdir()
            path=root/'fixtures/original.json';path.write_bytes(b'original fixture')
            (root/'private_evidence_manifest.json').write_text(json.dumps({'files':{'original.json':hashlib.sha256(path.read_bytes()).hexdigest()}}))
            verify_local_evidence(root)
            path.write_bytes(b'changed')
            with self.assertRaises(AssertionError):verify_local_evidence(root)
            path.unlink()
            with self.assertRaises(AssertionError):verify_local_evidence(root)


if __name__=='__main__':
    unittest.main()
