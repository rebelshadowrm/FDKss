"""Emulate original Actor cleanup with inert private resource destructors.

Proves tested field writes and callback order, never actual resource disposal,
Actor destruction, callable runtime ownership or lifecycle invalidation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct
from validate_mdl_transforms import Oracle,PROFILES


def verify(path,game,pin,cases):
    from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_WRITE
    from unicorn.x86_const import UC_X86_REG_RCX,UC_X86_REG_RDX
    oracle=Oracle(path,game);entry=int(pin['handler_rva'],16);body=bytes.fromhex(pin['code_hex'])
    if pin['build_sha256']!=PROFILES[game][0] or len(body)!=145 or hashlib.sha256(body).hexdigest()!=pin['body_sha256'] or bytes(oracle.vm.mem_read(oracle.base+entry,len(body)))!=body:
        raise ValueError('Cleanup profile differs from original pinned code')
    if not isinstance(cases,list) or not 1<=len(cases)<=1000:raise ValueError('Cleanup cases must be bounded')
    oracle.vm.mem_write(0x3000080,b'\xc3');observed=[];bad=[]
    def on_code(vm,at,size,user):
        if at==0x3000080:observed.append([vm.reg_read(UC_X86_REG_RCX),vm.reg_read(UC_X86_REG_RDX)])
        elif not oracle.base+entry<=at<oracle.base+entry+len(body):
            bad.append('Execution left original helper/private inert destructor');vm.emu_stop()
    def on_write(vm,access,at,size,value,user):
        allowed=(0x201e000<=at and at+size<=0x2020000) or (at,size) in ((0x2000158,8),(0x2000160,8),(0x2000180,8)) or (0x2004000<=at and at+size<=0x2004200 and size==8 and (at-0x2004000)%8==0)
        if not allowed:bad.append('Unexpected private write');vm.emu_stop()
    hooks=[oracle.vm.hook_add(UC_HOOK_CODE,on_code),oracle.vm.hook_add(UC_HOOK_MEM_WRITE,on_write)]
    callbacks=0
    try:
        for index,case in enumerate(cases):
            slots=case['slots'];link=case['linked_object']
            if not isinstance(slots,list) or len(slots)>64 or link not in (0,0x20020e0) or any(not isinstance(v,int) or v!=0 and (not 0x2002000<=v<=0x20020c0 or (v-0x2002000)%0x20) for v in slots):
                raise ValueError('Frozen private resource addresses/counts are invalid')
            expected_calls=([[link,1]] if link else [])+[[value,1] for value in slots if value]
            if expected_calls!=case['destructor_arguments']:raise ValueError('Frozen callback evidence disagrees with private inputs')
            storage=bytearray(random.Random(1090+index).randbytes(0x20000))
            for offset,value in ((0,0x1111111122222222),(0x158,link),(0x178,0x2004000),(0x180,len(slots))):struct.pack_into('<Q',storage,offset,value)
            for at in {v for v in slots if v}|{link}:
                if at:struct.pack_into('<Q',storage,at-0x2000000,0x2003000)
            struct.pack_into('<Q',storage,0x3000,0x3000080)
            for i,value in enumerate(slots):struct.pack_into('<Q',storage,0x4000+i*8,value)
            oracle.vm.mem_write(0x2000000,bytes(storage));observed.clear();bad.clear()
            oracle.call(entry,0x2000000)
            if bad:raise ValueError(bad[0])
            if observed!=expected_calls:raise ValueError('Original ordered destructor callbacks differ from frozen evidence')
            expected=storage[:]
            for at in (0x158,0x160,0x180):struct.pack_into('<Q',expected,at,0)
            for i,value in enumerate(slots):
                if value:struct.pack_into('<Q',expected,0x4000+i*8,0)
            actual=bytes(oracle.vm.mem_read(0x2000000,0x20000))
            if actual[:0x1e000]!=bytes(expected[:0x1e000]):raise ValueError('Actor/resource/private canary storage differs from frozen writes')
            callbacks+=len(observed)
        observed.clear();bad.clear();oracle.call(entry,0x2000000)
        if observed or bad:raise ValueError('Repeated empty cleanup caused a new callback or invalid instruction')
    finally:
        for hook in hooks:oracle.vm.hook_del(hook)
    return dict(build_sha256=PROFILES[game][0],body_sha256=pin['body_sha256'],original_cases=len(cases),
        total_private_destructor_callbacks=callbacks,inert_resource_destructors=True,
        all_actor_and_private_canaries_preserved_except_declared_writes=True,
        repeated_empty_cleanup_has_no_callbacks=True,lifetime_invalidation_proven=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1',type=Path);parser.add_argument('sky2',type=Path)
    parser.add_argument('--pins',required=True,type=Path);parser.add_argument('--fixtures',required=True,type=Path)
    parser.add_argument('--report',required=True,type=Path);args=parser.parse_args()
    inputs=(args.sky1,args.sky2,args.pins,args.fixtures,Path(__file__))
    if any(args.report.resolve()==p.resolve() or args.report.exists() and args.report.samefile(p) for p in inputs):parser.error('Report cannot overwrite an input or verifier')
    pins=json.loads(args.pins.read_text());fixtures=json.loads(args.fixtures.read_text())
    report={game:verify(getattr(args,game),game,pins[game],fixtures['cases']) for game in ('sky1','sky2')}
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2)+'\n')
    print('Both original helpers: frozen callbacks and private canaries reproduced; Actor lifetime invalidation remains unproven')

if __name__=='__main__':main()
