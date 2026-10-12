"""Inventory bounded table coverage for one explicit game without changing inputs."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

import table_inspect


def scan(input_root,game,workspace,*,schemas_root=None):
    root,work=Path(input_root).resolve(),Path(workspace).resolve()
    if not root.is_dir() or work.exists() or work.is_relative_to(root):
        raise ValueError('Expected input directory and new independent coverage workspace')
    files=sorted(root.rglob('*.tbl'))
    if not files or len(files)>10000 or sum(f.stat().st_size for f in files)>512*1024*1024:
        raise ValueError('Table collection is empty or exceeds count/total-byte budget')
    if any(f.is_symlink() or f.is_junction() or not f.resolve().is_relative_to(root) for f in files):
        raise ValueError('Table collection links are not accepted')
    schemas,pin=table_inspect.load_game_schemas(game,schemas_root)
    work.mkdir(parents=True)
    summary=Counter();reports=[]
    for f in files:
        if f.stat().st_size>table_inspect.MAX_SIZE:
            reports.append({'path':f.relative_to(root).as_posix(),'container_refusal':'File exceeds table budget'})
            summary['container_refused']+=1;continue
        raw=f.read_bytes();digest=hashlib.sha256(raw).hexdigest()
        row={'path':f.relative_to(root).as_posix(),'input_sha256':digest,'bytes':len(raw)}
        try:
            result=table_inspect.decode(raw,schemas,game,pin,strict_schema=False)
            row['headers']=[{k:v for k,v in h.items() if k!='records'} for h in result['headers']]
            summary.update(h['status'] for h in result['headers'])
            summary['decoded_records']+=sum(h['count'] for h in result['headers'] if h['status']=='decoded')
        except (ValueError,KeyError) as error:
            row['container_refusal']=str(error);summary['container_refused']+=1
        with f.open('rb') as stream:
            if hashlib.file_digest(stream,'sha256').hexdigest()!=digest:
                raise ValueError('Table input changed during coverage scan')
        reports.append(row)
    result={'format':'fdkss.table_coverage','version':1,'game':game,'source':str(root),
            'tables':len(files),'schemas':len(schemas),'schema_provenance':pin,'summary':dict(summary),
            'files':reports,'originals_unchanged':True,'native_loading_verified':False,
            'state':'container_scan_complete' if not summary['container_refused'] else 'scan_complete_with_container_refusals',
            'limits':'Every file is inventoried; unknown/incompatible field annotations remain raw rather than being guessed. This is serialized coverage, not runtime table semantics.'}
    (work/'coverage.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('input_root',type=Path);p.add_argument('--game',choices=list(table_inspect.PROFILES),required=True)
    p.add_argument('--workspace',type=Path,required=True);p.add_argument('--schemas',type=Path)
    a=p.parse_args()
    try:r=scan(a.input_root,a.game,a.workspace,schemas_root=a.schemas)
    except (ValueError,OSError) as e:p.exit(1,str(e)+'\n')
    print(f"{r['state']}: {r['tables']} tables; {a.workspace/'coverage.json'}")


if __name__=='__main__':main()
