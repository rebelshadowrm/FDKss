"""Run the pinned external sora2looseload verifier on a supported Sky2 file.

Read-only static preflight; never loads a proxy, attaches or approves hook ABI.
Upstream source remains external. Checkout, script and executable identities are
checked before execution; output is written outside both inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def verify_checkout(root, entry):
    root = root.resolve()
    def git(*args):
        return subprocess.run(['git','-C',str(root),*args],check=True,capture_output=True,
                              text=True,timeout=15).stdout.strip()
    if Path(git('rev-parse','--show-toplevel')).resolve()!=root:
        raise ValueError('peer root is not its own Git checkout')
    if git('rev-parse','HEAD')!=entry['revision'] or git('status','--porcelain','--untracked-files=no'):
        raise ValueError('peer revision or tracked working tree differs from assessed source')
    for name, expected in entry['assessed_file_sha256'].items():
        path = (root/name).resolve()
        if not path.is_relative_to(root) or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
            raise ValueError('assessed peer file changed: '+name)


def static_compatible(report, digest):
    if report.get('sha256','').lower()!=digest or report.get('machine')!='0x8664':
        raise ValueError('external report does not match AMD64 executable identity')
    return (report.get('log_hook_required') is True and
            report.get('xinput1_4_ordinals')==[2,3] and
            all(len(report.get('patterns',{}).get(name,[]))==1
                for name in ['initial_file_check','locale_handler','debug_logger']))


def preflight(root, executable, manifest):
    entry = manifest['sora2looseload']
    verify_checkout(root,entry)
    executable = executable.resolve()
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    if executable.name.lower()!='sora_2nd.exe' or digest not in entry['validated_builds']:
        raise ValueError('executable is outside assessed Sky2 build scope')
    script = root.resolve()/'tools/verify_target.py'
    with tempfile.TemporaryDirectory(prefix='fdkss-peer-preflight-') as workspace:
        invocation = [sys.executable,str(script),str(executable),'--with-log-hook']
        result = subprocess.run(invocation,cwd=workspace,capture_output=True,text=True,timeout=30)
    if hashlib.sha256(executable.read_bytes()).hexdigest()!=digest:
        raise ValueError('executable changed during read-only preflight')
    report = json.loads(result.stdout)
    compatible = static_compatible(report,digest) and result.returncode==0
    return dict(format='fdkss.peer_runtime_preflight',version=1,peer='sora2looseload',
                revision=entry['revision'],source_hashes=entry['assessed_file_sha256'],
                executable_sha256=digest,invocation=invocation,external_report=report,
                external_exit_code=result.returncode,stderr=result.stderr,
                static_compatible=compatible,runtime_accepted=False,
                limitations='Original-file uniqueness/import check only; no proxy loading, patched-process inspection, coexistence, ABI or lifetime approval.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--peer-root',type=Path,required=True)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    manifest_path = Path(__file__).with_name('peer_resources.json')
    try:
        output = args.output.resolve()
        if output.is_relative_to(args.peer_root.resolve()) or output.is_relative_to(args.executable.resolve().parent):
            raise ValueError('output must stay outside peer checkout and game installation')
        for source in [manifest_path,args.executable]:
            if output==source.resolve() or output.exists() and output.samefile(source):
                raise ValueError('output collides with an input')
        result = preflight(args.peer_root,args.executable,json.loads(manifest_path.read_text(encoding='utf-8')))
        output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({k:result[k] for k in ['peer','revision','static_compatible','runtime_accepted']}))
        return 0 if result['static_compatible'] else 2
    except (ValueError,OSError,subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__=='__main__':
    raise SystemExit(main())
