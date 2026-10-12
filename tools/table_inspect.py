"""Read-only bounded #TBL inspection with explicitly selected pinned FalcomSchema.

Container fields checked against original Ys X and installed Daybreak tables and the
MIT KuroTools tbl2json reader. Schema semantics remain upstream annotations.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

MAX_SIZE = 64 * 1024 * 1024
MAX_RECORDS = 100_000
MAX_VALUES = 1_000_000
MAX_STRING = 1024 * 1024
MAX_TEXT = 64 * 1024 * 1024
SCALARS = {'u8': '<B', 's8': '<b', 'u16': '<H', 's16': '<h', 'u32': '<I', 'u64': '<Q', 's32': '<i', 's64': '<q', 'f32': '<f'}
PROFILES = {'ysx': 'ys_X', 'daybreak1': 'ed9_Daybreak1', 'daybreak2': 'ed9_Daybreak2',
            'ysxpn': None, 'sky1': None, 'sky2': None, 'kyoto': None}
ALIASES = {'ConstantValueI': 'IncValueI', 'ConstantValueF': 'IncValueF', 'ConstantValueS': 'IncValueS'}


def width(datatype, depth=0):
    if depth > 16:
        raise ValueError('Schema nesting budget exceeded')
    if isinstance(datatype, str):
        if datatype in SCALARS:
            return struct.calcsize(SCALARS[datatype])
        if datatype in ('ptr_str_utf8', 'ptr_str_latin1'):
            return 8
        if datatype in ('arr_u8', 'arr_u16', 'arr_u32'):
            return 12
        if datatype.startswith('d') and datatype[1:].isdigit() and 0 < int(datatype[1:]) <= MAX_SIZE:
            return int(datatype[1:])
        raise ValueError(f'Unsupported schema type {datatype}')
    if not isinstance(datatype, dict) or set(datatype) != {'repeat', 'type'} or not isinstance(datatype['type'], (dict, str)):
        raise ValueError('Unsupported schema composite')
    repeat = datatype['repeat']
    if type(repeat) is not int or not 0 <= repeat <= 1024:
        raise ValueError('Schema repeat budget exceeded')
    child = datatype['type']
    result = repeat * (sum(width(v, depth + 1) for v in child.values()) if isinstance(child, dict) else width(child, depth + 1))
    if result > MAX_SIZE:
        raise ValueError('Schema width budget exceeded')
    return result


def decode(data, schemas, game, schema_manifest=None, *, strict_schema=True):
    if game not in PROFILES:
        raise ValueError('Unknown explicit game schema profile')
    if schema_manifest is not None and schema_manifest.get('selected_game', schema_manifest.get('game')) != game:
        raise ValueError('Schema manifest does not match selected game')
    if not 8 <= len(data) <= MAX_SIZE or data[:4] != b'#TBL':
        raise ValueError('Expected bounded unencrypted #TBL input')
    count, = struct.unpack_from('<I', data, 4)
    if count > 4096 or 8 + count * 80 > len(data):
        raise ValueError('Invalid table directory bounds')
    headers, occurrences, total = [], {}, 0
    for i in range(count):
        offset = 8 + i * 80
        raw = data[offset:offset + 64]
        end = raw.find(b'\0')
        if end < 1 or any(raw[end:]):
            raise ValueError('Invalid table header name')
        name = raw[:end].decode('utf-8')
        occurrence = occurrences.get(name, 0)
        occurrences[name] = occurrence + 1
        unknown, start, length, entries = struct.unpack_from('<4I', data, offset + 64)
        total += entries
        if total > MAX_RECORDS or not length or start < 8 + count * 80 or start > len(data) or entries * length > len(data) - start:
            raise ValueError('Invalid table record bounds/budget')
        headers.append(dict(name=name, directory_index=i, name_occurrence=occurrence,
                            unknown_word=unknown, start=start, record_size=length, count=entries))
    ranges = sorted((h['start'], h['start'] + h['count'] * h['record_size']) for h in headers if h['count'])
    if any(a < prev_b for (_, prev_b), (a, _) in zip(ranges, ranges[1:])):
        raise ValueError('Overlapping table sections')
    tail_start = max((b for a, b in ranges), default=8 + count * 80)
    budget = MAX_VALUES
    text_budget = MAX_TEXT
    def value(datatype, offset):
        nonlocal budget, text_budget
        budget -= 1
        if budget < 0:
            raise ValueError('Decoded value budget exceeded')
        if isinstance(datatype, dict):
            records = []
            for _ in range(datatype['repeat']):
                if isinstance(datatype['type'], str):
                    item, offset = value(datatype['type'], offset)
                    records.append(item)
                    continue
                row = {}
                for key, child in datatype['type'].items():
                    row[key], offset = value(child, offset)
                records.append(row)
            return records, offset
        if datatype in SCALARS:
            result, = struct.unpack_from(SCALARS[datatype], data, offset)
            if datatype == 'f32' and not math.isfinite(result):
                raise ValueError('Nonfinite table float')
            return result, offset + width(datatype)
        if datatype.startswith('d'):
            size = width(datatype)
            return {'raw_hex': data[offset:offset + size].hex()}, offset + size
        pointer, = struct.unpack_from('<Q', data, offset)
        if datatype in ('ptr_str_utf8', 'ptr_str_latin1'):
            if pointer == 0:
                return None, offset + 8
            if not tail_start <= pointer < len(data):
                raise ValueError('String pointer outside trailing data')
            end = data.find(b'\0', pointer, min(len(data), pointer + MAX_STRING))
            if end < 0:
                raise ValueError('Unterminated or over-budget table string')
            text_budget -= end - pointer
            if text_budget < 0:
                raise ValueError('Decoded string budget exceeded')
            return data[pointer:end].decode('utf-8' if datatype == 'ptr_str_utf8' else 'latin1'), offset + 8
        entries, = struct.unpack_from('<I', data, offset + 8)
        if entries > budget or entries > MAX_VALUES:
            raise ValueError('Array value budget exceeded')
        budget -= entries
        element_type = datatype[4:]
        element_size = width(element_type)
        if entries and (pointer < tail_start or pointer > len(data) or entries * element_size > len(data) - pointer):
            raise ValueError('Array pointer/count outside trailing data')
        return list(struct.unpack_from('<' + str(entries) + SCALARS[element_type][1:], data, pointer)) if entries else [], offset + 12

    for header in headers:
        schema_name = ALIASES.get(header['name'], header['name'])
        entry = schemas.get(schema_name)
        header['records'] = []
        if entry is None:
            header['status'] = 'raw_unknown_schema'
        else:
            candidates = entry if isinstance(entry, list) else [entry]
            matching = [candidate for candidate in candidates if
                        sum(width(v) for v in candidate[0]['schema'].values()) == header['record_size']]
            schema, identity = matching[0] if matching else candidates[0]
            if not isinstance(schema, dict) or schema.get('version') != 1 or not isinstance(schema.get('schema'), dict):
                raise ValueError('Unsupported FalcomSchema document')
            fields = schema['schema']
            expected = sum(width(v) for v in fields.values())
            header['schema'] = dict(name=schema_name, **identity)
            header['schema_width'] = expected
            header['status'] = 'decoded' if expected == header['record_size'] else 'raw_schema_size_mismatch'
            incompatible = (schema_manifest or {}).get('known_incompatible', {}).get(game, {}).get(schema_name)
            if incompatible and identity.get('source') not in ('KuroTools', 'FDKss validated overlay'):
                header['status'] = 'raw_schema_unvalidated_layout'
                header['schema_refusal'] = incompatible
        for i in range(header['count']):
            start = header['start'] + i * header['record_size']
            row = dict(raw_hex=data[start:start + header['record_size']].hex())
            if header['status'] == 'decoded':
                offset, fields_out = start, {}
                try:
                    for key, datatype in fields.items():
                        fields_out[key], offset = value(datatype, offset)
                    row['fields'] = fields_out
                except (ValueError, struct.error) as error:
                    if strict_schema:
                        raise
                    header['status'] = 'raw_schema_decode_refused'
                    header['schema_refusal'] = str(error)
                    for previous in header['records']:
                        previous.pop('fields', None)
            header['records'].append(row)
    return dict(format='fdkss.table_inspection', version=1, game=game,
                input_sha256=hashlib.sha256(data).hexdigest(), headers=headers,
                trailing_data_start=tail_start, trailing_data_hex=data[tail_start:].hex(),
                schema_manifest=schema_manifest,
                limitations=['Read-only explicitly selected serialized game data; no game loading or runtime layout approval.',
                             'Upstream field names are annotations, not independently established gameplay semantics.',
                             'Unknown/mismatched schemas remain raw; original records and trailing bytes are retained.'])


def load_schemas(root, game='ysx'):
    root = Path(root)
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if game not in PROFILES or manifest.get('revision') != '7a545e7205b2067990005d15518285456e66d1c9':
        raise ValueError('Expected pinned explicit game schema manifest')
    profiles = manifest.get('profiles', {'ysx': manifest.get('files', {})})
    if game not in profiles:
        raise ValueError('Requested schema profile is absent')
    result = {}
    common = {}
    def read_files(folder, files):
      output = {}
      for name, expected in files.items():
        if Path(name).name != name or not name.endswith('.json'):
            raise ValueError('Unsafe schema manifest filename')
        path = root / folder / name
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Schema identity differs from pinned manifest')
        output[path.stem] = (json.loads(raw), dict(source='FalcomSchema', sha256=expected, source_file=f'{folder}/{name}'))
      return output
    common = read_files('common', manifest.get('common', {}))
    result = read_files(PROFILES[game], profiles[game])
    def resolve(datatype, stack=()):
        if len(stack) > 16:
            raise ValueError('Schema reference nesting budget exceeded')
        if isinstance(datatype, str) and datatype.startswith('ref_'):
            name = datatype[4:]
            if name in stack or name not in common:
                raise ValueError('Unknown or cyclic common schema reference')
            schema = common[name][0]
            if schema.get('version') != 1:
                raise ValueError('Unsupported common schema version')
            return {'repeat': 1, 'type': {k: resolve(v, stack + (name,)) for k, v in schema['schema'].items()}}
        if isinstance(datatype, dict):
            if not set(datatype) <= {'repeat', 'type'} or 'type' not in datatype:
                raise ValueError('Unsupported schema composite')
            child = datatype['type']
            if isinstance(child, str) and child.startswith('ref_'):
                child = resolve(child, stack)['type']
            elif isinstance(child, dict):
                child = {k: resolve(v, stack) for k, v in child.items()}
            return {'repeat': datatype.get('repeat', 1), 'type': child}
        return datatype
    result = {name: (dict(schema, schema={k: resolve(v) for k,v in schema['schema'].items()}), identity)
              for name, (schema, identity) in result.items()}
    return result, dict(manifest, selected_game=game)


def load_game_schemas(game, root=None):
    """Combine pinned sources only within an explicitly selected game profile.

    FalcomSchema is preferred when its size matches. Alternate same-game KuroTools
    definitions supply missing sections/layouts; no cross-game size fallback.
    Independently validated FDKss overlays take precedence and retain provenance.
    """
    if game not in PROFILES:
        raise ValueError('Unknown game profile')
    root = Path(root) if root else Path(__file__).parent / 'schemas'
    if PROFILES[game] is not None:
        base, manifest = load_schemas(root / 'falcom', game)
    else:
        base, manifest = {}, {'selected_game': game}
    result = {name: [entry] for name, entry in base.items()}
    peer = root / 'kurotools'
    pin = json.loads((peer / 'manifest.json').read_text(encoding='utf-8'))
    if pin.get('revision') != '8b568364232d6a51b03b85b7ed7723f6a1218750' or game not in pin['profiles']:
        raise ValueError('Expected pinned KuroTools game profile')
    type_map = {'uint':'u32', 'int':'s32', 'ulong':'u64', 'long':'s64',
                'ushort':'u16', 'short':'s16', 'ubyte':'u8', 'byte':'s8', 'float':'f32',
                'toffset':'ptr_str_utf8', 'toffsetlatin-1':'ptr_str_latin1',
                'u8array':'arr_u8', 'u16array':'arr_u16', 'u32array':'arr_u32'}
    def convert(datatype, depth=0):
        if depth > 16:
            raise ValueError('Peer schema nesting budget exceeded')
        if isinstance(datatype, dict):
            if set(datatype) != {'size','schema'}:
                raise ValueError('Unsupported peer schema composite')
            return {'repeat': datatype['size'], 'type': {k: convert(v,depth+1) for k,v in datatype['schema'].items()}}
        if datatype not in type_map:
            raise ValueError('Unsupported peer schema type')
        return type_map[datatype]
    for filename, expected in pin['files'].items():
        if Path(filename).name != filename or not filename.endswith('.json'):
            raise ValueError('Unsafe peer schema filename')
        raw = (peer / 'headers' / filename).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Peer schema identity differs from pin')
        for variant, document in json.loads(raw).items():
            if not isinstance(document,dict) or document.get('game') != pin['profiles'][game]:
                continue
            schema = {'version':1, 'schema':{k:convert(v) for k,v in document['schema'].items()}}
            identity = {'source':'KuroTools','source_file':filename,'variant':variant,'sha256':expected,
                        'revision':pin['revision'],'explicit_game':document['game']}
            result.setdefault(Path(filename).stem,[]).append((schema,identity))
    overlay_root = root / 'overrides'
    overlays = json.loads((overlay_root / 'manifest.json').read_text(encoding='utf-8'))
    if 'patch_file' in overlays:
        filename=overlays['patch_file']
        if Path(filename).name!=filename or hashlib.sha256((overlay_root/filename).read_bytes()).hexdigest()!=overlays['patch_sha256']:
            raise ValueError('Maintained schema patch identity differs from manifest')
    for name, identity in overlays['profiles'].get(game,{}).items():
        filename = identity['file']
        if Path(filename).name != filename:
            raise ValueError('Unsafe overlay filename')
        raw = (overlay_root / filename).read_bytes()
        if hashlib.sha256(raw).hexdigest() != identity['sha256']:
            raise ValueError('Schema overlay identity differs from manifest')
        schema = json.loads(raw)
        if sum(width(v) for v in schema['schema'].values()) != identity['record_size']:
            raise ValueError('Schema overlay width differs from manifest')
        result.setdefault(name,[]).insert(0,(schema,dict(identity,source='FDKss validated overlay')))
    manifest = dict(manifest, peer_manifest=pin, overlay_manifest=overlays)
    return result, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('table', type=Path)
    parser.add_argument('--game', choices=list(PROFILES), required=True)
    parser.add_argument('--schemas', type=Path, default=Path(__file__).parent / 'schemas', help='Pinned schema collection root')
    parser.add_argument('--output', type=Path, required=True, help='New JSON output file')
    parser.add_argument('--strict-schema', action='store_true', help='Refuse the entire input on schema pointer/type errors instead of preserving the section raw')
    args = parser.parse_args()
    try:
        if args.output.exists() or args.output.is_symlink():
            raise ValueError('Output must be a new file')
        if args.table.stat().st_size > MAX_SIZE:
            raise ValueError('Table file exceeds inspection budget')
        schemas, manifest = load_game_schemas(args.game, args.schemas)
        result = decode(args.table.read_bytes(), schemas, args.game, manifest, strict_schema=args.strict_schema)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, ensure_ascii=False, allow_nan=False, indent=2)
            stream.write('\n')
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, str(error) + '\n')
    print(f"Inspected {len(result['headers'])} sections; output: {args.output}")


if __name__ == '__main__':
    main()
