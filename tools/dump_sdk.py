"""Export FDK MSVC RTTI, vtables and verified property SDKs without running a game."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


SKY1_HASH = '4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf'
SKY2_HASH = 'd8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf'
ROOT_TYPES = {
    'application': '.?AVApplication@sora@@', 'framework': '.?AVFramework@fdk@@',
    'fieldmap': '.?AVManager@fieldmap@sora@@', 'scene': '.?AVScene@fieldmap@sora@@',
    'player_controller': '.?AVPlayer@fieldmap@sora@@', 'player': '.?AVActor@chara@sora@@',
    'actor': '.?AVActor@fdk@@',
    'script_manager': '.?AVManager@script@sora@@',
    'player_model': '.?AVModel@fdk@@',
}


def verified_native_scalars(pe, game):
    profiles = {
        'sky1': (0x3d7620, 0x817cf0, 'abec2cb1908714dc80320f08449a2742eaa7fcbd003f2285f9f5c5169c508973'),
        'sky2': (0x4c4510, 0xb30f70, '2f33140eb2e02c6836c9f6e05b61cbf1dcb75dedd7d617203bff6b1a9925cb87'),
    }
    if game not in profiles or pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('native scalar export requires a supported AMD64 profile')
    handler, sign_mask, digest = profiles[game]
    if not any(s.Characteristics & 0x20000000 and s.VirtualAddress <= handler and
               handler + 139 <= s.VirtualAddress + s.Misc_VirtualSize for s in pe.sections):
        raise ValueError('native absolute-value handler is outside executable sections')
    code = pe.get_data(handler, 139)
    if len(code) != 139 or hashlib.sha256(code).hexdigest() != digest:
        raise ValueError('native absolute-value handler body changed')
    if handler + 106 + struct.unpack_from('<i', code, 102)[0] != sign_mask or pe.get_data(sign_mask, 16) != struct.pack('<4I', *([0x7fffffff] * 4)):
        raise ValueError('native absolute-value sign mask changed')
    return [dict(binding='engine.native_abs', class_name='.?AVCommandSys@script@sora@@',
                 group=0, command=39, handler_rva=hex(handler), verified_signature=True,
                 native_abi='RCX=command object; RDX=Thread storage; native return register ignored',
                 scalar_result='Copied tagged scalar from Thread+0x14', input_count=1,
                 input_types=['signed integer in [-536870911,536870911]', 'finite VM float'],
                 body_size=len(code), body_sha256=digest, sign_mask_rva=hex(sign_mask),
                 limitations='One pinned scalar signature; no arbitrary native or actor-context execution.')]


def verified_native_selections(pe, game):
    entries = {'sky1': (0x3d7310, 0x3d7430), 'sky2': (0x4c4200, 0x4c4320)}
    if game not in entries or pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('native selection export requires a supported AMD64 profile')
    source = Path(__file__).resolve().parents[1] / 'src/windows/native_select_profiles.inc'
    if not source.is_file():
        source = Path(__file__).with_name('native_select_profiles.inc')
    pins = source.read_text()
    rows = []
    for operation, command, handler in zip(('min', 'max'), (25, 26), entries[game]):
        match = re.search(operation + r'_code\[\]="([0-9a-f]+)"', pins)
        if not match:
            raise ValueError('missing native selection body pin')
        code = bytes.fromhex(match[1])
        if len(code) != 280 or pe.get_data(handler, len(code)) != code or not any(
                section.Characteristics & 0x20000000 and section.VirtualAddress <= handler and
                handler + len(code) <= section.VirtualAddress + section.Misc_VirtualSize for section in pe.sections):
            raise ValueError('native selection body changed or is outside executable sections')
        rows.append(dict(binding='engine.native_' + operation, class_name='.?AVCommandSys@script@sora@@',
            group=0, command=command, handler_rva=hex(handler), verified_signature=True,
            native_abi='RCX=command object; RDX=private Thread storage; native return register ignored',
            scalar_result='Selected original tagged argument copied from Thread+0x14; ties choose second argument',
            input_count=2, input_types=['signed 30-bit integer', 'finite normal/zero VM float'],
            comparison='Integer comparison for two integers; float32 conversion for mixed/float inputs',
            body_size=len(code), body_sha256=hashlib.sha256(code).hexdigest(),
            limitations='Direct private-storage and bounded synchronous VM invocation; no async/wait, arbitrary native or actor-context execution.'))
    return rows


def verified_native_square_roots(pe, game):
    entries = {'sky1': 0x3d71c0, 'sky2': 0x4c40b0}
    if game not in entries or pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('native sqrt export requires a supported AMD64 profile')
    source = Path(__file__).resolve().parents[1] / 'src/windows/native_sqrt_profiles.inc'
    if not source.is_file():
        source = Path(__file__).with_name('native_sqrt_profiles.inc')
    match = re.search('sqrt_' + game + r'_code\[\]="([0-9a-f]+)"', source.read_text())
    if not match:
        raise ValueError('missing native sqrt body pin')
    code = bytes.fromhex(match[1]); handler = entries[game]
    if len(code) != 138 or pe.get_data(handler, len(code)) != code or not any(
            section.Characteristics & 0x20000000 and section.VirtualAddress <= handler and
            handler + len(code) <= section.VirtualAddress + section.Misc_VirtualSize for section in pe.sections):
        raise ValueError('native sqrt body changed or is outside executable sections')
    return [dict(binding='engine.native_sqrt', class_name='.?AVCommandSys@script@sora@@',
        group=0, command=23, handler_rva=hex(handler), verified_signature=True, input_count=1,
        native_abi='RCX=command object; RDX=private Thread storage; native return register ignored',
        scalar_result='Copied tagged VM float from Thread+0x14',
        input_types=['nonnegative signed 30-bit integer', 'nonnegative finite normal/zero VM float'],
        body_size=len(code), body_sha256=hashlib.sha256(code).hexdigest(),
        limitations='Negative domain and external CRT error path refused; no async/wait or actor-context invocation.')]


def verified_native_resource_setters(pe, game):
    source = Path(__file__).resolve().parents[1] / 'src/windows/resource_setter_profiles.inc'
    # Installed SDK tools carry the same pins beside the exporter.
    if not source.is_file():
        source = Path(__file__).with_name('resource_setter_profiles.inc')
    match = re.search(r'inline constexpr SetterProfile\s+' + game + r'_setter\{(0x[0-9a-f]+),\s*((?:"[0-9a-f]+"\s*)+)\};', source.read_text())
    if not match:
        raise ValueError('missing native resource setter profile')
    entry = int(match[1], 16)
    code = bytes.fromhex(''.join(re.findall(r'"([0-9a-f]+)"', match[2])))
    if pe.get_data(entry, len(code)) != code:
        raise ValueError('native resource setter body or dispatch tables changed')
    return [dict(binding='engine.set_player_resource', entry_rva=hex(entry),
                 verified_signature=True, body_and_tables_size=len(code),
                 body_and_tables_sha256=hashlib.sha256(code).hexdigest(),
                 native_abi='RCX=status data; EDX=selector; R8D=signed 32-bit requested value; native return ignored',
                 selected_paths=[dict(name=name,selector=selector,status_offset=hex(offset)) for name,selector,offset in
                                 [('hp',7,0xc),('ep',9,0x14),('cp',11,0x1c)]],
                 result='Requested resource clamped to [0, maximum], copied after native execution',
                 observation=dict(hook='status.resource_change', observer_only=True, selectors=list(range(7,13)),
                     operations={'set':[7,9,11],'add':[8,10,12]},
                     additive_semantics='32-bit wrap before signed clamp; copied post value is authoritative',
                     pre_arguments=['diagnostic_status_address','opaque_status_word','level','resource','operation','signed_request','before','before_maximum'],
                     post_arguments=['same copied pre arguments','after_or_nil','after_maximum_or_nil','error_if_copy_failed'],
                     limitations='Owner-thread native game calls only; copied values, no persistent receiver; no argument/result replacement. Explicit native APIs bypass observers.'),
                 limitations='Only the three selected paths are callable. Status has no verified RTTI or public pointer handle.')]


def verified_native_status_getters(pe, game):
    if game not in ('sky1', 'sky2') or pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('native status export requires a supported AMD64 profile')
    source = Path(__file__).resolve().parents[1] / 'src/windows/player_resources_profiles.inc'
    if not source.is_file():
        source = Path(__file__).with_name('player_resources_profiles.inc')
    match = re.search(r'inline constexpr Profile\s+' + game +
                      r'\{(0x[0-9a-f]+),(0x[0-9a-f]+),(0x[0-9a-f]+),\s*"([0-9a-f]+)"\};', source.read_text())
    if not match:
        raise ValueError('missing native status getter profile')
    entry = int(match[3], 16)
    code = bytes.fromhex(match[4])
    if pe.get_data(entry, len(code)) != code:
        raise ValueError('native status getter body or dispatch tables changed')
    if not any(s.Characteristics & 0x20000000 and s.VirtualAddress <= entry and
               entry + len(code) <= s.VirtualAddress + s.Misc_VirtualSize for s in pe.sections):
        raise ValueError('native status getter is outside executable sections')
    table, indices = (108, 156) if game == 'sky1' else (140, 196)
    paths = []
    for selector, offset in [(0,4),(7,12),(9,20),(11,28),(80,36),(81,40),(82,44),(83,48),(84,52)]:
        target = struct.unpack_from('<I', code, table + code[indices + selector]*4)[0]
        if not entry <= target <= entry + len(code) - 4 or code[target-entry:target-entry+4] != bytes([0x8b,0x41,offset,0xc3]):
            raise ValueError('native status selected leaf path changed')
        paths.append(dict(selector=selector, status_offset=hex(offset), branch_rva=hex(target)))
    return [dict(binding='engine.character_status_value', owner_class=ROOT_TYPES['player'],
                 entry_rva=hex(entry), verified_signature=True,
                 signature='std::uint32_t (const void* status, std::uint32_t selector)',
                 native_abi='RCX=transient status pointer; EDX=selector; EAX=raw uint32 result',
                 body_and_tables_size=len(code), body_and_tables_sha256=hashlib.sha256(code).hexdigest(),
                 selected_paths=paths, effects='Selected paths read one uint32; no writes or external calls.',
                 limitations='Only nine selected paths; no Status RTTI, retained pointers, named stat semantics or other-selector ABI claims.')]


def recover(pe):
    image = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    size = len(image)

    def unpack(fmt, rva):
        length = struct.calcsize(fmt)
        if rva < 0 or rva + length > size:
            raise ValueError('out of image')
        return struct.unpack_from(fmt, image, rva)

    def name(rva):
        if not 0 < rva < size - 16:
            raise ValueError('invalid type descriptor')
        end = image.find(b'\0', rva + 16, min(size, rva + 16 + 512))
        if end < 0:
            raise ValueError('unterminated RTTI name')
        value = image[rva + 16:end].decode('ascii')
        if not re.fullmatch(r'\.\?A[VU][A-Za-z0-9_?$@]+', value):
            raise ValueError('invalid RTTI name')
        return value

    code_ranges = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                   for s in pe.sections if s.Characteristics & 0x20000000]

    def executable(rva):
        return any(start <= rva < end for start, end in code_ranges)

    classes, locators = {}, {}
    for section in pe.sections:
        if not section.Characteristics & 0x40000000 or section.Characteristics & 0x20000000:
            continue
        start = section.VirtualAddress
        stop = min(size - 24, start + min(section.Misc_VirtualSize, section.SizeOfRawData))
        for rva in range(start, stop + 1, 4):
            if image[rva:rva + 4] != b'\x01\0\0\0':
                continue
            signature, offset, constructor, td, hierarchy, self_rva = unpack('<6I', rva)
            if signature != 1 or self_rva != rva:
                continue
            try:
                type_name = name(td)
                hs, attributes, count, array = unpack('<4I', hierarchy)
                if hs != 0 or not 1 <= count <= 1024:
                    continue
                bases = []
                for index in range(count):
                    descriptor, = unpack('<I', array + index * 4)
                    base_td, contained, member, vbtable, displacement, flags = unpack('<IIiiiI', descriptor)
                    bases.append({'class': name(base_td), 'contained_bases': contained,
                                  'member_displacement': member, 'vbtable_displacement': vbtable,
                                  'virtual_displacement': displacement, 'attributes': flags})
            except (ValueError, UnicodeDecodeError, struct.error):
                continue
            entry = classes.setdefault(type_name, {'name': type_name, 'type_descriptor_rva': hex(td),
                                                   'bases': bases, 'vtables': []})
            locators[rva] = (entry, offset, constructor)

    # Vtable[-1] stores an absolute COL pointer. Enumerate image-aligned references.
    for section in pe.sections:
        if section.Characteristics & 0x20000000 or not section.Characteristics & 0x40000000:
            continue
        start = (section.VirtualAddress + 7) & ~7
        stop = min(size - 16, section.VirtualAddress + min(section.Misc_VirtualSize, section.SizeOfRawData))
        for rva in range(start, stop, 8):
            locator_va, = unpack('<Q', rva)
            locator = locators.get(locator_va - base)
            if not locator:
                continue
            methods = []
            for index in range(512):
                if rva + 8 + index * 8 + 8 > size:
                    break
                va, = unpack('<Q', rva + 8 + index * 8)
                if not executable(va - base):
                    break
                methods.append({'slot': index, 'rva': hex(va - base), 'signature': 'unknown'})
            if methods:
                entry, offset, constructor = locator
                entry['vtables'].append({'rva': hex(rva + 8), 'subobject_offset': offset,
                                         'constructor_displacement': constructor,
                                         'methods': methods,
                                         'termination': 'bounded executable-pointer scan; boundary unverified'})
    return sorted(classes.values(), key=lambda entry: entry['name'])


def verified_properties(schema):
    pattern = re.compile(r'^FDKSS_PROPERTY\((\w+), (\w+), (0x[0-9a-f]+), (FDKSS_\w+), (\w+), (true|false)\)$')
    properties = []
    for line in schema.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('//'):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError(f'invalid schema entry: {line}')
        root, name, offset, kind, storage, writable = match.groups()
        properties.append({'root': root, 'class': ROOT_TYPES[root], 'name': name,
                           'offset': int(offset, 16), 'value_type': kind,
                           'storage': storage, 'writable': writable == 'true', 'verified': True})
        if storage == 'computed_resource':
            status_offset={'hp':12,'max_hp':16,'ep':20,'max_ep':24,'cp':28,'max_cp':32}[name]
            properties[-1].update(computed=True, offset=None, source_offset=int(offset,16),
                status_offset=status_offset, expression='character.resources().' + name,
                constraints='Fresh exact Chara reference, pinned getter, revalidated copied status chain; hp/ep/cp use restricted native clamp setter, maxima read-only.')
        if storage == 'computed_yaw':
            properties[-1].update(computed=True, offset=None, source_offset=int(offset, 16),
                                  expression='remainder(2*atan2(rotation_y,rotation_w),2*pi)',
                                  constraints='Finite upright quaternion; setter preserves position via set_placement; no automatic undo.')
    return properties


def verified_methods(schema):
    pattern = re.compile(r'^FDKSS_METHOD\((\w+), (0x[0-9a-f]+), (0x[0-9a-f]+), (FDKSS_\w+)\)$')
    methods = []
    for line in schema.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('//'):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError(f'invalid method schema entry: {line}')
        name, slot, rva, kind = match.groups()
        result = 'void' if kind == 'FDKSS_NIL' else 'const char*' if kind == 'FDKSS_STRING' else 'std::uint32_t'
        methods.append({'class': ROOT_TYPES['player'], 'name': name, 'slot': int(slot, 16) // 8,
                        'rva': rva, 'value_type': kind, 'cpp_return_type': result,
                        'signature': result + ' (void* actor)', 'verified': True})
        if name == 'set_native_state_byte':
            methods[-1].update(cpp_arguments='void* actor, std::uint8_t value',
                signature='void (void* actor, std::uint8_t value)', lua_arguments=['value'],
                effects='Writes one raw character state byte through the pinned virtual setter; no automatic undo.')
    placement_schema = schema.with_name(schema.name.replace('_methods.inc', '_placement.inc'))
    for line in placement_schema.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'FDKSS_PLACEMENT\((\w+), (0x[0-9a-f]+)\)', line)
        if not match:
            raise ValueError(f'invalid placement schema entry: {line}')
        name, rva = match.groups()
        methods.append({'class': ROOT_TYPES['player'], 'name': name, 'slot': None,
                        'rva': rva, 'value_type': 'FDKSS_NIL', 'cpp_return_type': 'void',
                        'signature': 'void (void* actor, const Placement* placement)',
                        'verified': True, 'lua_arguments': ['x', 'y', 'z', 'yaw_radians'],
                        'effects': 'Sets current/previous position, yaw quaternion and clears interpolation; no automatic undo.'})
    actor_schema = schema.with_name(schema.name.replace('_methods.inc', '_actor_methods.inc'))
    for line in actor_schema.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'FDKSS_ACTOR_METHOD\((\w+), (0x[0-9a-f]+), (0x[0-9a-f]+)\)', line)
        if not match:
            raise ValueError(f'invalid actor method schema entry: {line}')
        name, slot, rva = match.groups()
        methods.append({'class': ROOT_TYPES['actor'], 'name': name, 'slot': int(slot, 16) // 8,
                        'rva': rva, 'value_type': 'FDKSS_NUMBER', 'cpp_return_type': 'const float*',
                        'signature': 'const float* (void* actor)', 'verified': True,
                        'lua_result_count': 16, 'effects': 'Copies 16 finite floats in engine matrix order; no pointer escapes to Lua.'})
    type_schema = schema.with_name(schema.name.replace('_methods.inc', '_actor_types.inc'))
    for line in type_schema.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('//'):
            continue
        match = re.fullmatch(r'FDKSS_ACTOR_TYPE\((0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), "([^"]+)"\)', line)
        if not match:
            raise ValueError(f'invalid actor type schema entry: {line}')
        table, name_rva, id_rva, flags_rva, text, ident, flags, class_name = match.groups()
        for slot, name, rva in ((1, 'type_name', name_rva), (2, 'type_id', id_rva), (3, 'type_flags', flags_rva)):
            if any(m['class'] == class_name and m['name'] == name for m in methods):
                continue
            result = 'const char*' if slot == 1 else 'std::uint32_t'
            symbol = re.match(r'\.\?AV(\w+)@', class_name).group(1) + '_' + name
            methods.append({'class': class_name, 'name': name, 'cpp_name': symbol, 'slot': slot,
                            'rva': rva, 'vtable_rva': table, 'value_type': 'FDKSS_STRING' if slot == 1 else 'FDKSS_INTEGER',
                            'cpp_return_type': result, 'signature': result + ' (void* actor)', 'verified': True,
                            'expected_string_rva': text if slot == 1 else None,
                            'expected_value': None if slot == 1 else int(ident if slot == 2 else flags, 16)})
    return methods


def write_sdk(directory, properties, methods, game='sky1'):
    header = ['// Generated verified layout prefixes. Not complete engine types or safe object handles.',
              '#pragma once', '#include <cstddef>', '#include <cstdint>',
              f'namespace fdkss::{game}::sdk {{']
    lua = ['---@meta', '-- Generated verified properties, including inherited Actor transforms.',
           '---@class FDKClassBase', '---@field name string', '---@field contained_bases integer',
           '---@field member_offset integer', '---@field vbtable_offset integer',
           '---@field virtual_displacement integer', '---@field attributes integer',
           '---@class FDKClassInfo', '---@field name string', '---@field count integer',
           '---@field flags integer', '---@field object_offset integer', '---@field constructor_offset integer',
           '---@field bases FDKClassBase[]',
           '---@class FDKModelInfo', '---@field name string', '---@field format_version integer',
           '---@field mesh_count integer', '---@field node_count integer']
    roots = ROOT_TYPES
    for root in roots:
        fields = sorted((p for p in properties if p['root'] == root), key=lambda p: p['source_offset'] if p.get('computed') else p['offset'])
        lua += [f'---@class FDK_{root}' + (' : FDK_actor' if root == 'player' else ''), '---@field class string|nil', '---@field address string|nil']
        lua += [f'---@field class_info fun(self:FDK_{root}):FDKClassInfo', f'---@field is_a fun(self:FDK_{root}, class_name:string):boolean']
        for p in fields:
            lua_type = {'FDKSS_NUMBER': 'number', 'FDKSS_INTEGER': 'integer', 'FDKSS_STRING': 'string', 'FDKSS_BOOLEAN': 'boolean'}[p['value_type']]
            lua.append(f"---@field {p['name']} {lua_type}|nil {'Writable.' if p['writable'] else 'Read-only.'}{(' Computed; upright characters only.' if p['storage']=='computed_yaw' else ' Computed through current character status link.') if p.get('computed') else ''}")
        if root in ('player','actor'):
            lua += [f'---@field status_value fun(self:FDK_{root}, selector:integer):integer|nil Restricted native raw uint32 getter.',
                    f'---@field resources fun(self:FDK_{root}):FDKPlayerResources|nil Character references only.',
                    f'---@field conditions fun(self:FDK_{root}):FDKConditionRecord[]|nil Character references only.']
            lua.append(f'---@field model_info fun(self:FDK_{root}):FDKModelInfo|nil Copied current Chara/ObjectActor Model metadata.')
        if root == 'player' and methods:
            lua.append('---@field call fun(self:FDK_player, method:"type_name"|"type_id"|"type_flags"|"set_placement"|"set_native_state_byte"|"get_transform", ...:number):string|integer|number|nil')
        if root == 'actor' and methods:
            lua.append('---@field call fun(self:FDK_actor, method:"get_transform"|"type_name"|"type_id"|"type_flags"):' + ','.join(['number|string|integer'] + ['number|nil'] * 15))
            lua.append('-- Actor type_name/type_id/type_flags calls are also available on 22 covered concrete classes; the base pure virtuals are rejected.')
        fields = [p for p in fields if not p.get('computed')]
        if not fields:
            continue
        header.append(f'struct {root}_prefix {{')
        cursor = 0
        for p in fields:
            if p['offset'] > cursor:
                header.append(f"    std::byte reserved_{cursor:x}[{p['offset'] - cursor}];")
            storage = p['storage']
            declaration = f"char {p['name']}[{storage[4:]}]" if storage in ('char32', 'char64') else f"{'std::' + storage if storage in ('uint8_t', 'uint32_t', 'uint64_t') else storage} {p['name']}"
            header.append(f'    {declaration};')
            cursor = p['offset'] + (int(storage[4:]) if storage in ('char32', 'char64') else 1 if storage in ('bool', 'uint8_t') else 8 if storage == 'uint64_t' else 4)
        header.append('};')
        for p in fields:
            header.append(f"static_assert(offsetof({root}_prefix, {p['name']}) == 0x{p['offset']:x});")
    if any(method['slot'] is None for method in methods):
        header += ['struct Placement { std::uint32_t flags; float x, y, z, yaw_radians; };',
                   'static_assert(sizeof(Placement) == 20 && offsetof(Placement, yaw_radians) == 16);']
    for method in methods:
        arguments = method.get('cpp_arguments', 'void* actor, const Placement* placement' if method['slot'] is None else 'void* actor')
        symbol = method.get('cpp_name', method['name'])
        header += [f"using {symbol}_fn = {method['cpp_return_type']} (*)({arguments});",
                   f"inline constexpr std::uintptr_t {symbol}_rva = {method['rva']};"]
        if method['slot'] is not None:
            header.append(f"inline constexpr std::size_t {symbol}_slot = {method['slot']};")
    header += ['// Copied battle wire values, not an engine object layout or handle.',
               'struct BattleSnapshotRecord { std::uint32_t status_word, level, hp, max_hp, ep, max_ep, cp, max_cp; };',
               'static_assert(sizeof(BattleSnapshotRecord) == 32);',
               'inline constexpr std::size_t battle_snapshot_limit = 70;']
    header.append(f'}} // namespace fdkss::{game}::sdk')
    lua += ['---@class FDKCommandGroup', '---@field class string Read-only.', '---@field address string Read-only diagnostic.',
            '---@field group integer Read-only.', '---@field slot_count integer Read-only.', '---@field implemented_count integer Read-only.',
            '---@field class_info fun(self:FDKCommandGroup):FDKClassInfo', '---@field is_a fun(self:FDKCommandGroup, class_name:string):boolean']
    lua += ['---@class FDKEngineRoots'] + [f'---@field {root} FDK_{root}' for root in roots if root != 'actor']
    lua += [
        '---@class FDKScriptVariableInfo', '---@field name string', '---@field index integer',
        '---@field raw_metadata integer', '---@field raw_value integer', '---@field value integer|number|string|nil',
        '---@class FDKScriptReference', '---@field info fun(self:FDKScriptReference):table|nil',
        '---@field functions fun(self:FDKScriptReference):string[]',
        '---@field variables fun(self:FDKScriptReference):string[]',
        '---@field variable_info fun(self:FDKScriptReference, name:string):FDKScriptVariableInfo|nil',
        '---@field set_variable fun(self:FDKScriptReference, name:string, value:integer|number):integer|number',
        '---@field function_info fun(self:FDKScriptReference, name:string):table|nil',
        '---@field function_at fun(self:FDKScriptReference, index:integer):table|nil',
        '---@field call_constant fun(self:FDKScriptReference, name:string):integer|number|string|nil',
        '---@field call_pure fun(self:FDKScriptReference, name:string, ...:integer|number|nil):integer|number|string|nil',
        '---@field call_pure_async fun(self:FDKScriptReference, name:string, quantum:integer, ...:integer|number|nil):integer',
        '---@field preview_pure fun(self:FDKScriptReference, name:string, ...:integer|number|nil):integer|number|string|nil',
        '---@class FDKPlayerResources',
        '---@field hp integer', '---@field max_hp integer',
        '---@field ep integer', '---@field max_ep integer',
        '---@field cp integer', '---@field max_cp integer',
        '---@class FDKConditionRecord', '---@field raw_key integer', '---@field raw_bytes string Copied 72-byte native record.',
        '---@class FDKBattleRecord: FDKPlayerResources',
        '---@field index integer Zero-based position in this copy; no persistent identity.',
        '---@field status_word integer Uninterpreted first status DWORD; no enum or lifetime guarantee.',
        '---@field level integer',
        '---@class FDKBattleSnapshot', '---@field count integer',
        '---@field records FDKBattleRecord[]', '---@field atomic boolean Always false; copied values with change rechecks.',
        '---@alias FDKResourceChangePre fun(address:string, status_word:integer, level:integer, resource:string, operation:string, request:integer, before:integer, maximum:integer)',
        '---@alias FDKResourceChangePost fun(address:string, status_word:integer, level:integer, resource:string, operation:string, request:integer, before:integer, maximum:integer, after:integer|nil, after_maximum:integer|nil, error?:string)',
        '---@class FDKApi', '---@field engine FDKEngineRoots',
        '---@field hook fun(name:string, phase:string, callback:function):integer Register during mod initialization.',
        '---@field unhook fun(token:integer):boolean',
        '---@field battle_snapshot fun():FDKBattleSnapshot|nil No addresses, handles, writes or native calls.',
        '---@field player_conditions fun():FDKConditionRecord[]|nil',
        '---@field player_resources fun():FDKPlayerResources|nil',
        '---@field set_player_resource fun(name:string, value:integer):integer',
        '---@field find_objects fun(class_filter?:string, name_filter?:string, include_derived?:boolean):FDK_actor[]',
        '---@field command_group fun(group:integer):FDKCommandGroup',
        '---@field find_command_groups fun(class_filter?:string):FDKCommandGroup[]',
        '---@field find_object fun(class_filter?:string, name_filter?:string, include_derived?:boolean):FDK_actor|nil',
        '---@field notify_on_actor_discovered fun(class_filter:string|nil, callback:fun(actor:FDK_actor)):integer',
        '---@field cancel_actor_notification fun(id:integer):boolean',
        '---@field script fun(name:string):FDKScriptReference',
        '---@field script_wait fun(seconds:number, quantum?:integer):integer',
        '---@field call_compiled_async fun(bytes:string, name:string, quantum?:integer, ...:integer|number|nil):integer',
        '---@field call_compiled fun(bytes:string, name:string, ...:integer|number|nil):integer|number|string|nil',
        '---@field preview_compiled fun(bytes:string, name:string, ...:integer|number|nil):integer|number|string|nil',
        '---@field find_scripts fun(name_filter?:string):FDKScriptReference[]',
        '---@type FDKApi', 'fdk = fdk or {}']
    (directory / f'{game}_layouts.hpp').write_text('\n'.join(header) + '\n', encoding='utf-8')
    (directory / 'fdk.lua').write_text('\n'.join(lua) + '\n', encoding='utf-8')


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.executable.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest not in (SKY1_HASH, SKY2_HASH):
        parser.error('executable is not a supported pinned FDK build')
    pe = pefile.PE(data=data)
    if pe.FILE_HEADER.Machine != 0x8664:
        parser.error('expected AMD64')
    classes = recover(pe)
    game = 'sky1' if digest == SKY1_HASH else 'sky2'
    properties = verified_properties(Path(__file__).resolve().parents[1] / f'include/fdkss/{game}_properties.inc')
    methods = verified_methods(Path(__file__).resolve().parents[1] / f'include/fdkss/{game}_methods.inc')
    native_scalars = verified_native_scalars(pe, game) + verified_native_selections(pe, game) + verified_native_square_roots(pe, game)
    native_resources = verified_native_resource_setters(pe, game)
    native_status = verified_native_status_getters(pe, game)
    for entry in classes:
        for table in entry['vtables']:
            for slot in table['methods']:
                known = next((method for method in methods if method['class'] == entry['name'] and
                              method['slot'] == slot['slot'] and method['rva'] == slot['rva']), None)
                if known:
                    slot.update(name=known['name'], signature=known['signature'], verified_signature=True)
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'format_version': 1, 'build_sha256': digest, 'preferred_image_base': hex(pe.OPTIONAL_HEADER.ImageBase),
              'classes': classes, 'verified_properties': properties, 'verified_methods': methods,
              'verified_native_scalar_calls': native_scalars,
              'verified_native_resource_setters': native_resources,
              'verified_native_status_getters': native_status,
              'battle_snapshot': {'binding':'engine.battle_snapshot','lua':'fdk.battle_snapshot',
                                  'limit':70,'record_bytes':32,'atomic':False,'persistent_handles':False,
                                  'fields':['status_word','level','hp','max_hp','ep','max_ep','cp','max_cp']},
              'limitations': ['RTTI hierarchy offsets describe inheritance, not arbitrary fields.',
                             'Vtable signatures are unknown except marked verified methods; final slot boundaries are unverified.',
                             'RVA exports require loaded image base; no live lifetime information is supplied.']}
    (args.output / 'metadata.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    write_sdk(args.output, properties, methods, game)
    print(f'Exported {len(classes)} RTTI classes, {sum(len(c["vtables"]) for c in classes)} vtables, {len(properties)} verified properties, {len(methods)} actor methods and {len(native_scalars)} native scalar signatures to {args.output}')


if __name__ == '__main__':
    main()
