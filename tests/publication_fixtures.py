"""Keep local original-byte checks distinct from public synthetic tests."""
from functools import wraps
import hashlib
import json
import os
from pathlib import Path
import unittest


def private_evidence(function):
    @wraps(function)
    def checked(*args, **kwargs):
        if os.environ.get('FDKSS_PRIVATE_EVIDENCE')!='1':
            raise unittest.SkipTest('Private original-byte check; enable FDKSS_PRIVATE_EVIDENCE=1 with pinned local evidence')
        verify_local_evidence(Path(__file__).parent)
        return function(*args, **kwargs)
    return checked


def verify_local_evidence(root):
    manifest=json.loads((root/'private_evidence_manifest.json').read_text(encoding='utf-8'))
    for name,digest in manifest['files'].items():
        path=root/'fixtures'/name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise AssertionError('Missing or changed private evidence: '+name)
