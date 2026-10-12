"""Trace direct engine calls in bounded original x64 function families.

Offline evidence only. Does not infer receiver types, ABI or runtime ownership.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from discover_actor_cleanup_callers import capstone_decoder, walk_function
from pe_unwind import from_pe


def inspect_function(image, executable, families, entry, decode):
    """Preserve a whole primary-entry family; rejected walks are evidence only."""
    if type(entry) is not int or not 1 <= len(image) <= 128 * 1024 * 1024:
        raise ValueError('invalid entry or mapped-image bound')
    if not executable or any(not 0 <= a < b <= len(image) for a, b in executable):
        raise ValueError('invalid executable section bounds')
    a, b = families.contiguous(entry)
    if a != entry or not 0 <= a < b <= len(image) or not any(x <= a and b <= y for x, y in executable):
        raise ValueError('exact primary entry within executable sections required')
    instructions = {}
    def record(at, end):
        row = decode(at, end)
        if row:
            instructions[at] = row
        return row
    walk = walk_function(image, a, b, -1, record)
    return dict(walk=walk, code_hex=image[a:b].hex(), runtime_profile_accepted=False,
                instructions=[dict(rva=hex(at), size=row['size'], kind=row['kind'],
                                   target_rva=None if row.get('target') is None else hex(row['target']),
                                   code_hex=image[at:at + row['size']].hex())
                              for at, row in sorted(instructions.items())])


def analyze_barrier(function, before, after):
    """Check static paths from one decoded instruction to a required later node.

    This is control-flow evidence only: visiting a call is not proof that a wait
    succeeds. Calls are assumed to return; exceptional edges and conditional
    feasibility remain outside the underlying static walk.
    """
    if not function.get('walk', {}).get('complete_static_walk'):
        raise ValueError('barrier analysis requires a complete static walk')
    instructions = {int(row['rva'], 16): row for row in function['instructions']}
    if type(before) is not int or type(after) is not int or before == after or \
            before not in instructions or after not in instructions:
        raise ValueError('two distinct decoded entry-reachable instruction RVAs required')
    successors = {at: set() for at in instructions}
    external = set()
    for edge in function['walk']['edges']:
        source, destination = int(edge['source'], 16), int(edge['destination'], 16)
        if source not in successors:
            raise ValueError('edge source is not a decoded instruction')
        if edge['external']:
            external.add(source)
        elif destination not in successors:
            raise ValueError('edge destination is not a decoded instruction')
        else:
            successors[source].add(destination)
    pending, avoiding, exits = [before], set(), set()
    reaches_after = False
    while pending:
        at = pending.pop()
        if at == after:
            reaches_after = True
            continue
        if at in avoiding:
            continue
        avoiding.add(at)
        if at in external or not successors[at]:
            exits.add(at)
        pending.extend(successors[at])
    # A reachable cycle can avoid the barrier indefinitely under unsolved
    # predicates. Removing acyclic vertices detects this without recursive DFS.
    incoming = {at: 0 for at in avoiding}
    for at in avoiding:
        for destination in successors[at] & avoiding:
            incoming[destination] += 1
    ready = [at for at, count in incoming.items() if count == 0]
    removed = 0
    while ready:
        at = ready.pop(); removed += 1
        for destination in successors[at] & avoiding:
            incoming[destination] -= 1
            if incoming[destination] == 0:
                ready.append(destination)
    cycle = removed != len(avoiding)
    return dict(before_rva=hex(before), barrier_rva=hex(after),
        barrier_reachable=reaches_after,
        bypass_exit_rvas=[hex(at) for at in sorted(exits)],
        avoiding_barrier_cycle_possible=cycle,
        all_finite_static_exit_paths_cross_barrier=reaches_after and not exits,
        barrier_on_every_static_path=reaches_after and not exits and not cycle,
        runtime_synchronization_verified=False,
        limitations='Instruction ordering only: calls assumed to return, predicates unsolved; wait results, handles, receiver ownership, exceptions and runtime synchronization are unverified.')


def trace(image, executable, families, targets, decode):
    if not 1 <= len(image) <= 128 * 1024 * 1024:
        raise ValueError('mapped image exceeds trace bound')
    if not executable or any(not 0 <= a < b <= len(image) for a, b in executable):
        raise ValueError('invalid executable section bounds')
    if not targets or len(targets) > 64 or len(set(targets)) != len(targets):
        raise ValueError('one to 64 unique target RVAs required')
    if any(type(at) is not int or not any(a <= at < b for a, b in executable) for at in targets):
        raise ValueError('target leaves executable sections')
    wanted = set(targets)
    raw, rejected = {}, []
    for begin, end in executable:
        for opcode in (b'\xe8', b'\xe9'):
            cursor = begin
            while cursor + 5 <= end:
                at = image.find(opcode, cursor, end - 4)
                if at < 0:
                    break
                cursor = at + 1
                target = at + 5 + struct.unpack_from('<i', image, at + 1)[0]
                if target not in wanted:
                    continue
                try:
                    bounds = families.contiguous(at)
                    a, b = bounds
                    if not 0 <= a <= at < at + 5 <= b <= len(image) or not any(x <= a and b <= y for x, y in executable):
                        raise ValueError('function leaves executable bounds')
                except ValueError as error:
                    rejected.append(dict(rva=hex(at), target_rva=hex(target), reason=str(error)))
                    continue
                raw.setdefault(bounds, []).append((at, target))
    callers = []
    # Walk each candidate family once. Target matching uses decoded entry-reachable
    # edges, never raw opcode matches or linear disassembly of skipped bytes.
    for (a, b), matches in sorted(raw.items()):
        instructions = {}

        def record(at, end):
            row = decode(at, end)
            if row:
                instructions[at] = row
            return row

        walk = walk_function(image, a, b, -1, record)
        sites = [dict(rva=hex(at), target_rva=hex(row['target']),
                      kind='direct_call' if row['kind'] == 'call' else 'tail_jump',
                      code_hex=image[at:at + row['size']].hex())
                 for at, row in sorted(instructions.items())
                 if row['kind'] in ('call', 'jump') and row.get('target') in wanted]
        row = dict(walk=walk, raw_candidates=[dict(rva=hex(at), target_rva=hex(target))
                                            for at, target in sorted(matches)], sites=sites)
        if walk['complete_static_walk'] and sites:
            callers.append(row)
        else:
            row['reason'] = 'incomplete static walk' if not walk['complete_static_walk'] else 'no entry-reachable decoded target edge'
            rejected.append(row)
    return dict(format='fdkss.engine_direct_calls', version=1,
                targets=[hex(at) for at in targets], callers=callers, rejected=rejected,
                runtime_profile_accepted=False, limitations=[
                    'Only direct rel32 calls/tail jumps in contiguous unwind families are considered.',
                    'Calls are assumed to return and conditional predicates are unsolved.',
                    'No exceptional edges, indirect dispatch or complete caller coverage is inferred.',
                    'Targets are instruction-address candidates; receiver types, signatures and ownership remain unverified.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--target', type=lambda value: int(value, 0), action='append')
    parser.add_argument('--entry', type=lambda value: int(value, 0), action='append',
                        help='inspect a complete contiguous family from its exact primary entry')
    parser.add_argument('--barrier', action='append', metavar='BEFORE:AFTER',
                        help='check static instruction ordering within exactly one selected --entry')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not args.target and not args.entry:
        parser.error('at least one --target or --entry required')
    if args.entry and (len(args.entry) > 64 or len(set(args.entry)) != len(args.entry)):
        parser.error('one to 64 unique --entry RVAs required')
    barriers = []
    if args.barrier:
        if not args.entry or len(args.entry) != 1 or len(args.barrier) > 64:
            parser.error('barriers require exactly one --entry and at most 64 pairs')
        try:
            for value in args.barrier:
                pair = value.split(':')
                if len(pair) != 2:
                    raise ValueError('expected BEFORE:AFTER RVAs')
                barriers.append(tuple(int(at, 0) for at in pair))
            if len(set(barriers)) != len(barriers):
                raise ValueError('barrier pairs must be unique')
        except ValueError as error:
            parser.error(str(error))
    if args.output.resolve() == args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('output must not overwrite the executable')
    original = args.executable.read_bytes()
    pe = pefile.PE(data=original)
    families = from_pe(pe)
    image = pe.get_memory_mapped_image()
    executable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]
    decode = capstone_decoder(image)
    result = trace(image, executable, families, args.target, decode) if args.target else dict(
        format='fdkss.engine_direct_calls', version=1, targets=[], callers=[], rejected=[],
        runtime_profile_accepted=False,
        limitations=['Entry-reachable static edges only; calls assumed to return; no exceptional edges, ABI or runtime ownership proof.'])
    result['functions'] = []
    for entry in args.entry or []:
        try:
            row = inspect_function(image, executable, families, entry, decode)
        except ValueError as error:
            row = dict(entry_rva=hex(entry), rejected=True, reason=str(error), runtime_profile_accepted=False)
        result['functions'].append(row)
    if barriers:
        try:
            result['barriers'] = [analyze_barrier(result['functions'][0], before, after)
                                  for before, after in barriers]
        except ValueError as error:
            parser.error(str(error))
    result.update(executable=str(args.executable.resolve()), executable_sha256=hashlib.sha256(original).hexdigest())
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
