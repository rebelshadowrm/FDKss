"""Freeze pinned AMD64 original function evidence; no runtime acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

from discover_actor_cleanup_callers import capstone_decoder
from trace_engine_calls import inspect_function
from pe_unwind import from_pe


def freeze_functions(image, sections, families, entries):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    if not 1 <= len(entries) <= 64 or len(set(entries)) != len(entries):
        raise ValueError('one to 64 unique primary entries required')
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    rows = []
    for entry in entries:
        fn = inspect_function(image, sections, families, entry, capstone_decoder(image))
        if not fn['walk']['complete_static_walk']:
            raise ValueError(f'incomplete function walk: {entry:#x}')
        a, b = families.contiguous(entry)
        linear = {ins.address: (ins.size, ins.bytes.hex()) for ins in decoder.disasm(image[a:b], a)}
        if any(linear.get(int(row['rva'], 16)) != (row['size'], row['code_hex'])
               for row in fn['instructions']):
            raise ValueError(f'entry walk disagrees with independent linear decoding: {entry:#x}')
        rows.append(dict(entry=entry, function=fn))
    return rows


def freeze_tail_thunks(image, sections, entries):
    """Preserve short entry-anchored forwarding sequences without unwind records.

    Entries must come from separately preserved pointer/capture evidence. This
    does not recover function boundaries or approve a receiver/calling convention.
    """
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET
    from capstone.x86_const import X86_OP_IMM
    if len(entries) > 16 or len(set(entries)) != len(entries):
        raise ValueError('at most 16 unique tail-thunk entries required')
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    def executable(at, size):
        return type(at) is int and 0 <= at < at + size <= len(image) and any(a <= at < at+size <= b for a,b in sections)
    rows = []
    for entry in entries:
        if not executable(entry, 1):
            raise ValueError('tail-thunk entry outside executable image')
        limit = min(entry+32, len(image), next(b for a,b in sections if a <= entry < b))
        instructions, cursor, target = [], entry, None
        for ins in decoder.disasm(image[entry:limit], entry):
            if ins.address != cursor or ins.mnemonic in ('ud2', 'hlt'):
                raise ValueError('unsupported tail-thunk instruction')
            cursor += ins.size
            if ins.group(CS_GRP_JUMP):
                if ins.mnemonic != 'jmp' or len(ins.operands) != 1 or ins.operands[0].type != X86_OP_IMM:
                    raise ValueError('tail thunk must end with a direct unconditional jump')
                target = ins.operands[0].imm
                if not executable(target, 1) or entry <= target < cursor:
                    raise ValueError('tail-thunk target must leave the sequence into executable image')
            elif any(ins.group(group) for group in (CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET)):
                raise ValueError('calls, returns and interrupts are not forwarding tail thunks')
            elif ins.mnemonic not in ('mov', 'lea', 'add', 'sub', 'xor', 'and', 'or', 'nop'):
                raise ValueError('unsupported forwarding tail-thunk prefix')
            instructions.append(dict(rva=hex(ins.address), size=ins.size, code_hex=ins.bytes.hex()))
            if target is not None:
                break
        if target is None:
            raise ValueError('no direct tail jump within 32-byte executable bound')
        code = image[entry:cursor]
        rows.append(dict(entry=entry, code_hex=code.hex(), body_sha256=hashlib.sha256(code).hexdigest(),
                         instructions=instructions, tail_target_rva=target,
                         boundary='Explicit entry through first direct tail jump; not an unwind function family',
                         receiver_verified=False, runtime_profile_accepted=False))
    return rows


def freeze_pe(path, expected_sha256, entries, words=(), thunks=()):
    import pefile
    if not re.fullmatch(r'[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('explicit 64-digit executable SHA-256 required')
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected_sha256.lower():
        raise ValueError('executable SHA-256 mismatch')
    try:
        pe = pefile.PE(data=data)
    except pefile.PEFormatError as error:
        raise ValueError(f'invalid PE: {error}') from error
    if (pe.FILE_HEADER.Machine, pe.OPTIONAL_HEADER.Magic) != (0x8664, 0x20b) or not 1 <= pe.OPTIONAL_HEADER.SizeOfImage <= 0x8000000:
        raise ValueError('bounded AMD64 PE32+ required')
    image = pe.get_memory_mapped_image()
    sections = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                for s in pe.sections if s.Characteristics & 0x20000000]
    if len(words) > 64 or len(set(words)) != len(words) or any(type(at) is not int or not 0 <= at <= len(image)-8 for at in words):
        raise ValueError('at most 64 unique bounded pointer-word RVAs required')
    base = pe.OPTIONAL_HEADER.ImageBase
    return dict(format='fdkss.frozen_engine_evidence', version=1,
        executable=str(path.resolve()), executable_sha256=digest,
        preferred_image_base=base, fixtures=freeze_functions(image, sections, from_pe(pe), entries),
        tail_thunks=freeze_tail_thunks(image, sections, thunks),
        pointer_words=[dict(rva=at, code_hex=image[at:at+8].hex()) for at in words],
        imports={hex(imp.address-base): imp.name.decode('ascii')
                 for descriptor in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', [])
                 for imp in descriptor.imports if imp.name},
        runtime_profile_accepted=False, receiver_verified=False,
        provenance='Pinned PE; complete primary-entry walks cross-checked against independent linear Capstone decoding. Both decodings use Capstone; this is not an independent decoder implementation.',
        limitations=['Selected functions and original words only; no complete producer coverage.',
                     'Calls assumed to return; exceptional paths and indirect effects unmodeled.',
                     'Original bytes do not establish type, ABI, lifetime or synchronization.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--entry', action='append', required=True, type=lambda s: int(s, 0))
    parser.add_argument('--word', action='append', default=[], type=lambda s: int(s, 0))
    parser.add_argument('--thunk', action='append', default=[], type=lambda s: int(s, 0),
                        help='explicit pointer/capture-anchored short direct forwarding thunk; not a recovered function boundary')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--replace', action='store_true', help='replace an existing backed-up fixture')
    args = parser.parse_args()
    temporary = None
    try:
        if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
            raise ValueError('output must not overwrite executable')
        if args.output.exists() and not args.replace:
            raise ValueError('output exists; back it up before using --replace')
        result = freeze_pe(args.executable, args.sha256, args.entry, args.word, args.thunk)
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=args.output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(json.dumps(result, indent=2)+'\n')
        os.replace(temporary, args.output)
        temporary = None
    except (ValueError, OSError) as error:
        parser.error(str(error))
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
