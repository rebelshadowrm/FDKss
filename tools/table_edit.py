"""Apply explicit schema-backed edits to a copy while preserving unknown table bytes.

Edits select directory/record identities and require the original SHA-256.
Strings/arrays append trailing data; record locations and unselected data stay
unchanged. Native meanings and game loading remain separate acceptance.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

import table_inspect as tables


def edit(data,schemas,game,pin,document):
    if document.get('input_sha256')!=hashlib.sha256(data).hexdigest():
        raise ValueError('Edit document input identity differs from source')
    operations=document.get('edits')
    if not isinstance(operations,list) or not 1<=len(operations)<=1000:
        raise ValueError('Expected bounded explicit edit list')
    before=tables.decode(data,schemas,game,pin,strict_schema=False)
    expected=json.loads(json.dumps(before['headers']))
    output=bytearray(data);seen=set();values=tables.MAX_VALUES
    def encode(datatype,value):
        nonlocal values
        values-=1
        if values<0:raise ValueError('Edited-value budget exceeded')
        if isinstance(datatype,dict):
            if not isinstance(value,list) or len(value)!=datatype['repeat']:
                raise ValueError('Composite repeat count differs from schema')
            child=datatype['type'];encoded=b''
            for row in value:
                if isinstance(child,str):encoded+=encode(child,row)
                else:
                    if not isinstance(row,dict) or set(row)!=set(child):raise ValueError('Composite fields differ from schema')
                    encoded+=b''.join(encode(t,row[k]) for k,t in child.items())
            return encoded
        if datatype in tables.SCALARS:
            if datatype=='f32':
                if type(value) not in (int,float) or not math.isfinite(value):raise ValueError('Expected finite float edit')
            elif type(value) is not int:raise ValueError('Expected integer scalar edit')
            try:encoded=struct.pack(tables.SCALARS[datatype],value)
            except (struct.error,OverflowError) as error:raise ValueError('Scalar edit exceeds serialized type') from error
            if datatype=='f32' and not math.isfinite(struct.unpack('<f',encoded)[0]):raise ValueError('Float edit overflows float32')
            return encoded
        if datatype in ('ptr_str_utf8','ptr_str_latin1'):
            if value is None:return struct.pack('<Q',0)
            if not isinstance(value,str) or '\0' in value:raise ValueError('String edit must be text without NUL')
            raw=value.encode('utf-8' if datatype=='ptr_str_utf8' else 'latin1')+b'\0'
            if len(raw)>tables.MAX_STRING:raise ValueError('Edited string exceeds budget')
            pointer=len(output);output.extend(raw);return struct.pack('<Q',pointer)
        if datatype in ('arr_u8','arr_u16','arr_u32'):
            if not isinstance(value,list) or len(value)>values:raise ValueError('Edited array exceeds value budget')
            values-=len(value);element=datatype[4:]
            if any(type(v) is not int for v in value):raise ValueError('Array edit values must be integers')
            try:raw=b''.join(struct.pack(tables.SCALARS[element],v) for v in value)
            except struct.error as error:raise ValueError('Array edit value exceeds serialized type') from error
            pointer=len(output);output.extend(raw);return struct.pack('<QI',pointer,len(value))
        raise ValueError('Opaque/unknown schema fields are not editable')
    applied=[]
    def normalized(datatype,value):
        if datatype=='f32':return struct.unpack('<f',struct.pack('<f',value))[0]
        if isinstance(datatype,dict):
            child=datatype['type']
            return [normalized(child,row) if isinstance(child,str) else
                    {k:normalized(t,row[k]) for k,t in child.items()} for row in value]
        return value
    for op in operations:
        if not isinstance(op,dict) or set(op)!={'directory_index','record_index','field','value'}:
            raise ValueError('Edit must name exact directory, record, field and value')
        hi,ri,name=op['directory_index'],op['record_index'],op['field']
        if type(hi) is not int or not 0<=hi<len(before['headers']):raise ValueError('Edit directory index out of bounds')
        header=before['headers'][hi]
        if header['status']!='decoded' or type(ri) is not int or not 0<=ri<header['count'] or not isinstance(name,str):
            raise ValueError('Edit requires a decoded record and exact field')
        identity=(hi,ri,name)
        if identity in seen:raise ValueError('Duplicate edit target')
        seen.add(identity)
        candidates=schemas[tables.ALIASES.get(header['name'],header['name'])]
        candidates=candidates if isinstance(candidates,list) else [candidates]
        schema=next(s for s,i in candidates if i=={k:v for k,v in header['schema'].items() if k!='name'})
        fields=schema['schema']
        if name not in fields:raise ValueError('Field not present in selected schema')
        offset=header['start']+ri*header['record_size']
        for key,datatype in fields.items():
            if key==name:break
            offset+=tables.width(datatype)
        encoded=encode(fields[name],op['value'])
        if len(encoded)!=tables.width(fields[name]) or len(output)>tables.MAX_SIZE:
            raise ValueError('Edited table size/field width exceeds contract')
        output[offset:offset+len(encoded)]=encoded
        # Compare float edits after serialized float32 rounding, not Python float64.
        expected[hi]['records'][ri]['fields'][name]=normalized(fields[name],op['value'])
        applied.append(dict(op,byte_offset=offset,field_width=len(encoded),schema=header['schema']))
    after=tables.decode(bytes(output),schemas,game,pin,strict_schema=False)
    for old,new in zip(expected,after['headers']):
        if old['status']!=new['status'] or old['start']!=new['start'] or old['record_size']!=new['record_size']:
            raise ValueError('Table section contract changed after edits')
        for a,b in zip(old['records'],new['records']):
            if a.get('fields')!=b.get('fields'):
                raise ValueError('Reinspection differs from exact expected field edits')
    return bytes(output),{'format':'fdkss.table_authoring','version':1,'game':game,
                          'input_sha256':hashlib.sha256(data).hexdigest(),'output_sha256':hashlib.sha256(output).hexdigest(),
                          'edits':applied,'independent_reinspection_equal':True,'native_loading_verified':False,
                          'limits':'Serialized field editing only; unknown records/bytes retained. No gameplay-value or in-game loading approval.'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('table',type=Path);p.add_argument('--game',choices=list(tables.PROFILES),required=True)
    p.add_argument('--edits',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True)
    a=p.parse_args()
    try:
        if a.output.exists() or a.manifest.exists() or a.output.resolve()==a.manifest.resolve() or any(path.resolve().is_relative_to(a.table.resolve().parent) for path in (a.output,a.manifest)):
            raise ValueError('Output/manifest must be new distinct files outside source directory')
        if a.table.stat().st_size>tables.MAX_SIZE or a.edits.stat().st_size>4*1024*1024:raise ValueError('Authoring input exceeds budget')
        source=a.table.read_bytes();schemas,pin=tables.load_game_schemas(a.game)
        output,report=edit(source,schemas,a.game,pin,json.loads(a.edits.read_text(encoding='utf-8')))
        if a.table.read_bytes()!=source:raise ValueError('Source table changed during authoring')
        for path in (a.output,a.manifest):path.parent.mkdir(parents=True,exist_ok=True)
        with a.output.open('xb') as stream:stream.write(output)
        report.update(source=str(a.table.resolve()),output=str(a.output.resolve()),originals_unchanged=True)
        with a.manifest.open('x',encoding='utf-8') as stream:json.dump(report,stream,indent=2);stream.write('\n')
    except (ValueError,OSError,KeyError,StopIteration) as e:p.exit(1,str(e)+'\n')
    print(f"Verified {len(report['edits'])} edits; {a.output}")


if __name__=='__main__':main()
