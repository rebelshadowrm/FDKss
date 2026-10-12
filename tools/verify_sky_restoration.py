import argparse,ctypes as C,hashlib,json,re,struct,sys
from pathlib import Path
from datetime import datetime,timezone
sys.path[:0]=['tools','.cache/python']
import pefile
from pe_unwind import from_pe
from windows_process import Process,kernel
parser=argparse.ArgumentParser();parser.add_argument('identity');parser.add_argument('manifest');parser.add_argument('--active-dll');parser.add_argument('--verify',action='store_true');parser.add_argument('--with-resources',action='store_true');args=parser.parse_args()
identity=json.loads(Path(args.identity).read_text(encoding='utf-8-sig'))
kernel.GetProcessTimes.argtypes=[C.c_void_p]+[C.POINTER(C.c_uint64)]*4
with Process(identity['pid']) as process:
    times=[C.c_uint64() for _ in range(4)]
    if not kernel.GetProcessTimes(process.handle,*[C.byref(v) for v in times]) or times[0].value!=identity['creation_filetime']:raise ValueError('Process creation changed')
    digest=hashlib.sha256(process.path.read_bytes()).hexdigest()
    profiles={'sora_1st.exe':('sky1','4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf'), 'sora_2nd.exe':('sky2','d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf')}
    game,pinned=profiles[process.path.name.lower()]
    if digest!=pinned:raise ValueError('Unknown original')
    if args.verify:
        manifest=json.loads(Path(args.manifest).read_text(encoding='utf-8'))
        if (manifest['pid'],manifest['creation_filetime'],manifest['image_base'],manifest['sha256'])!=(process.pid,times[0].value,process.base,digest):raise ValueError('Manifest identity changed')
        failures=[]
        for slot in manifest['slots']:
            if process.pointer(process.base+slot['rva'])!=slot['expected']:failures.append(f"slot {slot['rva']:x}")
        for body in manifest['bodies']:
            expected=bytes.fromhex(body['hex'])
            if process.read(process.base+body['rva'],len(expected))!=expected:failures.append(f"body {body['rva']:x}")
        for item in manifest['imports']:
            if process.pointer(process.base+item['rva'])!=item['expected']:failures.append(f"import {item['rva']:x}")
        if game=='sky1':
            framework=process.pointer(process.base+0x8ff030)
            background=process.read(framework+0xc,1)[0]
            if background!=manifest['expected_background']:failures.append('background preference')
        if failures:raise ValueError('Restoration incomplete: '+', '.join(failures))
        result={'utc':datetime.now(timezone.utc).isoformat(),'pid':process.pid,'creation_filetime':times[0].value,'verified':True,'slots':len(manifest['slots']),'complete_bodies':len(manifest['bodies']),'input_imports':len(manifest['imports']),'background':background if game=='sky1' else 'not owned by this task'}
        Path(args.manifest+'.verified.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result));sys.exit(0)
    active=Path(args.active_dll).resolve()
    modules=[m for m in process.modules if Path(m['path']).resolve()==active]
    if len(modules)!=1:raise ValueError('Active stage module missing/ambiguous')
    module=modules[0];module['sha256']=hashlib.sha256(active.read_bytes()).hexdigest()
    candidate_digest='757afbeca882dcce89577cdcb34263af58c73c85a7c22eda3e2a157622149eb6' if args.with_resources else 'f87aa8fcbb8ebdb17aabd6c7d2af97c44b28927aa8d9adc6a987773741721ceb'
    allowed={candidate_digest}
    if args.with_resources:
        allowed.add('3b3e254318037132953d786d693a885d35a5b6d2d649bd782647111efb5ef781')
        # October9 shared member-copy stage changes getter validation/copy logic,
        # not the 115 slots, seven detour entries or two owned input imports.
        # The complete inventory is still independently checked below.
        allowed.add('2b206e5bc3cf731e1d21ced540c29e59cc2dcd15396f2643ad3b18dcbbcb0b12')
    if module['sha256'] not in allowed:raise ValueError('Stage differs from selected hook inventory')
    pe=pefile.PE(str(process.path));families=from_pe(pe);image=pe.get_memory_mapped_image(ImageBase=process.base)
    if game=='sky1':
        source=Path('src/windows/sky1.cpp').read_text(encoding='utf-8')
        block=re.search(r'constexpr Descriptor descriptors\[\]\s*=\s*\{(.*?)\n\s*\};',source,re.S)[1]
        slots=[int(a,16) for a in re.findall(r'\{(0x[0-9a-f]+), (?:0x[0-9a-f]+),',block)]
        slots += [0x7da5a8+0x38,0x7fc550+0x20,0x8041e8+0x20,0x805d18+28*8]
        functions=[0x47ee80,0x47ef10,0x51bf50,0x521a80,0x3de830,0x1bd4d0]
        imports=[0x773498,0x773470];resource=0xdd4b0
    else:
        slots=[int(a,16) for a in re.findall(r'FDKSS_ACTOR\((0x[0-9a-f]+),',Path('src/windows/sky2_lifetimes.inc').read_text(encoding='utf-8'))]
        slots += [0xae7ab0+0x38,0xb13128+0x20,0xb1c318+0x20,0xb1e138+28*8]
        functions=[0x5a9960,0x100fb0,0x654640,0x65a120,0x4cb850,0x20c150]
        imports=[0x8bb5d8,0x8bb5f0];resource=0xf8db0
    types=[int(a,16) for a in re.findall(r'FDKSS_ACTOR_TYPE\((0x[0-9a-f]+),',Path(f'include/fdkss/{game}_actor_types.inc').read_text(encoding='utf-8'))]
    slots += [table+index*8 for table in types for index in (1,2,3,6)]
    if len(slots)!=115 or len(set(slots))!=115:raise ValueError('Hook slot inventory changed')
    slot_rows=[]
    for rva in slots:
        expected=struct.unpack_from('<Q',image,rva)[0];actual=process.pointer(process.base+rva)
        if actual!=expected and not module['base']<=actual<module['base']+module['size']:raise ValueError(f'Foreign slot {rva:x}')
        slot_rows.append(dict(rva=rva,expected=expected,observed_before=actual))
    bodies=[]
    for rva in functions+[resource]:
        if rva in (0x521a80,0x65a120):
            # Independently inspected non-unwinding raw-input leaves. Their
            # original early branches target the final ret; tables follow it.
            exit_rva=0x521c72 if game=='sky1' else 0x65a320
            if image[exit_rva]!=0xc3:raise ValueError('Raw-input leaf return changed')
            ranges=((rva,exit_rva+1,0),)
        else:_,ranges=families.containing(rva)
        if ranges[0][0]!=rva:raise ValueError('Function family entry changed')
        for begin,end,_ in ranges:
            expected=image[begin:end];actual=process.read(process.base+begin,end-begin)
            if rva==resource and not args.with_resources and actual!=expected:raise ValueError('Resource setter is not original before replacement')
            bodies.append(dict(rva=begin,hex=expected.hex(),size=end-begin,observed_before_sha256=hashlib.sha256(actual).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest(),extra_original_resource_guard=rva==resource and not args.with_resources))
    kernel.GetModuleHandleExW.argtypes=[C.c_uint32,C.c_void_p,C.POINTER(C.c_void_p)]
    kernel.GetModuleFileNameW.argtypes=[C.c_void_p,C.c_wchar_p,C.c_uint32]
    imported=[];user=C.WinDLL('user32')
    for rva,name in zip(imports,('GetAsyncKeyState','GetCursorPos')):
        address=C.cast(getattr(user,name),C.c_void_p).value;local=C.c_void_p()
        if not kernel.GetModuleHandleExW(6,C.c_void_p(address),C.byref(local)):raise C.WinError()
        path=C.create_unicode_buffer(32768)
        if not kernel.GetModuleFileNameW(local,path,len(path)):raise C.WinError()
        remote=next(m for m in process.modules if Path(m['path']).name.lower()==Path(path.value).name.lower())
        expected=remote['base']+address-local.value;actual=process.pointer(process.base+rva)
        if actual!=expected and not module['base']<=actual<module['base']+module['size']:raise ValueError('Foreign input import')
        imported.append(dict(rva=rva,name=name,expected=expected,observed_before=actual))
    manifest=dict(utc=datetime.now(timezone.utc).isoformat(),pid=process.pid,creation_filetime=times[0].value,path=str(process.path),sha256=digest,image_base=process.base,active_module=module,slots=slot_rows,bodies=bodies,imports=imported,detoured_entries=len(functions)+int(args.with_resources),extra_original_resource_guard=None if args.with_resources else resource)
    if game=='sky1':manifest.update(expected_background=0,background_basis='Original default restored by task-owned BackgroundUpdates journal; verify independently before replacement')
    Path(args.manifest).write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(game,'manifest:',len(slots),'slots;',manifest['detoured_entries'],'detoured entries;',len(bodies),'complete code ranges;',len(imported),'input imports')
