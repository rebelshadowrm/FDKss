"""Discover bounded leaf getter descriptions through MSVC RTTI, without fixed slots.

Descriptions express instruction effects, not field names or callable profiles.
No process is opened and no recovered native code is executed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from dump_sdk import recover


def discover(pe, classes=None, base_class='.?AVActor@fdk@@'):
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20b:
        raise ValueError('engine getter discovery requires AMD64 PE32+')
    image = pe.get_memory_mapped_image()
    if not 1 <= len(image) <= 128 * 1024 * 1024:
        raise ValueError('mapped image exceeds the discovery bound')
    executable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]
    readable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                for s in pe.sections if s.Characteristics & 0x40000000]
    if any(not 0 <= a <= b <= len(image) for a, b in executable + readable):
        raise ValueError('section leaves mapped image')

    def contains(ranges, at, size):
        return size > 0 and any(a <= at and at + size <= b for a, b in ranges)

    def code(at, size):
        return image[at:at + size] if contains(executable, at, size) else b''

    def leaf(at):
        prefix = code(at, 3)
        if prefix in (b'\x33\xc0\xc3', b'\x31\xc0\xc3'):
            body = prefix
            row = dict(kind='constant_u32', value=0)
        elif prefix[:1] == b'\xb8' and len(body := code(at, 6)) == 6 and body[-1] == 0xc3:
            row = dict(kind='constant_u32', value=struct.unpack_from('<I', body, 1)[0])
        elif prefix == b'\x48\x8d\x41' and len(body := code(at, 5)) == 5 and body[-1] == 0xc3:
            row = dict(kind='member_address', object_offset=struct.unpack_from('<b', body, 3)[0])
        elif prefix == b'\x48\x8d\x81' and len(body := code(at, 8)) == 8 and body[-1] == 0xc3:
            row = dict(kind='member_address', object_offset=struct.unpack_from('<i', body, 3)[0])
        elif prefix == b'\x48\x8d\x05' and len(body := code(at, 8)) == 8 and body[-1] == 0xc3:
            target = at + 7 + struct.unpack_from('<i', body, 3)[0]
            end = image.find(b'\0', max(0, target), min(len(image), target + 256)) if target >= 0 else -1
            if end <= target or not contains(readable, target, end - target + 1):
                return None
            text = image[target:end]
            if any(ch < 32 or ch > 126 for ch in text):
                return None
            row = dict(kind='constant_ascii_address', string_rva=hex(target), value=text.decode('ascii'),
                       string_sha256=hashlib.sha256(text + b'\0').hexdigest())
        else:
            return None
        if row['kind'] == 'member_address' and not 8 <= row['object_offset'] <= 0x100000:
            return None
        row.update(handler_rva=hex(at), body_size=len(body), code_hex=body.hex(),
                   body_sha256=hashlib.sha256(body).hexdigest())
        return row

    classes = recover(pe) if classes is None else classes
    if len(classes) > 16384:
        raise ValueError('RTTI class count exceeds discovery bound')
    names = Counter(c['name'] for c in classes)
    candidates, rejected = [], []
    for cls in classes:
        if not any(b['class'] == base_class and b['member_displacement'] == 0 and
                   b['vbtable_displacement'] == -1 for b in cls['bases']):
            continue
        try:
            if names[cls['name']] != 1:
                raise ValueError('RTTI class name is ambiguous')
            tables = [t for t in cls['vtables'] if t['subobject_offset'] == t['constructor_displacement'] == 0]
            if len(tables) != 1:
                raise ValueError('primary vtable is absent or ambiguous')
            table = tables[0]
            table_rva = int(table['rva'], 16)
            methods = table['methods']
            slots = [m['slot'] for m in methods]
            if not slots or len(slots) > 512 or len(slots) != len(set(slots)) or any(type(s) is not int or not 0 <= s < 512 for s in slots):
                raise ValueError('vtable slots are empty, duplicated or out of bounds')
            if table_rva < 8 or table_rva % 8:
                raise ValueError('vtable is unaligned or outside the image')
            getters = []
            for method in methods:
                slot = method['slot']; at = int(method['rva'], 16); word = table_rva + slot * 8
                if not contains(readable, word, 8) or struct.unpack_from('<Q', image, word)[0] != pe.OPTIONAL_HEADER.ImageBase + at:
                    raise ValueError('vtable metadata disagrees with original pointer words')
                if not contains(executable, at, 1):
                    raise ValueError('virtual entry is outside executable sections')
                getter = leaf(at)
                if getter is not None:
                    getter['slot'] = slot
                    getters.append(getter)
            candidates.append(dict(class_name=cls['name'], vtable_rva=hex(table_rva),
                                   getters=sorted(getters, key=lambda g: g['slot']),
                                   scanned_slots=len(methods),
                                   table_boundary_verified=False, runtime_profile_accepted=False))
        except (ValueError, struct.error) as error:
            rejected.append(dict(class_name=cls['name'], reason=str(error)))
    return dict(format='fdkss.engine_getter_candidates', version=1, base_class=base_class,
                candidates=sorted(candidates, key=lambda c: c['class_name']), rejected=rejected,
                limitations=[
                    'Exact leaf instruction effects only; no getter names, field types/extents or gameplay semantics are inferred.',
                    'RTTI tables use a bounded executable-pointer scan; complete virtual table boundaries are unverified.',
                    'Member-address results do not establish matrix layout, receiver lifetime, ownership or thread safety.',
                    'No runtime attachment, callable ABI or support for an additional build is approved.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--base-class', default='.?AVActor@fdk@@')
    args = parser.parse_args()
    if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('output cannot overwrite executable')
    data = args.executable.read_bytes()
    try:
        report = discover(pefile.PE(data=data), base_class=args.base_class)
    except (ValueError, pefile.PEFormatError) as error:
        parser.error(str(error))
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{len(report['candidates'])} class descriptions; {len(report['rejected'])} rejected")


if __name__ == '__main__':
    main()
