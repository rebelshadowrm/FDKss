"""Find entry-reachable Actor cleanup call sites and RTTI slot-zero associations.

Requires pefile and capstone for disk discovery. No game process is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from discover_actor_types import CLEANUP_TEMPLATE, discover_base_destructors
from dump_sdk import recover


def walk_function(image, start, end, target, decode, budget=65536):
    """Walk static edges; calls are assumed to return, predicates are not solved.

    decode(at, end) returns {size, kind, target}. kind is ordinary, call, jump,
    conditional, return or stop. Indirect jumps and external conditional edges
    are unresolved. Overlapping instruction interpretations invalidate the walk.
    """
    if not 0 <= start < end <= len(image) or not 0 < budget <= 65536:
        raise ValueError('invalid function bounds or instruction budget')
    pending = [start]
    visited, occupied, sites, unresolved, edges = {}, {}, [], [], []
    while pending:
        at = pending.pop()
        if at in visited:
            continue
        if not start <= at < end:
            unresolved.append(dict(rva=hex(at), reason='edge leaves function'))
            continue
        if len(visited) >= budget:
            unresolved.append(dict(rva=hex(at), reason='instruction budget exceeded'))
            break
        if at in occupied:
            unresolved.append(dict(rva=hex(at), reason='branch enters instruction interior'))
            break
        instruction = decode(at, end)
        if not instruction or not 1 <= instruction['size'] <= 15 or at + instruction['size'] > end:
            unresolved.append(dict(rva=hex(at), reason='undecodable or truncated instruction'))
            continue
        size, kind = instruction['size'], instruction['kind']
        if kind not in ('ordinary', 'call', 'jump', 'conditional', 'return', 'stop'):
            raise ValueError('unknown decoded instruction kind')
        if any(byte in occupied for byte in range(at, at + size)):
            unresolved.append(dict(rva=hex(at), reason='overlapping instruction interpretations'))
            break
        visited[at] = instruction
        for byte in range(at, at + size):
            occupied[byte] = at
        destination = instruction.get('target')
        if kind in ('call', 'jump') and destination == target:
            sites.append(dict(rva=hex(at), kind='direct_call' if kind == 'call' else 'tail_jump',
                              code_hex=image[at:at + size].hex()))
        successors = []
        if kind in ('ordinary', 'call', 'conditional'):
            successors.append(at + size)
        if kind in ('jump', 'conditional'):
            if destination is None:
                unresolved.append(dict(rva=hex(at), reason='indirect branch'))
            elif start <= destination < end:
                successors.append(destination)
            elif kind == 'conditional':
                unresolved.append(dict(rva=hex(at), reason='conditional edge leaves function'))
            else:
                edges.append(dict(source=hex(at), destination=hex(destination), external=True))
        for successor in successors:
            edges.append(dict(source=hex(at), destination=hex(successor), external=False))
            pending.append(successor)
    complete = not unresolved
    return dict(function_rva=hex(start), end_rva=hex(end),
                body_sha256=hashlib.sha256(image[start:end]).hexdigest(),
                instruction_count=len(visited), sites=sorted(sites, key=lambda row: int(row['rva'], 16)),
                unresolved=unresolved, complete_static_walk=complete,
                edges=sorted(edges, key=lambda row: (int(row['source'], 16), int(row['destination'], 16))))


def capstone_decoder(image):
    import capstone
    from capstone.x86_const import X86_OP_IMM
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True

    def decode(at, end):
        instruction = next(decoder.disasm(image[at:min(end, at + 15)], at, count=1), None)
        if instruction is None:
            return None
        kind = 'ordinary'
        if instruction.group(capstone.CS_GRP_CALL):
            kind = 'call'
        elif instruction.group(capstone.CS_GRP_JUMP) or instruction.group(capstone.CS_GRP_BRANCH_RELATIVE):
            kind = 'jump' if instruction.mnemonic == 'jmp' else 'conditional'
        elif instruction.group(capstone.CS_GRP_RET) or instruction.group(capstone.CS_GRP_IRET):
            kind = 'return'
        elif instruction.mnemonic in ('int3', 'ud2', 'hlt'):
            kind = 'stop'
        destination = instruction.operands[0].imm if kind in ('call', 'jump', 'conditional') and instruction.operands and instruction.operands[0].type == X86_OP_IMM else None
        return dict(size=instruction.size, kind=kind, target=destination)
    return decode


def discover(pe):
    if pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('caller discovery requires AMD64')
    image = pe.get_memory_mapped_image()
    executable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]
    readable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                for s in pe.sections if s.Characteristics & 0x40000000]
    if any(not 0 <= a <= b <= len(image) for a, b in executable + readable):
        raise ValueError('section extends beyond mapped image')
    classes = recover(pe)
    destructors = discover_base_destructors(image, executable, classes, pe.OPTIONAL_HEADER.ImageBase, readable)
    helpers = [row['cleanup_dependency'] for row in destructors['candidates'] if row['cleanup_dependency']]
    if len(helpers) != 1:
        raise ValueError('unique exact-body Actor cleanup dependency required')
    target = int(helpers[0]['handler_rva'], 16)
    if image[target:target + len(CLEANUP_TEMPLATE)] != CLEANUP_TEMPLATE:
        raise ValueError('cleanup body changed')
    functions = sorted((entry.struct.BeginAddress, entry.struct.EndAddress)
                       for entry in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', []))
    if not functions:
        raise ValueError('unwind function bounds required')
    if any(not 0 <= start < end <= len(image) or not any(a <= start and end <= b for a, b in executable)
           for start, end in functions):
        raise ValueError('unwind function leaves executable bounds')
    if any(a[1] > b[0] for a, b in zip(functions, functions[1:])):
        raise ValueError('overlapping unwind function bounds')
    slot_zero = {}
    for cls in classes:
        if not any(base['class'] == '.?AVActor@fdk@@' and base['member_displacement'] == 0 and
                   base['vbtable_displacement'] == -1 for base in cls['bases']):
            continue
        for table in cls['vtables']:
            if table['subobject_offset'] or table['constructor_displacement']:
                continue
            table_rva = int(table['rva'], 16)
            for method in table['methods']:
                if method['slot'] != 0:
                    continue
                handler = int(method['rva'], 16)
                if not any(a <= table_rva and table_rva + 8 <= b for a, b in readable) or struct.unpack_from('<Q', image, table_rva)[0] != pe.OPTIONAL_HEADER.ImageBase + handler:
                    raise ValueError('slot-zero metadata disagrees with image')
                slot_zero.setdefault(handler, []).append(dict(class_name=cls['name'], vtable_rva=table['rva']))
    decode = capstone_decoder(image)
    callers, rejected, raw_count = [], [], 0
    for start, end in functions:
        raw = []
        for at in range(start, end - 4):
            if image[at] in (0xe8, 0xe9) and at + 5 + struct.unpack_from('<i', image, at + 1)[0] == target:
                raw.append(at)
        if not raw:
            continue
        raw_count += len(raw)
        row = walk_function(image, start, end, target, decode)
        row['raw_candidate_rvas'] = [hex(at) for at in raw]
        row['actor_primary_slot_zero'] = sorted(slot_zero.get(start, []), key=lambda item: item['class_name'])
        row['lifetime_invalidation_verified'] = False
        if row['sites'] and row['complete_static_walk']:
            callers.append(row)
        else:
            rejected.append(row)
    return dict(format='fdkss.actor_cleanup_caller_candidates', version=1, helper=helpers[0],
                raw_direct_candidates=raw_count, callers=callers, rejected=rejected,
                limitations=[
                    'Entry-reachable static edges only: conditional predicates are unsolved and calls are assumed to return.',
                    'Exceptional control flow is not modeled; no execution reachability or complete lifetime coverage is inferred.',
                    'Unwind-bounded functions only; indirect branches, cross-function conditional edges and incomplete walks are rejected.',
                    'RTTI slot-zero associations do not prove the receiver passed to cleanup, resource ownership or Actor death.',
                    'No callable ABI, runtime hook, natural gameplay or complete destruction coverage is approved.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('output cannot overwrite executable')
    data = args.executable.read_bytes()
    try:
        report = discover(pefile.PE(data=data))
    except (ValueError, pefile.PEFormatError) as error:
        parser.error(str(error))
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{len(report['callers'])} complete static caller walks; {len(report['rejected'])} rejected functions")


if __name__ == '__main__':
    main()
