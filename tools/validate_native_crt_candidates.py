"""Verify frozen scalar CRT candidate traces in isolated original-code memory.

This is research evidence for exact pinned files and tested paths, not runtime
acceptance. Original CRT code runs in Unicorn; no host callbacks or game writes.
The disk-initialized dispatch flag is checked, never forced to another value.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct

from validate_mdl_transforms import Oracle, PROFILES


def verify(path,game,pins,cases,traces):
    from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
    oracle=Oracle(path,game)
    report=[]
    for pin in (row for row in pins if row['game']==game):
        command=pin['command'];entry=int(pin['handler_rva'],16)
        if command not in (20,21,22,24):raise ValueError('Unsupported CRT research command')
        code=bytes.fromhex(pin['code_hex'])
        if not 1<=len(code)<=256 or bytes(oracle.vm.mem_read(oracle.base+entry,len(code)))!=code:
            raise ValueError('Original handler body differs from the independent pin')
        trace=next(row for row in traces[game] if row['command']==command)
        if not 1<=len(trace['trace_instructions'])<=4096 or not 1<=len(trace['image_reads'])<=2048:
            raise ValueError('Dependency trace exceeds research bounds')
        instructions={}
        for row in trace['trace_instructions']:
            rva=int(row['rva'],16);body=bytes.fromhex(row['hex'])
            if not 0<=rva<512*1024*1024 or not 1<=len(body)<=15 or rva in instructions:
                raise ValueError('Invalid traced instruction')
            if bytes(oracle.vm.mem_read(oracle.base+rva,len(body)))!=body:
                raise ValueError('Original traced code changed')
            instructions[rva]=body
        reads={}
        for row in trace['image_reads']:
            rva=int(row['rva'],16);size=row['size']
            if not 0<=rva<512*1024*1024 or not isinstance(size,int) or not 1<=size<=16 or (rva,size) in reads:
                raise ValueError('Invalid traced image read')
            body=bytes(oracle.vm.mem_read(oracle.base+rva,size))
            if hashlib.sha256(body).hexdigest()!=row['sha256']:
                raise ValueError('Original traced constant/dispatch state changed')
            reads[rva,size]=body
        violations=[];executed=set();observed_reads=set();writes=[]
        def on_code(vm,at,size,user):
            rva=at-oracle.base
            body=instructions.get(rva)
            if body is None or size!=len(body) or bytes(vm.mem_read(at,size))!=body:
                violations.append('Execution left the frozen dependency trace');vm.emu_stop()
            else:executed.add(rva)
        def on_read(vm,access,at,size,value,user):
            if at<oracle.base:
                if not (0x2000000<=at<at+size<=0x2000420 or 0x201e000<=at<at+size<=0x2020000):
                    violations.append('Read left private Thread/call stack and pinned image');vm.emu_stop()
                return
            key=at-oracle.base,size
            if key not in reads or bytes(vm.mem_read(at,size))!=reads[key]:
                violations.append('Image read left the frozen constant/dispatch trace');vm.emu_stop()
            else:observed_reads.add(key)
        def on_write(vm,access,at,size,value,user):
            writes.append((at,size))
            if (at,size)!=(0x2000014,4) and not 0x201e000<=at<at+size<=0x2020000:
                violations.append('CRT wrote outside private result/call stack');vm.emu_stop()
        hooks=[oracle.vm.hook_add(kind,callback) for kind,callback in
               ((UC_HOOK_CODE,on_code),(UC_HOOK_MEM_READ,on_read),(UC_HOOK_MEM_WRITE,on_write))]
        count=0
        try:
            for index,case in enumerate(cases):
                if case['command']!=command:continue
                words=case['arguments'];arity=2 if command==24 else 1
                if len(words)!=arity or any(not isinstance(v,int) or not 0<=v<=0xffffffff for v in words):
                    raise ValueError('Invalid frozen scalar arguments')
                storage=bytearray(random.Random(index+10724).randbytes(0x420))
                struct.pack_into('<Q',storage,0x58,0x20000c8)
                struct.pack_into('<I',storage,0x64,4*arity);struct.pack_into('<I',storage,0x70,arity);storage[0x80]=1
                for i,word in enumerate(reversed(words)):struct.pack_into('<I',storage,0xc8+4*i,word)
                oracle.vm.mem_write(0x2000000,bytes(storage));writes.clear()
                try:oracle.call(entry,0,0x2000000)
                except ValueError:
                    if violations:raise ValueError(violations[0])
                    raise
                if violations:raise ValueError(violations[0])
                struct.pack_into('<I',storage,0x14,case['result'])
                if bytes(oracle.vm.mem_read(0x2000000,len(storage)))!=storage:
                    raise ValueError('Original result/private storage differs from frozen evidence')
                if [(at,size) for at,size in writes if at<0x201e000]!=[(0x2000014,4)]:
                    raise ValueError('Private result write count changed')
                count+=1
        finally:
            for hook in hooks:oracle.vm.hook_del(hook)
        if count!=trace['original_cases'] or executed!=set(instructions) or observed_reads!=set(reads):
            raise ValueError('Frozen cases did not reproduce the complete observed dependency trace')
        report.append(dict(command=command,handler_rva=hex(entry),handler_body_size=len(code),
            handler_sha256=hashlib.sha256(code).hexdigest(),original_cases=count,
            traced_instructions=len(executed),traced_image_reads=len(observed_reads),
            only_private_result_and_call_stack_written=True,runtime_signature_approved=False))
    if len(report)!=4 or len({row['command'] for row in report})!=4:raise ValueError('Incomplete candidate pins')
    return dict(build_sha256=PROFILES[game][0],candidates=report,
                limitations='Disk dispatch state and frozen inputs only; alternate CPU dispatch, exceptional domains and live invocation are unverified')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1',type=Path);parser.add_argument('sky2',type=Path)
    parser.add_argument('--pins',required=True,type=Path);parser.add_argument('--fixtures',required=True,type=Path)
    parser.add_argument('--traces',required=True,type=Path);parser.add_argument('--report',required=True,type=Path)
    args=parser.parse_args()
    if args.report.resolve() in {p.resolve() for p in (args.sky1,args.sky2,args.pins,args.fixtures,args.traces,Path(__file__))}:
        parser.error('Report cannot overwrite an input or verifier')
    pins=json.loads(args.pins.read_text());cases=json.loads(args.fixtures.read_text());traces=json.loads(args.traces.read_text())
    report={game:verify(getattr(args,game),game,pins,cases,traces) for game in ('sky1','sky2')}
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print('Both builds: frozen CRT results, instruction/read dependencies and private canaries reproduced; runtime calls remain gated')


if __name__=='__main__':main()
