"""Read-only, pinned Framework frame baseline/restoration verification.

This verifies the core bridge's complete original frame body, not Actor lifetime,
world simulation or arbitrary mod patches. It does not extend Sky Process access.
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct

PN_HASH = '34badc3071d5c571524a2234ad64a4c4251214de578eaa18c04b96432501765e'
BODY_HASH = 'f1529e1c8e4c6bb3344f90069fa3a07e782eb9cdabac1b19150863d633de750e'
FRAME_RVA, ROOT_RVA, TABLE_RVA, OWNER_RVA = 0x761520, 0xe26950, 0xb70230, 0xe260f8

PROFILES = {
    'ysx_pn.exe': dict(sha256=PN_HASH, body_hash=BODY_HASH, body_name='pn_frame_body',
                       body_bytes=971, image_size=0xf06000, frame=FRAME_RVA,
                       root=ROOT_RVA, table=TABLE_RVA, owner=OWNER_RVA),
    'ysx.exe': dict(sha256='224df0db3e0867cc83d4f71b2e3e78f71f47c430899c76b9beb12da5403ae246',
                    body_hash='ed5e2aaf085eb844ae92336a70c4625d610661b40347ab75ea7d7d7d418ff487',
                    body_name='ysx_frame_body', body_bytes=1358, image_size=0xdb0000,
                    frame=0x6a8cf0, root=0xd0bb90, table=0xa86988, owner=0xd0b658),
}


def select_profile(identity):
    profile = PROFILES.get(Path(identity['path']).name.lower())
    if profile is None or identity['sha256'] != profile['sha256']:
        raise ValueError('Identity has no accepted Framework frame profile')
    return profile


def original_body(header, profile=None):
    profile = PROFILES["ysx_pn.exe"] if profile is None else profile
    match = re.search(re.escape(profile['body_name']) + r'\s*\{([^}]+)\}', header)
    if not match or re.sub(r'0x[0-9a-fA-F]{2}|[\s,]', '', match[1]):
        raise ValueError('Invalid complete frame body evidence')
    body = bytes(int(word, 16) for word in re.findall(r'0x([0-9a-fA-F]{2})', match[1]))
    if len(body) != profile['body_bytes'] or hashlib.sha256(body).hexdigest() != profile['body_hash']:
        raise ValueError('Frame body evidence differs from independently recorded original')
    return body


def contract(identity, base, body):
    profile = select_profile(identity)
    return dict(schema_version=1, scope='framework-frame-original-only',
                pid=identity['pid'], creation_filetime=identity['creation_filetime'],
                path=str(Path(identity['path']).resolve()), sha256=profile['sha256'], image_base=base,
                frame_rva=profile['frame'], body_bytes=len(body), original_sha256=profile['body_hash'],
                original_hex=body.hex())


def require_manifest(manifest, expected):
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise ValueError('Manifest identity or complete original contract changed')


def require_original(observed, expected):
    if observed != expected:
        raise ValueError('Complete original frame body is not restored')


def protect_output(destination, inputs):
    for source in inputs:
        source = Path(source).resolve()
        if destination == source or destination.exists() and source.exists() and destination.samefile(source):
            raise ValueError('Evidence output collides with an input or loaded module')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('identity', help='fresh JSON identity including PID, creation_filetime, path, sha256 and image_base')
    parser.add_argument('manifest', help='baseline output, or existing baseline with --verify')
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    identity_path, manifest_path = Path(args.identity).resolve(), Path(args.manifest).resolve()
    destination = Path(str(manifest_path) + '.verified.json') if args.verify else manifest_path
    identity = json.loads(identity_path.read_text(encoding='utf-8-sig'))
    executable = Path(identity['path']).resolve()
    profile = select_profile(identity)
    protect_output(destination, [identity_path, executable] + ([manifest_path] if args.verify else []))
    if not type(identity['pid']) is int or not 0 < identity['pid'] <= 0xffffffff:
        raise ValueError('Invalid PID')
    if not type(identity['creation_filetime']) is int or not 0 < identity['creation_filetime'] <= 0xffffffffffffffff:
        raise ValueError('Fresh creation_filetime is required')
    if not type(identity['image_base']) is int or not 0 < identity['image_base'] <= 0x7fffffffffff - profile['image_size']:
        raise ValueError('Fresh bounded image_base is required')
    data = executable.read_bytes()
    if hashlib.sha256(data).hexdigest() != profile['sha256']:
        raise ValueError('Unknown executable build')
    local_header = Path(__file__).with_name('framework_profiles.h')
    header = local_header if local_header.exists() else Path(__file__).resolve().parents[1] / 'src/windows/framework_profiles.h'
    protect_output(destination, [header])
    body = original_body(header.read_text(encoding='utf-8'), profile)
    import pefile
    pe = pefile.PE(data=data)
    if (pe.FILE_HEADER.Machine, pe.OPTIONAL_HEADER.Magic, pe.OPTIONAL_HEADER.SizeOfImage) != (0x8664, 0x20b, profile['image_size']):
        raise ValueError('Image layout differs from frame contract')
    require_original(pe.get_memory_mapped_image()[profile['frame']:profile['frame'] + len(body)], body)
    # Use only the scoped raw read helpers; Sky's Process guard remains unchanged.
    from windows_process import kernel, Module
    from ctypes import wintypes as W
    handle = kernel.OpenProcess(0x410, False, identity['pid'])
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        path = C.create_unicode_buffer(32768)
        length = W.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, C.byref(length)) or Path(path.value).resolve() != executable:
            raise ValueError('Process executable identity changed')
        kernel.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(C.c_uint64)] * 4
        times = [C.c_uint64() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *[C.byref(value) for value in times]) or times[0].value != identity['creation_filetime']:
            raise ValueError('Process creation changed')
        snapshot = kernel.CreateToolhelp32Snapshot(0x18, identity['pid'])
        if snapshot == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        modules = []
        try:
            entry = Module(); entry.size = C.sizeof(entry)
            valid = kernel.Module32FirstW(snapshot, C.byref(entry))
            while valid:
                modules.append(dict(name=entry.name, path=entry.path, base=entry.base, size=entry.image_size))
                valid = kernel.Module32NextW(snapshot, C.byref(entry))
        finally:
            kernel.CloseHandle(snapshot)
        protect_output(destination, [module['path'] for module in modules])
        images = [module for module in modules if Path(module['path']).resolve() == executable]
        if len(images) != 1 or images[0]['base'] != identity['image_base'] or images[0]['size'] != profile['image_size']:
            raise ValueError('Process image identity changed')
        base = images[0]['base']
        def read(address, size):
            if not 0 < size <= 4096 or not 0 < address <= 0x7fffffffffff - size:
                raise ValueError('Invalid bounded frame read')
            value = C.create_string_buffer(size); copied = C.c_size_t()
            if not kernel.ReadProcessMemory(handle, address, value, size, C.byref(copied)) or copied.value != size:
                raise OSError('Frame contract memory is unreadable')
            return value.raw
        expected = contract(identity, base, body)
        if args.verify:
            require_manifest(json.loads(manifest_path.read_text(encoding='utf-8')), expected)
        require_original(read(base + profile['frame'], len(body)), body)
        root = struct.unpack('<Q', read(base + profile['root'], 8))[0]
        owner = struct.unpack('<I', read(base + profile['owner'], 4))[0]
        # Root/table readiness is needed to establish a pre-attachment baseline.
        # Restoration still verifies the frame when a later root is unavailable.
        table = None
        if root:
            try:
                table = struct.unpack('<Q', read(root, 8))[0]
            except OSError:
                if not args.verify:
                    raise
        if not args.verify and (not root or table != base + profile['table'] or not owner):
            raise ValueError('Framework owner/root is not ready for a baseline')
        result = expected | dict(utc=datetime.now(timezone.utc).isoformat(),
                                 verified=args.verify, complete_original_matches=True,
                                 framework=root, framework_table=table, main_thread=owner,
                                 fdkss_modules=[m for m in modules if m['name'].lower() == 'fdkss.dll'])
        destination.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({key: result[key] for key in ('utc', 'pid', 'scope', 'verified', 'body_bytes', 'complete_original_matches')}))
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    main()
