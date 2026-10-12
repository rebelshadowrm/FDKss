"""Recover bounded MDL transform binding switch candidates without pinned RVAs.

Only the two observed x64 encodings are recognized. Results are internal
fragments and raw layout candidates, not callable ABIs or adapter approvals.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

DISPATCH=re.compile(rb'\x83\xfb\x0b\x0f\x87(?P<default>.{4})'
    rb'\x48\x8d\x15(?P<anchor>.{4})\x8b\x8c\x9a(?P<table>.{4})'
    rb'\x48\x03\xca\xff\xe1',re.DOTALL)


def scan(image,executable,readable=None):
    readable=executable if readable is None else readable
    for ranges in (executable,readable):
        if any(not 0<=a<=b<=len(image) for a,b in ranges):
            raise ValueError('Section range is outside the mapped image')
    def contains(ranges,start,size):return any(a<=start and start+size<=b for a,b in ranges)
    candidates=[]
    for begin,end in executable:
        for match in DISPATCH.finditer(image,begin,end):
            anchor=match.end('anchor')+struct.unpack('<i',match.group('anchor'))[0]
            table=anchor+struct.unpack('<i',match.group('table'))[0]
            default=match.end('default')+struct.unpack('<i',match.group('default'))[0]
            if not 0<=anchor<len(image) or not contains(readable,table,48) or not contains(executable,default,1):continue
            targets=[anchor+value for value in struct.unpack_from('<12I',image,table)]
            pointer=None
            if match.start()>=4 and contains(executable,match.start()-4,4) and image[match.start()-4:match.start()-1]==b'\x49\x8b\x7b':
                pointer=int.from_bytes(image[match.start()-1:match.start()],'little',signed=True)
                if not 0<pointer<=0x78 or pointer%8:continue
            rows=[];continuations=set();valid=True
            for kind,target in enumerate(targets):
                inline=pointer is None
                if inline:
                    prefix=b'\x49\x8d\xbb';size=7
                    if not contains(executable,target,size) or image[target:target+3]!=prefix:valid=False;break
                    offset=struct.unpack_from('<i',image,target+3)[0]
                elif contains(executable,target,4) and image[target:target+3]==b'\x48\x83\xef':
                    size=4;offset=-int.from_bytes(image[target+3:target+4],'little',signed=True)
                elif contains(executable,target,7) and image[target:target+3]==b'\x48\x81\xc7':
                    size=7;offset=struct.unpack_from('<i',image,target+3)[0]
                else:valid=False;break
                if not 0<offset<=0x8000 or offset%4 or not contains(executable,target,size+8):valid=False;break
                if image[target+size:target+size+4]!=b'\x48\x85\xff\xe9':valid=False;break
                continuation=target+size+8+struct.unpack_from('<i',image,target+size+4)[0]
                if not contains(executable,continuation,1):valid=False;break
                continuations.add(continuation)
                body=image[target:target+size+8]
                rows.append(dict(kind=kind,branch_rva=hex(target),destination_offset=offset,
                    code_hex=body.hex(),code_sha256=hashlib.sha256(body).hexdigest()))
            if not valid or len(continuations)!=1:continue
            offsets=[row['destination_offset'] for row in rows]
            if any(offsets[base:base+3]!=[offsets[base]+i*4 for i in range(3)] for base in (0,3,6)):continue
            if any(targets[whole]!=targets[component] for whole,component in ((9,0),(10,3),(11,6))):continue
            bases=[offsets[i] for i in (0,3,6)]
            fields=list(zip(bases,(12,16,12)))
            if any(max(a,b)<min(a+wa,b+wb) for i,(a,wa) in enumerate(fields) for b,wb in fields[i+1:]):continue
            if len(candidates)==256:raise ValueError('Animation binding discovery exceeds256 candidates')
            fragment_start=match.start()-4 if pointer is not None else match.start()
            fragment=image[fragment_start:match.end()]
            candidates.append(dict(dispatch_fragment_rva=hex(fragment_start),dispatch_code_hex=fragment.hex(),
                dispatch_code_sha256=hashlib.sha256(fragment).hexdigest(),table_rva=hex(table),
                table_hex=image[table:table+48].hex(),anchor_rva=hex(anchor),default_branch_rva=hex(default),
                continuation_rva=hex(next(iter(continuations))),storage='node-transform-pointer' if pointer is not None else 'inline-node-fields',
                node_pointer_offset=pointer,branches=rows,callable_signature_verified=False))
    return sorted(candidates,key=lambda row:int(row['dispatch_fragment_rva'],16))


def discover(pe):
    if pe.FILE_HEADER.Machine!=0x8664 or not 0<pe.OPTIONAL_HEADER.SizeOfImage<=512*1024*1024:
        raise ValueError('Expected a bounded AMD64 PE image')
    image=pe.get_memory_mapped_image()
    def ranges(flag):return [(s.VirtualAddress,min(len(image),s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData))) for s in pe.sections if s.Characteristics&flag]
    return dict(format_version=1,candidates=scan(image,ranges(0x20000000),ranges(0x40000000)),limitations=[
        'Only two observed register/branch encodings and kinds0..11 are recognized.',
        'Inline offsets are relative to the node; pointer offsets are relative to the pointed transform.',
        'Fragments are not function starts, callable ABIs or proofs of full clip application.',
        'Default-branch destinations are executable-bounded; their complete bodies are not verified.',
        'No unknown build is enabled automatically; class, owner, lifetime and live-chain checks remain required.'])


def main():
    import pefile
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable',type=Path);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    if args.output.resolve()==args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('Output cannot overwrite the executable')
    data=args.executable.read_bytes()
    try:report=discover(pefile.PE(data=data))
    except (ValueError,pefile.PEFormatError) as error:parser.error(str(error))
    report['build_sha256']=hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f"Recovered {len(report['candidates'])} animation binding candidates")

if __name__=='__main__':main()
