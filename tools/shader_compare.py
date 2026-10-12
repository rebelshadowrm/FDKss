"""Compare pinned external shader-switch datasets with independent signature checks.

Results describe serialized switch similarity, not visual or runtime equivalence.
"""
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

import asset_decode
import mdl
from peer_model_workflow import PIN

PROFILES={'sky1':('sky','000_SKY_1ST'),'sky2':('sky','000_SKY_2ND'),
          'ysx':('ys_x','000_YS_X'),'ysxpn':('ys_x','000_YS_X_PROUD'),
          'daybreak1':('kuro','KURO1'),'daybreak2':('kuro','KURO2'),'kyoto':('kx','000_KX')}


def material_signature(material):
    import xxhash
    raw=b''
    for switch in material['switches']:
        name=switch['name'].encode('utf-8')
        if len(name)>255:raise ValueError('Material switch name exceeds Pascal width')
        raw+=bytes([len(name)])+name+struct.pack('<I',switch['value'])
    return material['shader']+'#'+xxhash.xxh64(raw).hexdigest()


def compare(peer,game,query,output):
    peer,output=Path(peer).resolve(),Path(output).resolve()
    if game not in PROFILES or output.exists() or output.is_relative_to(peer):
        raise ValueError('Expected explicit game and new independent shader output')
    if subprocess.check_output(['git','-C',str(peer),'rev-parse','HEAD'],text=True).strip()!=PIN or subprocess.run(['git','-C',str(peer),'diff','--quiet','HEAD'],capture_output=True).returncode:
        raise ValueError('External shader source differs from pin')
    dataset,restriction=PROFILES[game];file=peer/'misc'/f'{dataset}_shaders.csv'
    if file.stat().st_size>16*1024*1024:raise ValueError('Shader dataset exceeds budget')
    with file.open(newline='',encoding='utf-8') as stream:rows=list(csv.reader(stream))
    header,*rows=rows
    first_switch=header.index(restriction)+1
    while first_switch<len(header) and (header[first_switch].startswith('000_') or header[first_switch] in ('KURO1','KURO2','KAI')):
        first_switch+=1
    if dataset=='kuro':first_switch=4
    if len(rows)>100000 or len(header)>4096 or any(len(row)!=len(header) for row in rows):
        raise ValueError('Invalid shader dataset bounds')
    signatures={row[0]:row[first_switch:] for row in rows}
    if len(signatures)!=len(rows) or query not in signatures:
        raise ValueError('Shader query absent or duplicate in pinned dataset')
    module_path=peer/'misc'/f'{dataset}_find_similar_shaders.py'
    spec=importlib.util.spec_from_file_location('fdkss_external_shader_finder',module_path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    external=module.Shader_db(str(file));external.set_restricted_list(restriction)
    external.sort_shaders_by_similarity(query,restrict_type=True)
    allowed={row[0] for row in rows if row[header.index(restriction)]}
    expected={name:{header[first_switch+i]:value for i,value in enumerate(values) if value!=signatures[query][i]}
              for name,values in signatures.items() if name!=query and name.split('#')[0]==query.split('#')[0]}
    actual={name:diff for group in external.diffs.values() for name,diff in group.items()}
    if actual!=expected:raise ValueError('External shader differences disagree with independent CSV comparison')
    ranked=sorted((name,diff) for name,diff in expected.items() if name in allowed)
    ranked.sort(key=lambda row:(len(row[1]),row[0]))
    result={'format':'fdkss.shader_switch_comparison','version':1,'game':game,'query':query,
            'peer_revision':PIN,'dataset_sha256':hashlib.sha256(file.read_bytes()).hexdigest(),
            'external_script_sha256':hashlib.sha256(module_path.read_bytes()).hexdigest(),
            'independent_differences_equal':True,'candidate_count':len(ranked),
            'closest_candidates':[{'shader':name,'changed_switches':diff} for name,diff in ranked[:10]],
            'native_rendering_verified':False,'limits':'Pinned switch signatures and availability annotations only; similarity does not establish shader interchangeability.'}
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--peer',type=Path,required=True);p.add_argument('--game',choices=list(PROFILES),required=True)
    group=p.add_mutually_exclusive_group(required=True);group.add_argument('--query');group.add_argument('--model',type=Path)
    p.add_argument('--material-index',type=int,default=0);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        query=a.query
        if a.model:
            if a.model.stat().st_size>asset_decode.MAX_SIZE:raise ValueError('Model exceeds budget')
            body,_=asset_decode.decode(a.model.read_bytes());materials=mdl.parse(body).get('materials',[])
            if not 0<=a.material_index<len(materials):raise ValueError('Material index out of bounds')
            query=material_signature(materials[a.material_index])
        r=compare(a.peer,a.game,query,a.output)
    except (ValueError,OSError,subprocess.SubprocessError) as e:p.exit(1,str(e)+'\n')
    print(f"Compared {r['candidate_count']} candidates; {a.output}")


if __name__=='__main__':main()
