"""Compare copied external MDL authoring outputs with FDKss's bounded v4 reader.

Supported collision trees are compared independently; unknown sections stay opaque. Byte equality is reported separately
from decoded geometry/material/hierarchy equality and never proves native loading.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import mdl
import collision_inspect
import asset_decode


def chunks(data):
    if len(data) < 12 or len(data) > 128 * 1024 * 1024:
        raise ValueError('Model size out of bounds')
    offset, output = 12, []
    while offset < len(data):
        if offset + 4 == len(data) and data[offset:offset + 4] == b'\xff\xff\xff\xff':
            break
        if offset + 8 > len(data):
            raise ValueError('Truncated MDL chunk header')
        kind, size = struct.unpack_from('<II', data, offset)
        offset += 8
        if size > len(data) - offset:
            raise ValueError('MDL chunk outside file')
        output.append(dict(kind=kind, size=size, sha256=hashlib.sha256(data[offset:offset+size]).hexdigest()))
        offset += size
    return output


def compare(original, rebuilt):
    original_serialized, rebuilt_serialized = original, rebuilt
    original, original_wrappers = asset_decode.decode(original)
    rebuilt, rebuilt_wrappers = asset_decode.decode(rebuilt)
    a, b = mdl.parse(original), mdl.parse(rebuilt)
    def mesh_state(model):
        inspection = mdl.inspection(model)
        return [{k:v for k,v in row.items() if k!='collision_bytes'} for row in inspection['meshes']]
    ca, cb = chunks(original), chunks(rebuilt)
    changed_codes = {}
    normalized_equal = mesh_state(a)==mesh_state(b)
    if not normalized_equal and len(a['meshes'])==len(b['meshes']):
        normalized_equal = True
        for left,right in zip(a['meshes'],b['meshes']):
            if len(left['submeshes'])!=len(right['submeshes']):
                normalized_equal=False;break
            for pa,pb in zip(left['submeshes'],right['submeshes']):
                if len(pa['streams'])!=len(pb['streams']):
                    normalized_equal=False;break
                for sa,sb in zip(pa['streams'],pb['streams']):
                    if {k:v for k,v in sa.items() if k!='data'} != {k:v for k,v in sb.items() if k!='data'}:
                        normalized_equal=False;break
                    da,db=bytes(sa['data']),bytes(sb['data'])
                    if da!=db:
                        if sa['kind'] in (1,2) and sa['stride']==4 and len(da)==len(db):
                            if any(x!=y and {x,y}!={128,129} for x,y in zip(da,db)):
                                normalized_equal=False
                            changed_codes[str(sa['kind'])]=changed_codes.get(str(sa['kind']),0)+sum(x!=y for x,y in zip(da,db))
                        else:
                            normalized_equal=False
            # All non-stream mesh metadata is independently checked by the original inspector.
        if normalized_equal:
            def metadata_state(model):
                rows=mesh_state(model)
                for row in rows:
                    for sub in row['submeshes']:
                        for stream in sub['streams']:
                            if stream['kind'] in (1,2) and stream['stride']==4:
                                stream.pop('sha256',None)
                return rows
            normalized_equal=metadata_state(a)==metadata_state(b)
    return dict(format='fdkss.external_mdl_comparison', version=1,
                original_serialized_sha256=hashlib.sha256(original_serialized).hexdigest(),
                rebuilt_serialized_sha256=hashlib.sha256(rebuilt_serialized).hexdigest(),
                original_wrappers=original_wrappers,rebuilt_wrappers=rebuilt_wrappers,
                original_sha256=hashlib.sha256(original).hexdigest(),
                rebuilt_sha256=hashlib.sha256(rebuilt).hexdigest(), byte_equal=original==rebuilt,
                decoded_mesh_streams_equal=mesh_state(a)==mesh_state(b),
                normalized_stream_values_equal=normalized_equal,
                snorm_endpoint_byte_changes=changed_codes,
                decoded_materials_equal=a.get('materials')==b.get('materials'),
                decoded_hierarchy_equal=a.get('hierarchy')==b.get('hierarchy'),
                raw_chunks_changed=ca!=cb,
                original_chunks=ca, rebuilt_chunks=cb,
                original_collision_sizes=[m['collision_bytes'] for m in mdl.inspection(a)['meshes']],
                rebuilt_collision_sizes=[m['collision_bytes'] for m in mdl.inspection(b)['meshes']],
                collision_structure=collision_inspect.compare(original, rebuilt),
                native_loading_verified=False, collision_semantics_verified=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original',type=Path)
    parser.add_argument('rebuilt',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists() or args.output.is_symlink():parser.error('Output must be a new file')
    for path in (args.original,args.rebuilt):
        if path.stat().st_size>128*1024*1024:parser.error('Model size out of bounds')
    result=compare(args.original.read_bytes(),args.rebuilt.read_bytes())
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps({k:v for k,v in result.items() if k.endswith('_equal') or k.endswith('_verified')}))


if __name__=='__main__':main()
