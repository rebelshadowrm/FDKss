"""Offline battle-manager singleton candidates; never authorizes live calls."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from dump_sdk import recover

MANAGER = '.?AVManager@battle@sora@@'
PREFIX = bytes.fromhex('48895c24104889742418574883ec2048894908488d05000000004889010f57c0')
LIST_INIT = bytes.fromhex('488d871822000089b03802000048898030020000')


def scan(image, executable, writable, functions, table):
    """Match the observed complete constructor prefix and publication encoding.

    The list initialization sets inline storage at owner+2218, pointer at
    owner+2448 and count at owner+2450. A byte match is discovery evidence,
    not a proof that every caller is reachable or that the list owns FDK Actors.
    """
    if any(not 0 <= a < b <= len(image) for a, b in executable + writable + functions):
        raise ValueError('section/function bounds leave the mapped image')
    contains = lambda ranges, at, size: any(a <= at and at + size <= b for a, b in ranges)
    if table % 8 or not 8 <= table <= len(image) - 8:
        return []
    constructors = []
    for start, end in functions:
        if not contains(executable, start, end - start) or end - start < len(PREFIX):
            continue
        body = image[start:start + len(PREFIX)]
        if body[:22] != PREFIX[:22] or body[26:] != PREFIX[26:]:
            continue
        if start + 26 + struct.unpack_from('<i', body, 22)[0] != table:
            continue
        init = image.find(LIST_INIT, start + len(PREFIX), end)
        if init < 0:
            continue
        constructors.append((start, end, init))
    if len(constructors) != 1:
        return []
    constructor, constructor_end, init = constructors[0]
    candidates = []
    for start, end in functions:
        if not contains(executable, start, end - start):
            continue
        cursor = start
        while (at := image.find(b'\x48\x8b\xcb\xe8', cursor, end)) >= 0:
            cursor = at + 1
            if at + 20 > end:
                continue
            body = image[at:at + 20]
            if at + 8 + struct.unpack_from('<i', body, 4)[0] != constructor:
                continue
            # Non-null constructor result branches over mov rax,rdi/rsi.
            if body[8:12] != b'\xeb\x03\x48\x8b' or body[12] not in (0xc6, 0xc7):
                continue
            if body[13:16] != b'\x48\x89\x05':
                continue
            root = at + 20 + struct.unpack_from('<i', body, 16)[0]
            if root % 8 or not contains(writable, root, 8):
                continue
            candidates.append(dict(global_rva=hex(root), constructor_rva=hex(constructor),
                constructor_end_rva=hex(constructor_end), constructor_prefix_hex=image[constructor:constructor + len(PREFIX)].hex(),
                publication_rva=hex(at), publication_hex=body.hex(), publication_function_rva=hex(start),
                list_initialization_rva=hex(init), list_initialization_hex=LIST_INIT.hex(),
                inline_storage_offset=0x2218, pointer_offset=0x2448, count_offset=0x2450,
                count_storage='32-bit; signed exit-loop bounds observed separately',
                callable_verified=False, lifetime_verified=False, runtime_adapter_approved=False))
    # Ambiguous publications cannot identify one current singleton root.
    return candidates if len(candidates) == 1 else []


def discover(pe):
    image = pe.get_memory_mapped_image()
    classes = [c for c in recover(pe) if c['name'] == MANAGER]
    tables = [t for c in classes for t in c['vtables']
              if t['subobject_offset'] == t['constructor_displacement'] == 0]
    result = dict(candidates=[], limitations=[
        'Observed Sky constructor/publication encodings only; misses do not prove absence.',
        'Offline byte matches do not approve calls, live object reads or persistent handles.',
        'Battle entries are not assumed to belong to the reflected FDK Actor hierarchy.'])
    if len(classes) != 1 or len(tables) != 1 or pe.FILE_HEADER.Machine != 0x8664:
        return result
    ranges = lambda flag: [(s.VirtualAddress, min(len(image), s.VirtualAddress + s.Misc_VirtualSize))
                           for s in pe.sections if s.Characteristics & flag and s.Misc_VirtualSize]
    functions = [(x.struct.BeginAddress, x.struct.EndAddress)
                 for x in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', [])]
    result['candidates'] = scan(image, ranges(0x20000000), ranges(0x80000000), functions, int(tables[0]['rva'], 16))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    import pefile
    data = args.executable.read_bytes()
    result = discover(pefile.PE(data=data))
    result.update(executable=str(args.executable.resolve()), sha256=hashlib.sha256(data).hexdigest())
    encoded = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded, encoding='utf-8')
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
