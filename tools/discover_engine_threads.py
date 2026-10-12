"""Describe exact Application thread-ID predicates in original FDK executables.

Instruction effects only: a native predicate accepting a thread does not prove
that arbitrary Actor reads are safe on that thread. No native code is executed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from dump_sdk import recover
from discover_engine_getters import discover as validate_family


def describe(image, executable, readable, at, thread_id_imports):
    def contains(ranges, address, size):
        return 0 <= address and address + size <= len(image) and any(a <= address and address + size <= b for a, b in ranges)
    if not contains(executable, at, 24):
        return None
    body = image[at:at + 52]
    if body[:6] != b'\x48\x83\xec\x28\xff\x15' or body[10:12] != b'\x3b\x05':
        return None
    api = at + 10 + struct.unpack_from('<i', body, 6)[0]
    global_id = at + 16 + struct.unpack_from('<i', body, 12)[0]
    if api not in thread_id_imports or not contains(readable, api, 8) or not contains(readable, global_id, 4):
        return None
    return_false = b'\x0f\x94\xc0\x48\x83\xc4\x28\xc3'
    return_true = b'\xb0\x01\x48\x83\xc4\x28\xc3'
    if body[16:24] == return_false:
        size = 24
        row = dict(kind='current_thread_matches_global', thread_id_global_rva=hex(global_id))
    elif body[16:17] == b'\x74' and body[18:20] == b'\xff\x15' and body[24:27] == b'\x48\x8b\x0d':
        if len(body) < 49 or at + 24 + struct.unpack_from('<i', body, 20)[0] != api:
            return None
        root = at + 31 + struct.unpack_from('<i', body, 27)[0]
        if body[31:33] == b'\x3b\x41':
            member, tail = struct.unpack_from('<b', body, 33)[0], 34
        elif len(body) == 52 and body[31:33] == b'\x3b\x81':
            member, tail = struct.unpack_from('<i', body, 33)[0], 37
        else:
            return None
        size = tail + 15
        if not 4 <= member <= 0x100000 or not contains(readable, root, 8) or body[17] != tail + 8 - 18:
            return None
        if body[tail:tail + 8] != return_false or body[tail + 8:size] != return_true:
            return None
        row = dict(kind='current_thread_matches_global_or_indirect_member',
                   thread_id_global_rva=hex(global_id), pointer_global_rva=hex(root), member_offset=member)
    else:
        return None
    if not contains(executable, at, size):
        return None
    row.update(handler_rva=hex(at), body_size=size, code_hex=body[:size].hex(),
               body_sha256=hashlib.sha256(body[:size]).hexdigest(),
               get_current_thread_id_iat_rva=hex(api), runtime_profile_accepted=False)
    return row


def discover(pe, classes=None, base_class='.?AVApplication@fdk@@'):
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20b:
        raise ValueError('engine thread discovery requires AMD64 PE32+')
    classes = recover(pe) if classes is None else classes
    # Reuse architecture, section, inheritance, ambiguity, slot and complete
    # original-pointer-word checks. No fixed Application slots are assumed.
    family = validate_family(pe, classes, base_class)
    image = pe.get_memory_mapped_image()
    executable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]
    readable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                for s in pe.sections if s.Characteristics & 0x40000000]
    imports = set()
    for directory in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        if directory.dll.lower() not in (b'kernel32.dll', b'kernelbase.dll'):
            continue
        for entry in directory.imports:
            if entry.name == b'GetCurrentThreadId':
                imports.add(entry.address - pe.OPTIONAL_HEADER.ImageBase)
    by_name = {c['name']: c for c in classes}
    candidates = []
    for validated in family['candidates']:
        cls = by_name[validated['class_name']]
        table = next(t for t in cls['vtables'] if int(t['rva'], 16) == int(validated['vtable_rva'], 16))
        for method in table['methods']:
            row = describe(image, executable, readable, int(method['rva'], 16), imports)
            if row:
                row.update(class_name=cls['name'], slot=method['slot'], vtable_rva=validated['vtable_rva'])
                candidates.append(row)
    return dict(format='fdkss.engine_thread_predicates', version=1, base_class=base_class,
                candidates=candidates, rejected=family['rejected'], runtime_profile_accepted=False,
                limitations=[
                    'Exact complete instruction templates and named original imports only.',
                    'Global/member names, pointee types and native lifecycle/scheduling remain unverified.',
                    'Thread membership does not establish Actor synchronization, safe field access or adapter readiness.',
                    'Bounded RTTI virtual-table extents are not verified; no callable ABI or new runtime support is approved.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--base-class', default='.?AVApplication@fdk@@')
    args = parser.parse_args()
    if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('output cannot overwrite executable')
    data = args.executable.read_bytes()
    report = discover(pefile.PE(data=data), base_class=args.base_class)
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{len(report['candidates'])} predicates; {len(report['rejected'])} rejected class descriptions")


if __name__ == '__main__':
    main()
