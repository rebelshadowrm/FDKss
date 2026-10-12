"""Inventory encoded disp32 member effects in complete original AMD64 walks.

Displacements identify instruction candidates, not receiver types or field
schemas. Atomic RMW, vector accesses and address-taking remain explicit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from discover_actor_cleanup_callers import capstone_decoder
from pe_unwind import from_pe
from trace_engine_calls import inspect_function


def trace_members(image, executable, families, members, decode):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_AC_READ, CS_AC_WRITE
    from capstone.x86_const import X86_OP_MEM
    if not 1 <= len(image) <= 0x8000000 or not executable or any(not 0 <= a < b <= len(image) for a,b in executable):
        raise ValueError('bounded mapped image/executable sections required')
    if not 1 <= len(members) <= 64 or len(set(members)) != len(members) or any(type(n) is not int or not 128 <= n <= 0x100000 for n in members):
        raise ValueError('one to 64 unique disp32 members in 128..0x100000 required')
    candidates, rejected = {}, []
    for member in members:
        pattern=struct.pack('<i',member)
        for a,b in executable:
            cursor=a
            while cursor+4 <= b:
                at=image.find(pattern,cursor,b)
                if at<0:
                    break
                cursor=at+1
                try:
                    start,end=families.contiguous(at)
                    if not 0 <= start <= at < at+4 <= end <= len(image) or not any(x <= start < end <= y for x,y in executable):
                        raise ValueError('candidate family leaves executable section')
                except ValueError as error:
                    rejected.append(dict(rva=hex(at),member=hex(member),reason=str(error)))
                    continue
                candidates.setdefault((start,end),[]).append(dict(rva=hex(at),member=hex(member)))
    decoder=Cs(CS_ARCH_X86,CS_MODE_64); decoder.detail=True
    accepted=[]
    for (a,b),raw in sorted(candidates.items()):
        function=inspect_function(image,executable,families,a,decode)
        if not function['walk']['complete_static_walk']:
            rejected.append(dict(function=function,raw_candidates=raw,reason='incomplete static walk'))
            continue
        sites=[]
        for instruction in function['instructions']:
            at=int(instruction['rva'],16)
            ins=next(decoder.disasm(image[at:at+instruction['size']],at,count=1))
            for operand in ins.operands:
                if operand.type != X86_OP_MEM or operand.mem.disp not in members or not operand.mem.base or ins.reg_name(operand.mem.base)=='rip':
                    continue
                # The raw candidate must be this instruction's encoded disp32,
                # not an immediate or an unrelated address in the same family.
                offset=ins.disp_offset
                # Capstone 5 reports disp_size=2 for some 66-prefixed SSE
                # disp32 operands. Prove the actual four encoded bytes instead;
                # the positive >=128 bound excludes sign-extended disp8 forms.
                if not offset or offset+4>ins.size or image[at+offset:at+offset+4]!=struct.pack('<i',operand.mem.disp):
                    continue
                sites.append(dict(rva=hex(at),member=hex(operand.mem.disp),
                    instruction=ins.mnemonic+' '+ins.op_str,code_hex=ins.bytes.hex(),
                    base_register=ins.reg_name(operand.mem.base),index_register=ins.reg_name(operand.mem.index),
                    operand_bytes=operand.size,address_taken=ins.mnemonic=='lea',
                    read=bool(operand.access&CS_AC_READ),write=bool(operand.access&CS_AC_WRITE),
                    locked=0xf0 in ins.prefix,atomic_memory_exchange=ins.mnemonic=='xchg'))
        if sites:
            accepted.append(dict(function=function,sites=sites,raw_candidates=raw))
        else:
            rejected.append(dict(function_rva=hex(a),raw_candidates=raw,reason='no entry-reachable matching disp32 operand'))
    return dict(format='fdkss.engine_member_effects',version=1,members=[hex(n) for n in members],
        functions=accepted,rejected=rejected,runtime_profile_accepted=False,receiver_verified=False,
        limitations=['Encoded positive disp32 members only; indirect aliases/arithmetic offsets and disp8 forms are outside coverage.',
            'Complete entry-reachable unwind families only; calls assumed to return; exceptional edges unmodeled.',
            'Offsets and atomic instructions do not prove receiver identity, field type, producer coverage, synchronization or lifetime.'])


def main():
    import pefile
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable',type=Path)
    parser.add_argument('--member',required=True,action='append',type=lambda s:int(s,0))
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    try:
        if args.output.resolve()==args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
            raise ValueError('output must not overwrite executable')
        data=args.executable.read_bytes(); pe=pefile.PE(data=data)
        if (pe.FILE_HEADER.Machine,pe.OPTIONAL_HEADER.Magic)!=(0x8664,0x20b) or not 1 <= pe.OPTIONAL_HEADER.SizeOfImage <= 0x8000000:
            raise ValueError('bounded AMD64 PE32+ required')
        image=pe.get_memory_mapped_image()
        sections=[(s.VirtualAddress,s.VirtualAddress+s.Misc_VirtualSize) for s in pe.sections if s.Characteristics&0x20000000]
        result=trace_members(image,sections,from_pe(pe),args.member,capstone_decoder(image))
        result.update(executable=str(args.executable.resolve()),executable_sha256=hashlib.sha256(data).hexdigest())
        args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    except (ValueError,OSError,pefile.PEFormatError) as error:
        parser.error(str(error))


if __name__=='__main__':
    main()
