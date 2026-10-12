"""Bounded intraprocedural address aliases for original AMD64 function evidence.

Reports watched memory effects and alias escapes. Receiver seeds are explicit
hypotheses, not inferred types. This tool never approves lifetime or attachment.
"""
import argparse
from collections import deque
import hashlib
import json
from pathlib import Path

from discover_actor_cleanup_callers import capstone_decoder, walk_function
from pe_unwind import from_pe
from trace_engine_calls import inspect_function

REGISTERS = ('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp',
             'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15')
VOLATILE = ('rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11')
UNKNOWN = frozenset({None})
TOP = frozenset({'overflow'})


def canonical(name):
    for full in REGISTERS:
        if name == full or full.startswith('r') and full[1:].isdigit() and name in (full+'d', full+'w', full+'b'):
            return full
    for full, aliases in [('rax', ('eax', 'ax', 'al', 'ah')), ('rbx', ('ebx', 'bx', 'bl', 'bh')),
                          ('rcx', ('ecx', 'cx', 'cl', 'ch')), ('rdx', ('edx', 'dx', 'dl', 'dh')),
                          ('rsi', ('esi', 'si', 'sil')), ('rdi', ('edi', 'di', 'dil')),
                          ('rbp', ('ebp', 'bp', 'bpl')), ('rsp', ('esp', 'sp', 'spl'))]:
        if name in aliases:
            return full
    return None


def bounded(values):
    values = frozenset(values)
    if 'overflow' in values or len(values) > 8 or any(type(v) is int and not -0x8000000 <= v <= 0x8000000 for v in values):
        return TOP
    return values


def shift(values, displacement):
    if values == TOP:
        return TOP
    return bounded(None if v is None else v + displacement for v in values)


def analyze_aliases(function, watches, receiver_rva=None):
    """Conservative may-alias CFG walk; unknown paths remain visibly ambiguous.

    Calls clobber Windows x64 volatile registers; no callee summaries, stack
    alias reloads, indirect pointer chasing or exceptional edges are modeled.
    """
    import capstone
    from capstone.x86_const import X86_OP_REG, X86_OP_IMM, X86_OP_MEM
    walk = function.get('walk', {})
    if not walk.get('complete_static_walk'):
        raise ValueError('alias analysis requires a complete static walk')
    if not watches or len(watches) > 64 or any(type(a) is not int or type(n) is not int or not 0 <= a < a+n <= 0x8000000 for a, n in watches):
        raise ValueError('one to 64 bounded watched byte ranges required')
    if receiver_rva is not None and (type(receiver_rva) is not int or not 0 <= receiver_rva < 0x8000000):
        raise ValueError('receiver hypothesis must be a bounded image RVA')
    start, end = int(walk['function_rva'], 16), int(walk['end_rva'], 16)
    code = bytes.fromhex(function['code_hex'])
    if not 0 <= start < end <= 0x8000000 or len(code) != end-start or hashlib.sha256(code).hexdigest() != walk['body_sha256']:
        raise ValueError('function bytes do not match complete-walk identity')
    # Reconstruct edges from the preserved original bytes: a tampered cached
    # CFG must not suppress a writer or manufacture a receiver path.
    image = bytes(start) + code
    independently_decoded = {}
    decode = capstone_decoder(image)
    def record(at, limit):
        row = decode(at, limit)
        if row:
            independently_decoded[at] = row
        return row
    rebuilt = walk_function(image, start, end, -1, record)
    if not rebuilt['complete_static_walk'] or rebuilt['edges'] != walk['edges']:
        raise ValueError('CFG evidence disagrees with original bytes')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); decoder.detail = True
    decoded = {}
    for row in function['instructions']:
        at = int(row['rva'], 16)
        instruction = next(decoder.disasm(code[at-start:at-start+row['size']], at, count=1), None)
        if not start <= at < end or not instruction or instruction.size != row['size'] or instruction.bytes.hex() != row['code_hex']:
            raise ValueError('instruction evidence disagrees with original bytes')
        decoded[at] = instruction
    if len(decoded) != len(function['instructions']) or decoded.keys() != independently_decoded.keys():
        raise ValueError('instruction inventory disagrees with original reachability')
    successors = {at: [] for at in decoded}
    for edge in walk['edges']:
        a, b = int(edge['source'], 16), int(edge['destination'], 16)
        if a not in decoded or not edge['external'] and b not in decoded:
            raise ValueError('CFG edge is not a decoded instruction')
        if not edge['external']:
            successors[a].append(b)
    if start not in decoded:
        raise ValueError('missing primary entry')

    def address(ins, memory, state):
        if memory.segment or memory.index:
            return UNKNOWN
        name = ins.reg_name(memory.base)
        if name == 'rip':
            return bounded({ins.address + ins.size + memory.disp})
        return shift(state.get(canonical(name), UNKNOWN), memory.disp)

    def transfer(ins, state):
        result = dict(state)
        for reg in ins.regs_access()[1]:
            key = canonical(ins.reg_name(reg))
            if key:
                result[key] = UNKNOWN
        operands = ins.operands
        if len(operands) >= 2 and operands[0].type == X86_OP_REG and operands[0].size == 8:
            dest = canonical(ins.reg_name(operands[0].reg)); source = operands[1]
            if dest and ins.mnemonic == 'mov' and source.type == X86_OP_REG and source.size == 8:
                result[dest] = state.get(canonical(ins.reg_name(source.reg)), UNKNOWN)
            elif dest and ins.mnemonic == 'lea' and source.type == X86_OP_MEM:
                result[dest] = address(ins, source.mem, state)
            elif dest and ins.mnemonic in ('add', 'sub') and source.type == X86_OP_IMM:
                result[dest] = shift(state.get(dest, UNKNOWN), source.imm * (-1 if ins.mnemonic == 'sub' else 1))
            elif dest and ins.mnemonic.startswith('cmov') and source.type == X86_OP_REG:
                result[dest] = bounded(state.get(dest, UNKNOWN) | state.get(canonical(ins.reg_name(source.reg)), UNKNOWN))
        if ins.group(capstone.CS_GRP_CALL):
            for reg in VOLATILE:
                result[reg] = UNKNOWN
        return result

    initial = {reg: UNKNOWN for reg in REGISTERS}
    if receiver_rva is not None:
        initial['rcx'] = frozenset({receiver_rva})
    states, pending, steps = {start: initial}, deque([start]), 0
    while pending:
        at = pending.popleft(); steps += 1
        if steps > 1000000:
            raise ValueError('alias fixed-point budget exceeded')
        result = transfer(decoded[at], states[at])
        for successor in successors[at]:
            prior = states.get(successor)
            merged = result if prior is None else {reg: bounded(prior[reg] | result[reg]) for reg in REGISTERS}
            if prior != merged:
                states[successor] = merged
                pending.append(successor)
    effects, escapes, unresolved = [], [], []
    for at, state in sorted(states.items()):
        ins = decoded[at]
        for operand in ins.operands:
            if operand.type != X86_OP_MEM or ins.mnemonic == 'lea':
                continue
            addresses = address(ins, operand.mem, state)
            known = sorted(v for v in addresses if type(v) is int)
            hits = [v for v in known if any(v < a+n and a < v+operand.size for a, n in watches)]
            if hits:
                effects.append(dict(rva=hex(at), instruction=ins.mnemonic+' '+ins.op_str,
                    address_rvas=[hex(v) for v in hits], operand_bytes=operand.size,
                    read=bool(operand.access & capstone.CS_AC_READ), write=bool(operand.access & capstone.CS_AC_WRITE),
                    ambiguous=None in addresses or len(addresses) != 1))
            # Indexed accesses through a known base are not silently treated as
            # exact addresses. A later experiment must recover the index range.
            if operand.mem.index and any(type(v) is int for v in state.get(canonical(ins.reg_name(operand.mem.base)), UNKNOWN)):
                unresolved.append(dict(rva=hex(at), reason='indexed access through known address; index range unmodeled'))
        if ins.group(capstone.CS_GRP_CALL) or ins.mnemonic == 'jmp' and any(e['external'] and int(e['source'],16)==at for e in walk['edges']):
            args = {reg: [hex(v) for v in sorted(x for x in state[reg] if type(x) is int)] for reg in ('rcx','rdx','r8','r9')}
            args = {reg: values for reg, values in args.items() if values}
            if args:
                escapes.append(dict(rva=hex(at), instruction=ins.mnemonic+' '+ins.op_str, register_addresses=args,
                    reason='callee effects, receiver identity and retained aliases unverified'))
        # A pointer spill/store can escape even when the destination is unknown.
        if ins.mnemonic == 'mov' and len(ins.operands)==2 and ins.operands[0].type==X86_OP_MEM and ins.operands[1].type==X86_OP_REG and ins.operands[1].size==8:
            values = state.get(canonical(ins.reg_name(ins.operands[1].reg)), UNKNOWN)
            known = sorted(v for v in values if type(v) is int)
            if known:
                escapes.append(dict(rva=hex(at), instruction=ins.mnemonic+' '+ins.op_str,
                    stored_address_rvas=[hex(v) for v in known], reason='stored alias; memory reloads/retention unmodeled'))
        if any(values == TOP for values in state.values()):
            unresolved.append(dict(rva=hex(at), reason='address alternative/extent bound exceeded'))
    return dict(format='fdkss.engine_address_aliases', version=1, function_rva=hex(start),
        body_sha256=walk['body_sha256'], receiver_hypothesis_rva=None if receiver_rva is None else hex(receiver_rva),
        watches=[dict(rva=hex(a), bytes=n) for a,n in watches], effects=effects, escapes=escapes,
        unresolved=unresolved, fixed_point_steps=steps, runtime_profile_accepted=False,
        lifetime_verified=False, complete_producer_coverage=False,
        limitations=['Receiver seed is a hypothesis requiring independent caller/receiver proof.',
            'Only register copies, RIP/base LEA, immediate add/sub and conditional register copies retain aliases.',
            'Stack/memory reloads, global pointer loads, index ranges, callee effects and exceptions are unmodeled.',
            'Windows x64 volatile registers are discarded after calls; predicates remain unsolved.',
            'Missing watched effects cannot establish absence of writers or same-address reuse.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--entry', action='append', required=True, type=lambda s:int(s,0))
    parser.add_argument('--watch', action='append', required=True, help='RVA:BYTES')
    parser.add_argument('--receiver', action='append', default=[], help='ENTRY:RVA; conditional receiver seed')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= len(args.entry) <= 64 or len(set(args.entry)) != len(args.entry):
        parser.error('one to 64 unique entries required')
    try:
        watches = [tuple(int(v,0) for v in s.split(':')) for s in args.watch]
        receivers = [tuple(int(v,0) for v in s.split(':')) for s in args.receiver]
        if any(len(pair)!=2 for pair in watches+receivers) or len(dict(receivers))!=len(receivers) or any(entry not in args.entry for entry,_ in receivers):
            raise ValueError('invalid watch/receiver pairs')
        if args.output.resolve()==args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
            raise ValueError('output must not overwrite executable')
        data=args.executable.read_bytes(); pe=pefile.PE(data=data)
        if (pe.FILE_HEADER.Machine,pe.OPTIONAL_HEADER.Magic)!=(0x8664,0x20b) or not 1 <= pe.OPTIONAL_HEADER.SizeOfImage <= 0x8000000:
            raise ValueError('bounded AMD64 PE32+ required')
        image=pe.get_memory_mapped_image()
        if any(not 0<=a<a+n<=len(image) for a,n in watches) or any(not 0<=rva<len(image) for _,rva in receivers):
            raise ValueError('watch/receiver leaves mapped image')
        sections=[(s.VirtualAddress,s.VirtualAddress+s.Misc_VirtualSize) for s in pe.sections if s.Characteristics&0x20000000]
        families=from_pe(pe); decode=capstone_decoder(image); seeds=dict(receivers)
        functions=[inspect_function(image,sections,families,entry,decode) for entry in args.entry]
        reports=[analyze_aliases(row,watches,seeds.get(entry)) for row,entry in zip(functions,args.entry)]
        result=dict(executable=str(args.executable.resolve()), executable_sha256=hashlib.sha256(data).hexdigest(),
            functions=reports, runtime_profile_accepted=False)
        args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    except (ValueError,OSError,pefile.PEFormatError) as error:
        parser.error(str(error))


if __name__=='__main__':
    main()
