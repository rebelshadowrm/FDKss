"""Run pinned external model authoring on isolated copies and compare final files.

The peer remains external. A pure-animation version downgrade is diagnosed and
the original supported 1/2/4 tag is restored only after independent section parsing.
No output is installed in a game and native loading remains unverified.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import asset_decode
import compare_mdl
import mdl
import mdl_animation

PIN = 'e44f0990604bbd3fa5b40b85458cd15313a56a75'


def section_data(data):
    r=mdl.Reader(data);magic,version,flags=r.unpack('<4sII')
    if magic!=b'MDL ' or version not in (1,2,4):
        raise ValueError('Expected observed MDL 1/2/4 format')
    result={}
    while True:
        kind=r.u32()
        if kind==0xffffffff:
            r.done();break
        if kind in result:
            raise ValueError('Duplicate model section')
        result[kind]=bytes(r.take(r.u32()))
    return version,result


def animation_compare(a,b):
    a,_=asset_decode.decode(a);b,_=asset_decode.decode(b)
    va,sa=section_data(a);vb,sb=section_data(b)
    if set(sa)!={2,3} or set(sb)!={2,3}:
        raise ValueError('Animation workflow is scoped to pure skeleton/motion MDLs')
    normalized=lambda data:data[:4]+struct.pack('<I',4)+data[8:]
    ia,ib=mdl_animation.inspect(normalized(a)),mdl_animation.inspect(normalized(b))
    ha,hb=mdl.hierarchy(sa[2],0),mdl.hierarchy(sb[2],0)
    result = {'original_version':va,'rebuilt_version':vb,'animation_records_equal':ia['records']==ib['records'],
            'animation_trailer_equal':ia['raw_trailer']==ib['raw_trailer'],'hierarchy_equal':ha==hb,
            'sections_byte_equal':sa==sb,'native_loading_verified':False,
            'comparison_scope':'Observed pure-animation storage layout 1/2/4; header normalization is diagnostic parsing, not native version compatibility.'}
    def metadata(motion):
        return sorted((r['target'],r['raw_kind'],r['raw_words'],
                       [(k['raw_coordinate'],k['raw_extra_hex']) for k in r['keys']]) for r in motion['records'])
    result['key_metadata_equal']=metadata(ia)==metadata(ib)
    if not result['animation_records_equal'] or not result['hierarchy_equal']:
        try:
            coordinates=sorted({k['raw_coordinate'] for motion in (ia,ib) for r in motion['records'] for k in r['keys']})
            if not coordinates:raise ValueError('No pose sample coordinates')
            if len(coordinates)>128:
                coordinates=[coordinates[i*(len(coordinates)-1)//127] for i in range(128)]
            coordinates=sorted(set(coordinates+[(x+y)/2 for x,y in zip(coordinates,coordinates[1:])]))
            if len(coordinates)*max(len(ha['nodes']),len(hb['nodes']))>250000:
                raise ValueError('Pose comparison budget exceeded')
            error=0.0
            for t in coordinates:
                pa=mdl_animation.sample_pose({'hierarchy':ha},ia,t);pb=mdl_animation.sample_pose({'hierarchy':hb},ib,t)
                if [n['name'] for n in pa['nodes']]!=[n['name'] for n in pb['nodes']] or pa['missing_targets']!=pb['missing_targets']:
                    raise ValueError('Pose target ordering/binding differs')
                for x,y in zip(pa['nodes'],pb['nodes']):
                    if x['kind']!=y['kind'] or x['children']!=y['children']:
                        raise ValueError('Pose hierarchy structure differs')
                    error=max(error,max(abs(i-j) for key in ('local_matrix','world_matrix') for i,j in zip(x[key],y[key])))
            result['pose_comparison']={'samples':len(coordinates),'max_absolute_matrix_error':error,
                                       'tolerance':1e-5,'within_tolerance':error<=1e-5,
                                       'limits':'One unblended clip in raw coordinates; samples do not establish all-coordinate or native clock/blending/rendering equivalence.'}
        except ValueError as error:
            result['pose_comparison']={'refused':str(error),'within_tolerance':False}
    return result


def restore_animation_words(original,rebuilt):
    """Keep source unknown words that the peer's glTF importer resets globally."""
    original,_=asset_decode.decode(original)
    normalized=original[:4]+struct.pack('<I',4)+original[8:]
    source=mdl_animation.inspect(normalized)
    identities={(r['target'],r['raw_kind']):r for r in source['records']}
    if len(identities)!=len(source['records']):
        raise ValueError('Ambiguous source animation metadata')
    result=bytearray(rebuilt);reader=mdl.Reader(rebuilt);reader.take(12);restored=0
    while True:
        kind=reader.u32()
        if kind==0xffffffff:reader.done();break
        size=reader.u32();start=reader.offset;payload=reader.take(size)
        if kind!=3:continue
        tracks=mdl.Reader(payload);count=tracks.u32()
        if count!=len(identities):raise ValueError('glTF animation track set differs from source')
        seen=set()
        for _ in range(count):
            tracks.name();target=tracks.name();offset=tracks.offset
            channel,word1,word2,keys=tracks.unpack('<4I')
            identity=(target,channel)
            if identity in seen or identity not in identities:raise ValueError('glTF animation binding differs from source')
            seen.add(identity)
            source_record=identities[identity]
            if keys!=len(source_record['keys']):raise ValueError('glTF animation key count differs from source')
            if (word1,word2)!=tuple(source_record['raw_words']):
                struct.pack_into('<2I',result,start+offset+4,*source_record['raw_words']);restored+=1
            tracks.take(keys*(4+mdl_animation.WIDTHS[channel]*4+20))
        tracks.take(8);tracks.done()
    return bytes(result),restored


def run(source,peer,workspace,mode,*,compress_after=False,switch_edit=None):
    source,peer,work=Path(source).resolve(),Path(peer).resolve(),Path(workspace).resolve()
    if not source.is_file() or source.stat().st_size>asset_decode.MAX_SIZE or work.exists() or work.is_relative_to(source.parent) or work.is_relative_to(peer):
        raise ValueError('Expected bounded source and new independent workspace')
    revision=subprocess.check_output(['git','-C',str(peer),'rev-parse','HEAD'],text=True).strip()
    if revision!=PIN or subprocess.run(['git','-C',str(peer),'diff','--quiet','HEAD'],capture_output=True).returncode:
        raise ValueError('External source differs from pinned clean revision')
    original=source.read_bytes();decoded,wrappers=asset_decode.decode(original)
    version,sections=section_data(decoded)
    expected_materials = None
    if switch_edit is not None:
        if mode!='mesh' or version!=4:
            raise ValueError('Material switch authoring is verified for MDL v4 mesh inputs')
        index,name,value=switch_edit
        expected_materials=copy.deepcopy(mdl.parse(decoded).get('materials',[]))
        if type(index) is not int or not 0<=index<len(expected_materials) or type(value) is not int or not -(2**31)<=value<2**31:
            raise ValueError('Material switch edit index/value out of bounds')
        selected=[s for s in expected_materials[index]['switches'] if s['name']==name]
        if len(selected)!=1:raise ValueError('Material switch must match exactly one existing switch')
        selected[0]['value']=value & 0xffffffff
    if mode in ('animation-json','animation-gltf'):
        animation_compare(original,original)
    elif mode=='mesh':
        if version==4:mdl.parse(decoded)
        if 1 not in sections:
            raise ValueError('Model has no mesh section')
    else:
        raise ValueError('Unsupported external authoring operation')
    work.mkdir(parents=True)
    copied=work/source.name;shutil.copy2(source,copied)
    manifest={'format':'fdkss.external_model_workflow','version':1,'mode':mode,
              'source':str(source),'source_sha256':hashlib.sha256(original).hexdigest(),
              'peer':str(peer),'peer_revision':PIN,'commands':[],'state':'prepared',
              'native_loading_verified':False,'source_wrappers':wrappers}
    def save():(work/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    def invoke(script,arguments,label):
        path=peer/script
        command=[sys.executable,str(path),*arguments,str(copied)]
        result=subprocess.run(command,capture_output=True,input=b'',timeout=120)
        (work/(label+'.log')).write_bytes(result.stdout+result.stderr)
        manifest['commands'].append({'argv':command,'returncode':result.returncode,
                                     'script_sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
        save()
        if result.returncode:raise ValueError(f'External {label} failed; see retained log')
    try:
        if mode=='mesh':
            invoke('kuro_mdl_export_meshes.py',['-o'],'export')
            if switch_edit is not None:
                index,name,value=switch_edit
                metadata=copied.with_suffix('')/'material_info.json'
                records=json.loads(metadata.read_text(encoding='utf-8'))
                selected=[s for s in records[index]['material_switches'] if s['material_switch_name']==name]
                if len(selected)!=1:raise ValueError('External switch metadata differs from independent parse')
                selected[0]['int2']=value
                metadata.write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
                manifest['material_switch_edit']={'material_index':index,'switch':name,'value':value,
                                                 'metadata_sha256':hashlib.sha256(metadata.read_bytes()).hexdigest()}
            invoke('kuro_mdl_import_meshes.py',[],'import')
        else:
            invoke('kuro_mdl_to_basic_gltf.py',['-d','-o'],'export')
            invoke('kuro_mdl_import_animation.py',['-j'] if mode=='animation-json' else [],'import')
            rebuilt,rebuilt_wrappers=asset_decode.decode(copied.read_bytes())
            new_version,new_sections=section_data(rebuilt)
            if version!=new_version:
                # The peer's Kuro-specific downgrade is incompatible with preserving the input build's tag.
                animation_compare(original,rebuilt)
                restored=rebuilt[:4]+struct.pack('<I',version)+rebuilt[8:]
                manifest['version_restoration']={'peer_output_version':new_version,'preserved_source_version':version,
                                                'peer_output_sha256':hashlib.sha256(copied.read_bytes()).hexdigest(),
                                                'scope':'Independently parsed pure-animation sections; original serialized version preserved.'}
                copied.write_bytes(restored)
                if rebuilt_wrappers:
                    invoke('cle_compress.py',[],'restore-compression')
            if mode=='animation-gltf':
                rebuilt,rebuilt_wrappers=asset_decode.decode(copied.read_bytes())
                restored,count=restore_animation_words(original,rebuilt)
                if count:
                    copied.write_bytes(restored)
                    manifest['animation_metadata_restoration']={'tracks':count,'scope':'Preserve original unknown words by unique target/channel; key count and bindings must match.'}
                    if rebuilt_wrappers:invoke('cle_compress.py',[],'restore-metadata-compression')
        if compress_after:
            invoke('cle_compress.py',[],'compress')
        rebuilt=copied.read_bytes();body,output_wrappers=asset_decode.decode(rebuilt)
        new_version,new_sections=section_data(body)
        if mode in ('animation-json','animation-gltf'):
            comparison=animation_compare(original,rebuilt)
            valid=all(comparison[k] for k in ('animation_records_equal','animation_trailer_equal','hierarchy_equal','sections_byte_equal')) and version==new_version
            if mode=='animation-gltf':
                valid=(valid or (comparison.get('pose_comparison',{}).get('within_tolerance',False) and
                                 comparison['key_metadata_equal'] and comparison['animation_trailer_equal'])) and version==new_version
        elif version==new_version==4:
            comparison=compare_mdl.compare(original,rebuilt)
            valid=all(comparison[k] for k in ('normalized_stream_values_equal','decoded_materials_equal','decoded_hierarchy_equal'))
            if expected_materials is not None:
                comparison['authored_materials_match_expected']=mdl.parse(body).get('materials')==expected_materials
                valid=all(comparison[k] for k in ('normalized_stream_values_equal','decoded_hierarchy_equal','authored_materials_match_expected'))
        else:
            comparison={'original_version':version,'rebuilt_version':new_version,'sections_byte_equal':sections==new_sections,
                        'scope':'Raw section comparison; no FDKss typed mesh reader for earlier versions','native_loading_verified':False}
            valid=comparison['sections_byte_equal'] and version==new_version
        if source.read_bytes()!=original:
            raise ValueError('Original model changed')
        (work/'comparison.json').write_text(json.dumps(comparison,indent=2)+'\n',encoding='utf-8')
        manifest.update(state='offline_verified' if valid else 'offline_difference_found',
                        output=str(copied),output_sha256=hashlib.sha256(rebuilt).hexdigest(),
                        output_wrappers=output_wrappers,originals_unchanged=True,
                        final_byte_equal=original==rebuilt,comparison=str(work/'comparison.json'))
        save();return manifest
    except Exception as error:
        manifest.update(state='failed',failure=str(error));save();raise


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('--peer',type=Path,required=True)
    p.add_argument('--workspace',type=Path,required=True);p.add_argument('--mode',choices=['mesh','animation-json','animation-gltf'],required=True)
    p.add_argument('--compress-after',action='store_true')
    p.add_argument('--material-index',type=int,default=0);p.add_argument('--switch-name');p.add_argument('--switch-value',type=int)
    a=p.parse_args()
    if (a.switch_name is None)!=(a.switch_value is None):p.error('--switch-name and --switch-value must be supplied together')
    edit=(a.material_index,a.switch_name,a.switch_value) if a.switch_name is not None else None
    try:r=run(a.source,a.peer,a.workspace,a.mode,compress_after=a.compress_after,switch_edit=edit)
    except (ValueError,OSError,subprocess.SubprocessError) as e:p.exit(1,str(e)+'\n')
    print(f"{r['state']}: {r['output']}")


if __name__=='__main__':main()
