from publication_fixtures import private_evidence
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from verify_framework_restoration import original_body, contract, require_manifest, require_original, protect_output, PN_HASH, select_profile, PROFILES


class RestorationTests(unittest.TestCase):
    @private_evidence
    def test_original_ys_profile_is_independent_and_cannot_cross_match(self):
        identity = dict(pid=1, creation_filetime=2, path=str(ROOT / 'ysx.exe'),
                        sha256=PROFILES['ysx.exe']['sha256'])
        profile = select_profile(identity)
        body = original_body(self.header, profile)
        rows = json.loads((ROOT / 'tests/fixtures/engine_call_originals.json').read_text())['fixtures']
        fixture = next(row for row in rows if row['game'] == 'ysx' and row['begin'] == 0x6a8cf0)
        self.assertEqual(body, bytes.fromhex(fixture['code_hex']))
        expected = contract(identity, 0x140000000, body)
        self.assertEqual(expected['frame_rva'], 0x6a8cf0)
        self.assertEqual(expected['body_bytes'], 1358)
        for offset in range(len(body)):
            changed = bytearray(body); changed[offset] ^= 1
            with self.assertRaises(ValueError):
                require_original(changed, body)
        with self.assertRaises(ValueError):
            require_manifest(self.expected, expected)
        for path, digest in (('ysx.exe', PN_HASH), ('ysx_pn.exe', identity['sha256']),
                             ('kuro.exe', identity['sha256'])):
            with self.assertRaises(ValueError):
                select_profile(identity | dict(path=path, sha256=digest))

    def test_output_cannot_overwrite_input_through_a_hardlink(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'; source.write_bytes(b'preserved')
            alias = Path(directory) / 'alias'; alias.hardlink_to(source)
            for destination in (source, alias):
                with self.assertRaises(ValueError):
                    protect_output(destination, [source])
            protect_output(Path(directory) / 'new-evidence', [source])
            self.assertEqual(source.read_bytes(), b'preserved')

    def setUp(self):
        self.header = (ROOT / 'src/windows/framework_profiles.h').read_text()
        self.body = original_body(self.header)
        self.identity = dict(pid=1, creation_filetime=2, path=str(ROOT / 'ysx_pn.exe'), sha256=PN_HASH)
        self.expected = contract(self.identity, 0x140000000, self.body)

    @private_evidence
    def test_independent_original_and_every_byte(self):
        rows = json.loads((ROOT / 'tests/fixtures/engine_call_originals.json').read_text())['fixtures']
        fixture = next(row for row in rows if row['game'] == 'ysxpn' and row['begin'] == 0x761520)
        self.assertEqual(self.body, bytes.fromhex(fixture['code_hex']))
        self.assertEqual(hashlib.sha256(self.body).hexdigest(), fixture['body_sha256'])
        require_original(self.body, self.body)
        for offset in range(len(self.body)):
            changed = bytearray(self.body); changed[offset] ^= 1
            with self.assertRaises(ValueError):
                require_original(changed, self.body)
        with self.assertRaises(ValueError):
            require_original(self.body[:-1], self.body)

    def test_manifest_contract_cannot_supply_alternate_addresses_or_body(self):
        require_manifest(self.expected | {'utc': 'observation'}, self.expected)
        for key, value in self.expected.items():
            changed = self.expected.copy(); changed[key] = 'changed'
            with self.assertRaises(ValueError):
                require_manifest(changed, self.expected)
            changed.pop(key)
            with self.assertRaises(ValueError):
                require_manifest(changed, self.expected)

    def test_damaged_or_unbounded_header_rejected(self):
        for changed in (self.header.replace('0x40,', '0x41,', 1),
                        self.header.replace('0x40,', '0x400,', 1),
                        self.header.replace('0x40,', 'arbitrary,', 1),
                        self.header.replace('pn_frame_body', 'other_body')):
            with self.assertRaises(ValueError):
                original_body(changed)


if __name__ == '__main__':
    unittest.main()
