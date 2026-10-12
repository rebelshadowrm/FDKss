import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from peer_runtime_preflight import static_compatible, verify_checkout


class PeerPreflightTests(unittest.TestCase):
    def test_identity_uniqueness_and_optional_logger_scope(self):
        digest='a'*64
        report=dict(sha256=digest.upper(),machine='0x8664',log_hook_required=True,
                    xinput1_4_ordinals=[2,3],patterns={n:[{'rva':'0x100'}] for n in
                    ['initial_file_check','locale_handler','debug_logger']})
        self.assertTrue(static_compatible(report,digest))
        for name in report['patterns']:
            changed=report | dict(patterns=report['patterns'] | {name:[]})
            self.assertFalse(static_compatible(changed,digest))
            changed=report | dict(patterns=report['patterns'] | {name:[{},{}]})
            self.assertFalse(static_compatible(changed,digest))
        self.assertFalse(static_compatible(report | dict(xinput1_4_ordinals=[2]),digest))
        self.assertFalse(static_compatible(report | dict(log_hook_required=False),digest))
        for change in [dict(sha256='b'*64),dict(machine='0x14c')]:
            with self.assertRaises(ValueError):
                static_compatible(report | change,digest)

    def test_checkout_must_not_resolve_to_parent_and_files_must_match(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            def git(*args):
                return subprocess.run(['git','-C',str(root),*args],check=True,capture_output=True,text=True).stdout.strip()
            git('init','--quiet')
            (root/'verifier.py').write_bytes(b'original source')
            git('add','verifier.py')
            git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','--quiet','-m','fixture')
            entry=dict(revision=git('rev-parse','HEAD'),assessed_file_sha256={'verifier.py':hashlib.sha256(b'original source').hexdigest()})
            verify_checkout(root,entry)
            nested=root/'nested';nested.mkdir()
            with self.assertRaisesRegex(ValueError,'own Git checkout'):
                verify_checkout(nested,entry)
            with self.assertRaisesRegex(ValueError,'revision'):
                verify_checkout(root,entry | dict(revision='0'*40))
            with self.assertRaisesRegex(ValueError,'file changed'):
                verify_checkout(root,entry | dict(assessed_file_sha256={'verifier.py':'0'*64}))
            (root/'verifier.py').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'working tree'):
                verify_checkout(root,entry)


if __name__=='__main__':
    unittest.main()
