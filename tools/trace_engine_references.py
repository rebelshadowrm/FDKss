"""Offline selected RIP-relative scalar/vector/address references in x64 walks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from trace_engine_calls import inspect_function
from discover_actor_cleanup_callers import capstone_decoder
from pe_unwind import from_pe

# Deliberately small instruction vocabulary. No claim of all reference coverage.
PREFIX = re.compile(rb'(?:(?:\xf0)?(?:\x66)?[\x40-\x4f]?[\x8b\x89\x8a\x88\x86\x87\x8d\x3b\x39\x80\x81\x83\xc6\xc7]|(?:[\x66\xf2\xf3])?[\x40-\x4f]?\x0f[\x10\x11\x28\x29\x6f\x7f])[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]....', re.S)


def reference(image, at, end):
    """Decode only the explicit vocabulary; caller proves instruction reachability."""
    if not 0 <= at < end <= len(image):
        return None
    cursor, prefix, rex, locked = at, 0, 0, False
    if image[cursor] == 0xf0:
        locked = True; cursor += 1
    if cursor >= end:
        return None
    if image[cursor] in (0x66, 0xf2, 0xf3):
        prefix = image[cursor]; cursor += 1
    if cursor < end and 0x40 <= image[cursor] <= 0x4f:
        rex = image[cursor]; cursor += 1
    if cursor + 6 > end:
        return None
    if image[cursor] == 0x0f:
        if locked:
            return None
        if cursor + 7 > end:
            return None
        opcode, modrm = image[cursor+1:cursor+3]
        if modrm & 0xc7 != 5:
            return None
        if opcode in (0x10, 0x11):
            width = 4 if prefix == 0xf3 else 8 if prefix == 0xf2 else 16
        elif opcode in (0x28, 0x29) and prefix in (0, 0x66):
            width = 16
        elif opcode in (0x6f, 0x7f) and prefix in (0x66, 0xf3):
            width = 16
        else:
            return None
        size = cursor - at + 7
        target = at + size + struct.unpack_from('<i', image, cursor+3)[0]
        return dict(size=size, target_rva=target,
                    operation='write' if opcode in (0x11,0x29,0x7f) else 'read',
                    operand_bytes=width, code_hex=image[at:at+size].hex())
    if prefix not in (0, 0x66):
        return None
    word = prefix == 0x66
    opcode, modrm = image[cursor:cursor + 2]
    if opcode not in (0x8b, 0x89, 0x8a, 0x88, 0x86, 0x87, 0x8d, 0x3b, 0x39, 0x80, 0x81, 0x83, 0xc6, 0xc7) or modrm & 0xc7 != 5:
        return None
    # REX.B/X do not turn RIP-relative ModRM into a base/index register.
    group = (modrm >> 3) & 7
    rmw = opcode in (0x86,0x87) or opcode in (0x80,0x81,0x83) and group in (1,4)
    if locked and not rmw:
        return None
    if opcode in (0x80, 0x81, 0x83) and (group not in (1,4,7) or rex & 4):
        return None
    if opcode in (0xc6, 0xc7) and (group != 0 or rex & 4):
        return None
    width = 8 if rex & 8 else 2 if word else 4
    if opcode in (0x8a, 0x88, 0x86, 0x80, 0xc6):
        width = 1
    immediate = 1 if opcode in (0x80, 0x83, 0xc6) else (2 if width == 2 else 4) if opcode in (0x81, 0xc7) else 0
    size = cursor - at + 6 + immediate
    if at + size > end:
        return None
    target = at + size + struct.unpack_from('<i', image, cursor + 2)[0]
    operation = 'read_write' if rmw else 'address' if opcode == 0x8d else 'write' if opcode in (0x89, 0x88, 0xc6, 0xc7) else 'read'
    return dict(size=size, target_rva=target, operation=operation, operand_bytes=width,
                code_hex=image[at:at + size].hex(),locked=locked,atomic_memory_exchange=opcode in (0x86,0x87))


def trace_references(image, executable, families, targets, decode, watches=()):
    if not 1 <= len(image) <= 128 * 1024 * 1024 or not executable or any(not 0 <= a < b <= len(image) for a, b in executable):
        raise ValueError('Invalid mapped image or executable section bounds')
    if not targets and not watches or len(targets)+len(watches) > 64 or len(set(targets)) != len(targets) or any(type(at) is not int or not 0 <= at < len(image) for at in targets):
        raise ValueError('One to 64 mapped-image reference targets/ranges required')
    if any(type(a) is not int or type(n) is not int or not 0 <= a < a+n <= len(image) for a,n in watches):
        raise ValueError('Watched byte range leaves mapped image')
    wanted, candidates, rejected = set(targets), {}, []
    for start, end in executable:
        cursor = start
        while cursor < end:
            match = PREFIX.search(image, cursor, end)
            if not match:
                break
            at = match.start(); cursor = at + 1  # Preserve overlapping raw candidates.
            effect = reference(image, at, end)
            if not effect or effect['target_rva'] not in wanted and not any(
                    effect['target_rva'] < a+n and a < effect['target_rva']+effect['operand_bytes']
                    for a,n in watches):
                continue
            try:
                a, b = families.contiguous(at)
                if not 0 <= a <= at < at + effect['size'] <= b <= len(image) or not any(x <= a and b <= y for x, y in executable):
                    raise ValueError('Reference family leaves executable bounds')
            except ValueError as error:
                rejected.append(dict(rva=hex(at), reason=str(error))); continue
            candidates.setdefault((a, b), set()).add(at)
    functions = []
    for (a, b), matches in sorted(candidates.items()):
        info = inspect_function(image, executable, families, a, decode)
        instructions = {int(row['rva'], 16): row for row in info['instructions']}
        sites = []
        for at in sorted(matches):
            effect = reference(image, at, b)
            if at in instructions and instructions[at]['size'] == effect['size']:
                sites.append(dict(rva=hex(at), **(effect | {'target_rva': hex(effect['target_rva'])})))
        row = dict(evidence=info, sites=sites, raw_candidates=[hex(at) for at in sorted(matches)])
        if info['walk']['complete_static_walk'] and sites:
            functions.append(row)
        else:
            row['reason'] = 'incomplete static walk' if not info['walk']['complete_static_walk'] else 'no decoded entry-reachable reference'
            rejected.append(row)
    return dict(format='fdkss.engine_references', version=1, targets=[hex(at) for at in targets],
                watches=[dict(rva=hex(a), bytes=n) for a,n in watches],
                functions=functions, rejected=rejected, runtime_profile_accepted=False,
                limitations=['Selected scalar/address and legacy SSE move RIP-relative forms only; no full reference coverage or VEX/EVEX decoding.',
                             'Instruction effects do not establish stored types, pointee extent, receiver ABI, lifetime or ownership.',
                             'Entry-reachable complete static walks only; calls assumed to return; no exceptional edges.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--target', action='append', default=[], type=lambda value: int(value, 0))
    parser.add_argument('--watch', action='append', default=[], help='RVA:BYTES; include selected-form references overlapping this range')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        watches = [tuple(int(v,0) for v in value.split(':')) for value in args.watch]
        if any(len(pair)!=2 for pair in watches) or not args.target and not watches:
            raise ValueError('at least one --target or valid --watch RVA:BYTES required')
    except ValueError as error:
        parser.error(str(error))
    if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('Output must not overwrite executable')
    data = args.executable.read_bytes(); pe = pefile.PE(data=data)
    if (pe.FILE_HEADER.Machine, pe.OPTIONAL_HEADER.Magic) != (0x8664, 0x20b):
        parser.error('AMD64 PE32+ required')
    if not 1 <= pe.OPTIONAL_HEADER.SizeOfImage <= 128 * 1024 * 1024:
        parser.error('Mapped image must be bounded by 128 MiB')
    image = pe.get_memory_mapped_image()
    sections = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
    result = trace_references(image, sections, from_pe(pe), args.target, capstone_decoder(image), watches)
    result.update(executable=str(args.executable.resolve()), executable_sha256=hashlib.sha256(data).hexdigest())
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
