"""Discover Actor getter, constructor, destructor and byte-setter candidates."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from dump_sdk import recover

CONSTRUCTOR_TEMPLATE = bytes.fromhex(
    '48895c2408574883ec20488d05972d3800488bd948890133ff488979084883c110e8da98baff'
    '488d4b5848897b3448897b4048897b4848897b50e8f1b10200488bc34889bb58010000'
    '4889bb60010000c78368010000ffffffff4088bb6c010000c7837001000007000000'
    '4889bb780100004889bb800100004889bb88010000488b5c24304883c4205fc3')

DESTRUCTOR_PATTERNS = (
    ('direct-delete',bytes.fromhex('48895c2408574883ec208bda488bf9488d05022d3800488901e8d20b0000488d8f7801000048c7410800000000e80e62baff90c6471000f6c301740dba90010000488bcfe807ab2100488bc7488b5c24304883c4205fc3'),
     ((18,'vtable'),(26,'call'),(46,'call'),(69,'call'))),
    ('heap-range-delete',bytes.fromhex('48895c2408574883ec208bda488bf9488d058a8ba100488901e8d2934a00488d8f7801000048c7410800000000e83e49f2ff90c6471000f6c3017447488b0585feb5004885c07432488d48084839b8e83800007709483bb8f038000072124839b8487200007713483bb850720000730a488bd7e8e8944c00eb09488bcfe86ef4770090488bc7488b5c24304883c4205fc3'),
     ((18,'vtable'),(26,'call'),(46,'call'),(63,'global'),(116,'call'),(126,'call'))),
)

CLEANUP_TEMPLATE=bytes.fromhex('48895c240848896c24104889742418574883ec20488bf933ed488b89580100004885c97411488b01ba01000000ff104889af58010000488b9f78010000488b8780010000488d34c3483bde74210f1f00488b0b4885c9740d488b01ba01000000ff1048892b4883c308483bde75e2488b5c2430488b7424404889af600100004889af80010000488b6c24384883c4205fc3')


def describe_cleanup(image, executable, entry):
    """Describe one complete observed resource-release loop, with layout operands.

    Matching the helper does not establish its receiver type or resource ownership.
    Six independent originals share these instructions, including both Ys and
    Daybreak's different member layouts. Branches, delete flags and full tail stay
    fixed; only six displacement operands vary.
    """
    if not 1 <= len(image) <= 128 * 1024 * 1024 or type(entry) is not int:
        raise ValueError('invalid image or entry bound')
    if any(not 0 <= a < b <= len(image) for a, b in executable):
        raise ValueError('cleanup executable bounds leave the image')
    size = len(CLEANUP_TEMPLATE)
    if not 0 <= entry <= len(image) - size or not any(a <= entry and entry + size <= b for a, b in executable):
        return None
    body = image[entry:entry + size]
    offsets = (28, 50, 57, 64, 123, 130)
    operands = {i for at in offsets for i in range(at, at + 4)}
    if any(body[i] != value for i, value in enumerate(CLEANUP_TEMPLATE) if i not in operands):
        return None
    linked, linked_clear, data, count, trailing_clear, count_clear = [
        struct.unpack_from('<i', body, at)[0] for at in offsets]
    if any(not 8 <= value <= 0x100000 or value % 8 for value in
           (linked, data, count, trailing_clear)) or linked != linked_clear or count != count_clear or \
            len({linked, data, count, trailing_clear}) != 4:
        return None
    return dict(handler_rva=hex(entry), body_size=size, code_hex=body.hex(),
        body_sha256=hashlib.sha256(body).hexdigest(), linked_object_offset=linked,
        vector_pointer_offset=data, vector_count_offset=count,
        cleared_receiver_offsets=[linked, trailing_clear, count],
        virtual_resource_delete_slot=0, resource_delete_flags=1,
        lifetime_invalidation_verified=False, receiver_type_verified=False,
        limitations='Complete instruction effects only; receiver identity, resource destructors, ownership, extent and live lifetime coverage remain unverified.')


def discover_cleanup_helpers(image, executable):
    """Find complete cleanup effect bodies; do not infer Actor associations."""
    if not 1 <= len(image) <= 128 * 1024 * 1024 or any(
            not 0 <= a < b <= len(image) for a, b in executable):
        raise ValueError('invalid cleanup image or executable bounds')
    found = {}
    for begin, end in executable:
        cursor = begin
        while cursor < end:
            at = image.find(CLEANUP_TEMPLATE[:28], cursor, end)
            if at < 0:
                break
            cursor = at + 1
            row = describe_cleanup(image, executable, at)
            if row:
                found[at] = row
                if len(found) > 256:
                    raise ValueError('cleanup helper discovery exceeds 256 bodies')
    return dict(candidates=[found[at] for at in sorted(found)],
        runtime_profile_accepted=False,
        limitations='Body effects only; no entry boundary, Actor association, callers, ownership or lifetime-hook approval is inferred.')


def discover_base_destructors(image,executable,classes,image_base,readable=None):
    """Match two full observed deleting-destructor encodings through Actor RTTI.

    Relocated operands are dependencies, not authorization to execute them.
    """
    readable=[(0,len(image))] if readable is None else readable
    for ranges in (executable,readable):
        if any(not 0<=a<=b<=len(image) for a,b in ranges):raise ValueError('destructor section bounds leave the mapped image')
    def contains(ranges,at,size):return any(a<=at and at+size<=b for a,b in ranges)
    result=dict(candidates=[],limitations=[
        'Two observed complete base Actor deleting-destructor encodings only.',
        'External code and heap-global behavior are not verified by discovery.',
        'No callable ABI, owner/lifetime coverage or unknown-build approval is inferred.'])
    bases=[c for c in classes if c['name']=='.?AVActor@fdk@@']
    if len(bases)!=1:return result
    tables=[t for t in bases[0]['vtables'] if t['subobject_offset']==t['constructor_displacement']==0]
    if len(tables)!=1:return result
    table=tables[0];at=int(table['rva'],16)
    methods=[m for m in table['methods'] if m['slot']==0]
    if len(methods)!=1 or at<8 or at%8 or not contains(readable,at,8):return result
    entry=int(methods[0]['rva'],16)
    if struct.unpack_from('<Q',image,at)[0]!=image_base+entry:return result
    for encoding,template,fixups in DESTRUCTOR_PATTERNS:
        if not contains(executable,entry,len(template)):continue
        body=image[entry:entry+len(template)]
        operands={i for offset,kind in fixups for i in range(offset,offset+4)}
        if any(body[i]!=value for i,value in enumerate(template) if i not in operands):continue
        dependencies=[];valid=True
        for offset,kind in fixups:
            target=entry+offset+4+struct.unpack_from('<i',body,offset)[0]
            if kind=='vtable':allowed=target==at
            elif kind=='call':allowed=contains(executable,target,1) and not entry<=target<entry+len(template)
            else:allowed=target%8==0 and contains(readable,target,8) and not entry<=target<entry+len(template)
            if not allowed:valid=False;break
            dependencies.append(dict(operand_offset=offset,kind=kind,target_rva=hex(target)))
        if not valid:continue
        first_call=next(d for d in dependencies if d['kind']=='call')
        target=int(first_call['target_rva'],16)
        # These deleting-destructor profiles retain their existing exact Sky
        # helper dependency. Independent effect discovery must not broaden a
        # previously pinned destructor's accepted member layout.
        cleanup=describe_cleanup(image, executable, target) if \
            image[target:target + len(CLEANUP_TEMPLATE)] == CLEANUP_TEMPLATE else None
        if cleanup:
            # Retain the established Actor-specific report field only where the
            # complete RTTI-backed deleting destructor supplies the association.
            cleanup['cleared_actor_offsets'] = cleanup['cleared_receiver_offsets']
        result['candidates'].append(dict(class_name=bases[0]['name'],vtable_rva=hex(at),slot=0,
            handler_rva=hex(entry),encoding=encoding,body_size=len(body),code_hex=body.hex(),
            body_sha256=hashlib.sha256(body).hexdigest(),dependencies=dependencies,
            deletion_flag_mask=1,callable_signature_verified=False,cleanup_dependency=cleanup))
    return result


def discover_constructors(image, executable, classes):
    bases = [entry for entry in classes if entry['name'] == '.?AVActor@fdk@@']
    if len(bases) != 1:
        return dict(candidates=[], limitation='Base Actor RTTI is absent or ambiguous.')
    tables = [table for table in bases[0]['vtables'] if table['subobject_offset'] == 0 and table['constructor_displacement'] == 0]
    if len(tables) != 1:
        return dict(candidates=[], limitation='Base Actor primary vtable is absent or ambiguous.')
    table = int(tables[0]['rva'], 16)
    if not 8 <= table <= len(image) - 8:
        return dict(candidates=[], limitation='Base Actor primary vtable is outside the image.')
    relative = set(range(13, 17)) | set(range(34, 38)) | set(range(59, 63))
    fixed = [(i, value) for i, value in enumerate(CONSTRUCTOR_TEMPLATE) if i not in relative]
    candidates = []
    for start, end in executable:
        stop = min(end, len(image)); cursor = max(0, start)
        while cursor < stop:
            at = image.find(CONSTRUCTOR_TEMPLATE[:10], cursor, stop)
            if at < 0:
                break
            cursor = at + 1
            if at + len(CONSTRUCTOR_TEMPLATE) > stop:
                continue
            body = image[at:at + len(CONSTRUCTOR_TEMPLATE)]
            if any(body[i] != value for i, value in fixed) or at + 17 + struct.unpack_from('<i', body, 13)[0] != table:
                continue
            calls = [at + operand + 4 + struct.unpack_from('<i', body, operand)[0] for operand in (34, 59)]
            if not all(any(a <= target < b and target < len(image) for a, b in executable) for target in calls):
                continue
            if len(candidates) == 256:
                raise ValueError('base constructor discovery exceeds 256 candidates')
            candidates.append(dict(class_name=bases[0]['name'], handler_rva=hex(at), vtable_rva=hex(table),
                code_hex=body.hex(), body_sha256=hashlib.sha256(body).hexdigest(), body_size=len(body),
                called_handler_rvas=[hex(target) for target in calls],
                initialization=[dict(offset=0x16c, storage='uint8_t', value=0), dict(offset=0x170, storage='uint32_t', value=7)]))
    # Raw E8 matches are candidates, not disassembler-verified call boundaries.
    by_rva = {int(row['handler_rva'], 16): row for row in candidates}
    for row in candidates:
        row['direct_call_candidates'] = []
    for start, end in executable:
        stop = min(end, len(image)); cursor = max(0, start)
        while cursor + 5 <= stop:
            at = image.find(b'\xe8', cursor, stop - 4)
            if at < 0:
                break
            cursor = at + 1
            target = at + 5 + struct.unpack_from('<i', image, at + 1)[0]
            if target in by_rva:
                if len(by_rva[target]['direct_call_candidates']) == 4096:
                    raise ValueError('constructor call discovery exceeds 4096 raw matches')
                by_rva[target]['direct_call_candidates'].append(hex(at))
    return dict(candidates=candidates, ambiguous=len(candidates) > 1,
                limitation='Matched constructor bodies and raw direct-call candidates; no automatic runtime profile acceptance, whole-object construction guarantee or destructor coverage.')


def discover_byte_setters(image, executable, classes, image_base):
    """Recover exact leaf `mov byte ptr [rcx+offset],dl; ret` virtual entries.

    RTTI constrains owners to nonvirtual zero-displacement Actor inheritance.
    Slots and offsets are recovered; no property name or writable API is inferred.
    """
    found, rejected = [], []
    def code(at, length):
        if not 0 <= at <= len(image)-length or not any(a<=at and at+length<=b for a,b in executable):
            return None
        return image[at:at+length]
    for entry in classes:
        if not any(base['class']=='.?AVActor@fdk@@' and base['member_displacement']==0 and
                   base['vbtable_displacement']==-1 for base in entry['bases']):
            continue
        primary=[table for table in entry['vtables'] if table['subobject_offset']==0 and table['constructor_displacement']==0]
        if len(primary)!=1:
            rejected.append(dict(class_name=entry['name'],reason='primary vtable is absent or ambiguous'))
            continue
        table=primary[0]; table_rva=int(table['rva'],16)
        methods=table['methods']
        slots=[method['slot'] for method in methods]
        if len(slots)!=len(set(slots)) or any(not isinstance(slot,int) or not 0<=slot<128 for slot in slots):
            rejected.append(dict(class_name=entry['name'],reason='virtual method slots are duplicated or out of bounds'))
            continue
        candidates=[]; invalid=False
        for method in methods:
            slot=method['slot']; at=int(method['rva'],16); word=table_rva+slot*8
            if table_rva<8 or not 0<=word<=len(image)-8 or struct.unpack_from('<Q',image,word)[0]!=image_base+at:
                invalid=True; break
            prefix=code(at,2)
            if prefix==b'\x88\x91':
                body=code(at,7)
                if body is None or body[-1]!=0xc3:continue
                offset=struct.unpack_from('<i',body,2)[0]
            elif prefix==b'\x88\x51':
                body=code(at,4)
                if body is None or body[-1]!=0xc3:continue
                offset=struct.unpack_from('<b',body,2)[0]
            else:continue
            # A zero or negative displacement could change an object's vtable
            # or another subobject. Keep those outside this candidate category.
            if not 1<=offset<=0x100000:continue
            candidates.append(dict(class_name=entry['name'],vtable_rva=hex(table_rva),slot=slot,
                handler_rva=hex(at),object_offset=offset,storage='uint8_t',body_size=len(body),
                code_hex=body.hex(),body_sha256=hashlib.sha256(body).hexdigest()))
        if invalid:
            rejected.append(dict(class_name=entry['name'],reason='recovered virtual slot disagrees with image pointer or bounds'))
            continue
        if len(found)+len(candidates)>4096:raise ValueError('Actor byte setter discovery exceeds 4096 entries')
        found.extend(candidates)
    return dict(candidates=sorted(found,key=lambda row:(row['class_name'],row['slot'])),rejected=rejected,
        limitations=['Exact leaf byte-store virtual entries only; no field names, gameplay meanings or callable runtime profiles inferred.',
                     'Overrides with additional behavior are omitted; offsets are candidates, not general field reflection.'])


def discover(pe, classes=None):
    if pe.FILE_HEADER.Machine != 0x8664:
        raise ValueError('actor discovery requires an AMD64 PE image')
    image = pe.get_memory_mapped_image()
    if not 1 <= len(image) <= 128 * 1024 * 1024:
        raise ValueError('mapped image size is outside the discovery bound')
    classes = recover(pe) if classes is None else classes
    executable = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]

    def code(rva, length):
        if rva < 0 or rva > len(image) - length or not any(a <= rva and rva + length <= b for a, b in executable):
            raise ValueError('getter body is outside an executable image section')
        return image[rva:rva + length]

    def constant(rva):
        body = code(rva, 3)
        if body[0] == 0xb8:
            body = code(rva, 6)
            if body[5] != 0xc3:
                raise ValueError('unsigned constant getter lacks an immediate return')
            value = struct.unpack_from('<I', body, 1)[0]
        elif body[:3] in (b'\x33\xc0\xc3', b'\x31\xc0\xc3'):
            body, value = body[:3], 0
        else:
            raise ValueError('unsigned constant getter pattern did not match')
        return dict(handler_rva=hex(rva), value=value, code_hex=body.hex())

    def candidate(entry, table):
        slots = {m['slot']: int(m['rva'], 16) for m in table['methods']}
        if len(slots) != len(table['methods']) or not all(slot in slots for slot in (1, 2, 3, 6)):
            raise ValueError('required getter slots are missing or ambiguous')
        body = code(slots[1], 8)
        if body[:3] != b'\x48\x8d\x05' or body[7] != 0xc3:
            raise ValueError('constant type-name getter pattern did not match')
        text = slots[1] + 7 + struct.unpack_from('<i', body, 3)[0]
        if not 0 <= text < len(image):
            raise ValueError('type-name target is outside the image')
        end = image.find(b'\0', text, min(text + 256, len(image)))
        if end < 0:
            raise ValueError('type name is not bounded and terminated')
        name = image[text:end].decode('ascii')
        if not name or any(ord(ch) < 32 or ord(ch) > 126 for ch in name):
            raise ValueError('type name is not printable ASCII')
        transform = code(slots[6], 5)
        if transform != b'\x48\x8d\x41\x58\xc3':
            raise ValueError('Actor transform-address getter pattern did not match')
        table_rva = int(table['rva'], 16)
        if table_rva < 8 or table_rva + 7 * 8 > len(image):
            raise ValueError('candidate vtable is outside the image')
        # Recovered metadata must agree with the original PE pointer words.
        for slot in (1, 2, 3, 6):
            actual = struct.unpack_from('<Q', image, table_rva + slot * 8)[0]
            if actual != pe.OPTIONAL_HEADER.ImageBase + slots[slot]:
                raise ValueError('recovered slot disagrees with image pointer')
        return dict(class_name=entry['name'], vtable_rva=hex(table_rva),
                    type_name=dict(handler_rva=hex(slots[1]), string_rva=hex(text), value=name, code_hex=body.hex()),
                    type_id=constant(slots[2]), type_flags=constant(slots[3]),
                    transform=dict(handler_rva=hex(slots[6]), code_hex=transform.hex(), object_offset=88))

    found, rejected = [], []
    for entry in classes:
        if not any(b['class'] == '.?AVActor@fdk@@' and b['member_displacement'] == 0 and
                   b['vbtable_displacement'] == -1 for b in entry['bases']):
            continue
        primary = [t for t in entry['vtables'] if t['subobject_offset'] == 0 and t['constructor_displacement'] == 0]
        if len(primary) != 1:
            rejected.append(dict(class_name=entry['name'], reason='primary vtable is missing or ambiguous'))
            continue
        try:
            found.append(candidate(entry, primary[0]))
        except (ValueError, UnicodeDecodeError, struct.error) as error:
            rejected.append(dict(class_name=entry['name'], reason=str(error)))
    return dict(format='fdkss.actor_getter_candidates', version=1,
                candidates=sorted(found, key=lambda c: c['class_name']), rejected=rejected,
                base_constructors=discover_constructors(image, executable, classes),
                base_destructors=discover_base_destructors(image,executable,classes,pe.OPTIONAL_HEADER.ImageBase,
                    [(s.VirtualAddress,min(len(image),s.VirtualAddress+s.Misc_VirtualSize)) for s in pe.sections if s.Characteristics&0x40000000]),
                virtual_byte_setters=discover_byte_setters(image, executable, classes, pe.OPTIONAL_HEADER.ImageBase),
                cleanup_helpers=discover_cleanup_helpers(image, executable),
                limitations=['Code-pattern and RTTI candidates only; this does not authorize runtime attachment.',
                             'Constructor/destructor, lifetime, owner-thread and scene-collection profiles require separate validation.',
                             'Transform offset is a matched instruction operand, not general field reflection.',
                             'Additional builds are unsupported until independently validated and hash-pinned.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve()==args.executable.resolve() or args.output.exists() and args.output.samefile(args.executable):
        parser.error('Output cannot overwrite the executable')
    if args.output.resolve() == args.executable.resolve():
        parser.error('output must not overwrite the executable')
    data = args.executable.read_bytes()
    try:
        pe = pefile.PE(data=data)
        report = discover(pe)
    except (ValueError, pefile.PEFormatError) as error:
        parser.error(str(error))
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Discovered {len(report['candidates'])} getter, {len(report['base_constructors']['candidates'])} base constructor, {len(report['base_destructors']['candidates'])} base destructor and {len(report['virtual_byte_setters']['candidates'])} byte-setter entries; rejected {len(report['rejected'])} Actor getter classes")
    print(f"{len(report['cleanup_helpers']['candidates'])} cleanup effect bodies; receiver types and lifetime coverage are not inferred")


if __name__ == '__main__':
    main()
