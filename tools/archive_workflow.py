"""Prepare bounded copied P3A/FPAC packages with independent content verification.

External tools are never redistributed. Outputs and benchmark logs remain in a
new workspace; native overlay selection/loading is outside this offline contract.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import pac
import p3a_access

MAX_FILES = 10000
MAX_TOTAL = 256 * 1024 * 1024


def inputs(directory):
    supplied=Path(directory)
    if supplied.is_symlink() or supplied.is_junction():
        raise ValueError('Input root links/reparse points are not package inputs')
    root = supplied.resolve()
    if not root.is_dir():
        raise ValueError('Expected an input asset directory')
    files, names, total = [], set(), 0
    for path in sorted(root.rglob('*')):
        if path.is_symlink() or path.is_junction():
            raise ValueError('Input links/reparse points are not package inputs')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError('Non-regular package input')
        name = path.relative_to(root).as_posix()
        if len(name.encode('utf-8')) >= 256:
            raise ValueError('Archive input path exceeds P3A path width')
        p3a_access.safe_name(name.encode()+b'\0')
        if name.casefold() in names:
            raise ValueError('Case-insensitive duplicate package path')
        names.add(name.casefold())
        size = path.stat().st_size
        total += size
        if size > p3a_access.MAX_FILE or total > MAX_TOTAL or len(files) >= MAX_FILES:
            raise ValueError('Package input budget exceeded')
        files.append({'name':name, 'size':size, 'sha256':p3a_access.sha256(path)})
    if not files:
        raise ValueError('Empty package input')
    return root, files


def pack(directory, workspace, kind, tool, *, compression='none', wsl_distro=None, benchmark_accesses=0):
    source, files = inputs(directory)
    work = Path(workspace).resolve()
    tool = Path(tool).resolve()
    if work.exists() or work.is_relative_to(source) or source.is_relative_to(work):
        raise ValueError('Workspace must be new and independent of package inputs')
    if not tool.is_file() or kind not in ('p3a','fpac') or compression not in ('none','lz4','zstd'):
        raise ValueError('Expected external tool and supported package format/compression')
    if not 0 <= benchmark_accesses <= 10000 or (benchmark_accesses and kind != 'p3a'):
        raise ValueError('Benchmark is bounded P3A-only')
    roots = {f['name'].split('/')[0] for f in files}
    if kind == 'fpac' and (len(roots)!=1 or any('/' not in f['name'] for f in files)):
        raise ValueError('External FPACker package requires exactly one top-level asset directory')
    work.mkdir(parents=True)
    manifest = {'format':'fdkss.offline_package','version':1,'kind':kind,'source':str(source),
                'inputs':files,'tool':str(tool),'tool_sha256':p3a_access.sha256(tool),
                'compression':compression if kind=='p3a' else 'none','commands':[],
                'native_loading_verified':False,'runtime_overlay_selection_verified':False,'state':'prepared'}
    def save():
        (work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    def run(command,cwd,label):
        start=time.perf_counter()
        result=subprocess.run(command,cwd=cwd,capture_output=True,timeout=120)
        elapsed=time.perf_counter()-start
        (work/(label+'.log')).write_bytes(result.stdout+result.stderr)
        manifest['commands'].append({'argv':command,'cwd':str(cwd),'returncode':result.returncode,'elapsed_seconds':elapsed})
        save()
        if result.returncode:
            raise ValueError(f'External {label} failed; see retained log')
    try:
        copied = work/'input'
        copied.mkdir()
        for f in files:
            dest=copied/f['name'];dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source/f['name'],dest)
        (work/'output').mkdir()
        output=work/'output'/('package.'+('p3a' if kind=='p3a' else 'pac'))
        if kind=='p3a':
            run([str(tool),'archive',str(output),*[f['name'] for f in files],f'--comp={compression}','--threads=1'],copied,'pack')
            # The existing access contract verifies index, tool verify and every extracted content hash.
            access=p3a_access.extract(output,tool,work/'verified')
            actual=[{'name':e['name'],'size':e['size'],'sha256':p3a_access.sha256(work/'verified/extracted'/e['name'])}
                    for e in p3a_access.inventory(output)['entries']]
            manifest['verification_manifest']=str(work/'verified/manifest.json')
            if benchmark_accesses:
                run([str(tool),'benchmark',str(output),f'--num_files={benchmark_accesses}'],work,'benchmark')
                manifest['benchmark_limitations']='Tool-only random extraction on a copied package; OS cache uncontrolled. Not game hot/cold load or contention evidence.'
        else:
            asset=copied/next(iter(roots))
            if wsl_distro:
                def linux(path):
                    result=subprocess.run(['wsl','--distribution',wsl_distro,'--','wslpath','-a',path.as_posix()],capture_output=True,timeout=30)
                    if result.returncode:
                        raise ValueError('Cannot map explicit path into selected WSL distribution')
                    return result.stdout.decode().strip()
                command=['wsl','--distribution',wsl_distro,'--',linux(tool),'pack',linux(asset)]
            else:
                command=[str(tool),'pack',str(asset)]
            run(command,copied,'pack')
            produced=copied/'out.pac'
            if not produced.is_file():
                raise ValueError('External FPACker did not produce expected isolated output')
            shutil.move(produced,output)
            actual=[]
            for entry in pac.inventory(output):
                report=pac.extract(output,entry.name,work/'verified'/entry.name)
                actual.append({k:report[k] for k in ('name','size','sha256')})
        if sorted(actual,key=lambda f:f['name']) != sorted(files,key=lambda f:f['name']):
            raise ValueError('Package content differs from exact input set')
        for f in files:
            if p3a_access.sha256(source/f['name'])!=f['sha256'] or p3a_access.sha256(copied/f['name'])!=f['sha256']:
                raise ValueError('Original/copied package input changed')
        if p3a_access.sha256(tool)!=manifest['tool_sha256']:
            raise ValueError('External tool identity changed')
        manifest.update(state='verified',output=str(output),output_sha256=p3a_access.sha256(output),verified_files=len(actual),originals_unchanged=True)
        save()
        return manifest
    except Exception as error:
        manifest.update(state='failed',failure=str(error));save();raise


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input_directory',type=Path);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--kind',choices=['p3a','fpac'],required=True);p.add_argument('--tool',type=Path,required=True)
    p.add_argument('--compression',choices=['none','lz4','zstd'],default='none')
    p.add_argument('--wsl-distro');p.add_argument('--benchmark-accesses',type=int,default=0)
    a=p.parse_args()
    try:
        report=pack(a.input_directory,a.workspace,a.kind,a.tool,compression=a.compression,wsl_distro=a.wsl_distro,benchmark_accesses=a.benchmark_accesses)
    except (ValueError,OSError,subprocess.SubprocessError) as e:
        p.exit(1,str(e)+'\n')
    print(f"Verified {report['verified_files']} packaged files; {report['output']}")


if __name__=='__main__':main()
