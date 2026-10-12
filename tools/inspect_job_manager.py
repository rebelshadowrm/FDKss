"""Read-only, fingerprinted global JobManager diagnostics for the two Ys builds.

Copies scalar members only; never locks/calls the manager or follows job/Actor
pointers. Samples are non-atomic and do not approve ownership or safe handles.
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct

PROFILES = {
    'ysx_pn.exe': dict(sha256='34badc3071d5c571524a2234ad64a4c4251214de578eaa18c04b96432501765e',
                       image_size=0xf06000, root=0xe26968, table=0xb72508, extent=0x150,
                       worker=0x7a4c30, fixture='engine_global_jobs_pn.json'),
    'ysx.exe': dict(sha256='224df0db3e0867cc83d4f71b2e3e78f71f47c430899c76b9beb12da5403ae246',
                    image_size=0xdb0000, root=0xd0bbf8, table=0xa89240, extent=0x150,
                    worker=0x6f2e40, fixture='engine_global_jobs_ysx.json'),
}


def select_profile(identity):
    profile = PROFILES.get(Path(identity['path']).name.lower())
    if profile is None or identity.get('sha256') != profile['sha256']:
        raise ValueError('no pinned global-manager diagnostic profile')
    for key, limit in [('pid',0xffffffff), ('creation_filetime',0xffffffffffffffff),
                       ('image_base',0x7fffffffffff-profile['image_size'])]:
        if type(identity.get(key)) is not int or not 0 < identity[key] <= limit:
            raise ValueError('fresh bounded process identity required')
    return profile


def worker_original(fixture, profile):
    if fixture.get('executable_sha256') != profile['sha256']:
        raise ValueError('worker fixture build differs from diagnostic profile')
    rows = [r for r in fixture['fixtures'] if r['entry']==profile['worker']]
    if len(rows)!=1 or not rows[0]['function']['walk']['complete_static_walk']:
        raise ValueError('complete original worker evidence required')
    fn = rows[0]['function']
    code = bytes.fromhex(fn['code_hex'])
    walk = fn['walk']
    if int(walk['function_rva'],16)!=profile['worker'] or int(walk['end_rva'],16)!=profile['worker']+len(code) or hashlib.sha256(code).hexdigest()!=walk['body_sha256']:
        raise ValueError('original worker bytes disagree with identity')
    return code


def sample_manager(read, base, profile):
    root = base+profile['root']
    manager = struct.unpack('<Q',read(root,8))[0]
    if not 0 < manager <= 0x7fffffffffff-profile['extent']:
        raise ValueError('global manager absent or outside user address range')
    raw = read(manager,profile['extent'])
    if len(raw)!=profile['extent'] or struct.unpack_from('<Q',raw)[0]!=base+profile['table']:
        raise ValueError('global manager table/extent does not match profile')
    if struct.unpack('<Q',read(root,8))[0]!=manager or struct.unpack('<Q',read(manager,8))[0]!=base+profile['table']:
        raise ValueError('global manager changed across non-atomic sample')
    return dict(manager=manager, table=base+profile['table'],
                raw_members={'stop_byte_08':raw[8], 'worker_count_18':struct.unpack_from('<Q',raw,0x18)[0],
                             'free_item_count_48':struct.unpack_from('<Q',raw,0x48)[0],
                             'pending_group_count_60':struct.unpack_from('<Q',raw,0x60)[0],
                             'worker_mask_90':struct.unpack_from('<I',raw,0x90)[0]},
                non_atomic=True, root_and_table_rechecked=True,
                lifetime_verified=False, same_address_reuse_excluded=False,
                phase_synchronization_verified=False, runtime_profile_accepted=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('identity',type=Path,help='fresh PID/creation_filetime/path/hash/image_base JSON')
    parser.add_argument('--fixture',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    identity = json.loads(args.identity.read_text(encoding='utf-8-sig'))
    profile = select_profile(identity)
    executable = Path(identity['path']).resolve()
    if args.output.resolve().is_relative_to(executable.parent):
        raise ValueError('diagnostic output must stay outside the game installation')
    fixture_path = args.fixture or Path(__file__).resolve().parents[1]/'tests/fixtures'/profile['fixture']
    from verify_framework_restoration import protect_output, require_original
    protect_output(args.output.resolve(),[args.identity,executable,fixture_path])
    data = executable.read_bytes()
    if hashlib.sha256(data).hexdigest()!=profile['sha256']:
        raise ValueError('executable changed since identity observation')
    import pefile
    pe = pefile.PE(data=data)
    if (pe.FILE_HEADER.Machine,pe.OPTIONAL_HEADER.Magic,pe.OPTIONAL_HEADER.SizeOfImage)!=(0x8664,0x20b,profile['image_size']):
        raise ValueError('PE image layout changed')
    original = worker_original(json.loads(fixture_path.read_text(encoding='utf-8')),profile)
    require_original(pe.get_memory_mapped_image()[profile['worker']:profile['worker']+len(original)],original)
    # Raw kernel primitives only; the existing Sky Process guard is untouched.
    from windows_process import kernel, Module
    from ctypes import wintypes as W
    handle = kernel.OpenProcess(0x410,False,identity['pid'])
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        path = C.create_unicode_buffer(32768); length = W.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(handle,0,path,C.byref(length)) or Path(path.value).resolve()!=executable:
            raise ValueError('process executable identity changed')
        kernel.GetProcessTimes.argtypes = [W.HANDLE]+[C.POINTER(C.c_uint64)]*4
        times = [C.c_uint64() for _ in range(4)]
        if not kernel.GetProcessTimes(handle,*[C.byref(v) for v in times]) or times[0].value!=identity['creation_filetime']:
            raise ValueError('process creation changed')
        snap = kernel.CreateToolhelp32Snapshot(0x18,identity['pid'])
        if snap == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        modules = []
        try:
            item = Module(); item.size = C.sizeof(item)
            valid = kernel.Module32FirstW(snap,C.byref(item))
            while valid:
                modules.append(dict(name=item.name,path=item.path,base=item.base,size=item.image_size))
                valid = kernel.Module32NextW(snap,C.byref(item))
        finally:
            kernel.CloseHandle(snap)
        protect_output(args.output.resolve(),[m['path'] for m in modules])
        images = [m for m in modules if Path(m['path']).resolve()==executable]
        if len(images)!=1 or (images[0]['base'],images[0]['size'])!=(identity['image_base'],profile['image_size']):
            raise ValueError('process image changed')
        def read(address,size):
            if not 0 < size <= 4096 or not 0 < address <= 0x7fffffffffff-size:
                raise ValueError('bounded diagnostic read required')
            value = C.create_string_buffer(size); copied = C.c_size_t()
            if not kernel.ReadProcessMemory(handle,address,value,size,C.byref(copied)) or copied.value!=size:
                raise OSError('diagnostic memory unreadable')
            return value.raw
        base = identity['image_base']
        require_original(read(base+profile['worker'],len(original)),original)
        result = sample_manager(read,base,profile)
        result.update(identity=identity, observed_utc=datetime.now(timezone.utc).isoformat(),
                      original_worker_matches=True, modules=modules,
                      fdkss_modules=[m | dict(sha256=hashlib.sha256(Path(m['path']).read_bytes()).hexdigest())
                                     for m in modules if m['name'].lower()=='fdkss.dll'],
                      limitations='Scalar diagnostic only; no locks, native calls, phase synchronization, job-pointer traversal or Actor access. Non-atomic reads do not prove lifetime or ABA safety.')
        args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(result['raw_members']))
    finally:
        kernel.CloseHandle(handle)


if __name__=='__main__':
    main()
