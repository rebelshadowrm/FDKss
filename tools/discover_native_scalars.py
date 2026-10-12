"""Find observed scalar VM-handler encodings without known RVAs or build hashes.

This offline report supplies code and layout candidates, not callable signatures
or automatic runtime acceptance. No recovered code is executed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

PATTERNS = json.loads(Path(__file__).with_name('native_scalar_patterns.json').read_text())
WIDTHS = dict(argument_count=4, stack_used_bytes=4, stack_pointer=8,
              return_enabled=1, result_register=4)


def ranges_checked(ranges, length):
    result = sorted(ranges)
    for index, (start, end) in enumerate(result):
        if not 0 <= start < end <= length:
            raise ValueError('section range is outside the mapped image')
        if index and start < result[index - 1][1]:
            raise ValueError('section ranges overlap')
    return result


def contained(start, size, ranges):
    return any(a <= start and start + size <= b for a, b in ranges)


def expression(pattern):
    body = bytes.fromhex(pattern['body_hex'])
    masked = {at + i for occurrences in pattern['fields'].values()
              for at, width in occurrences for i in range(width)}
    if pattern['external_operand']:
        masked.update(range(pattern['external_operand'][1], pattern['external_operand'][1] + 4))
    return re.compile(b''.join(b'.' if i in masked else re.escape(bytes([byte]))
                              for i, byte in enumerate(body)), re.DOTALL)


def layout(body, pattern):
    fields = {}
    for name, occurrences in pattern['fields'].items():
        values = [int.from_bytes(body[at:at + width], 'little', signed=True)
                  for at, width in occurrences]
        offset = values[0]
        width = WIDTHS[name]
        if len(set(values)) != 1 or not 0 < offset <= 0x8000:
            return None
        if width > 1 and offset % width:
            return None
        fields[name] = offset
    ordered = sorted((offset, offset + WIDTHS[name]) for name, offset in fields.items())
    if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
        return None
    return fields


def scan(image, executable, readable):
    executable = ranges_checked(executable, len(image))
    readable = ranges_checked(readable, len(image))
    candidates = []
    for pattern in PATTERNS:
        regex = expression(pattern)
        for start, end in executable:
            # Search every start, including overlapping ambiguous matches.
            cursor = start
            while cursor < end:
                match = regex.search(image, cursor, end)
                if match is None:
                    break
                cursor = match.start() + 1
                body = match.group()
                fields = layout(body, pattern)
                if fields is None:
                    continue
                dependency = None
                if pattern['external_operand']:
                    role, at = pattern['external_operand']
                    target = match.start() + at + 4 + struct.unpack_from('<i', body, at)[0]
                    if role == 'sign_mask':
                        if target % 16 or not contained(target, 16, readable):
                            continue
                        if image[target:target + 16] != struct.pack('<4I', *([0x7fffffff] * 4)):
                            continue
                    elif not contained(target, 1, executable) or match.start() <= target < match.end():
                        continue
                    dependency = dict(role=role, target_rva=hex(target),
                                      executed_or_callable_verified=False)
                if len(candidates) == 256:
                    raise ValueError('native scalar discovery exceeds 256 candidates')
                candidates.append(dict(operation_candidate=pattern['operation_candidate'],
                    handler_candidate_rva=hex(match.start()), body_size=len(body),
                    body_hex=body.hex(), body_sha256=hashlib.sha256(body).hexdigest(),
                    storage_offset_candidates=fields, external_dependency=dependency))
    return sorted(candidates, key=lambda row: int(row['handler_candidate_rva'], 16))


def discover(pe):
    if pe.FILE_HEADER.Machine != 0x8664 or not 0 < pe.OPTIONAL_HEADER.SizeOfImage <= 512 * 1024 * 1024:
        raise ValueError('expected a bounded AMD64 PE image')
    image = pe.get_memory_mapped_image()
    sections = [(s.VirtualAddress, min(len(image), s.VirtualAddress +
                 max(s.Misc_VirtualSize, s.SizeOfRawData)), s.Characteristics) for s in pe.sections]
    rows = scan(image, [(a, b) for a, b, flags in sections if flags & 0x20000000],
                [(a, b) for a, b, flags in sections if flags & 0x40000000])
    return dict(format_version=1, candidates=rows,
        ambiguous_operations=[p['operation_candidate'] for p in PATTERNS
                              if sum(r['operation_candidate'] == p['operation_candidate'] for r in rows) > 1],
        limitations=[
            'Only six observed complete instruction encodings are recognized; other compiler output may be missed.',
            'Thread storage offsets are candidates, without verified object identity or callable ABI.',
            'No command number, native group, function boundary or manager binding is inferred.',
            'Square-root negative-domain call targets are reported but never executed or approved.',
            'Unary/binary CRT wrappers remain unnamed call candidates; external bodies, constants and runtime dispatch are not validated by discovery.',
            'Multiple candidates stay ambiguous; unknown builds are never enabled automatically.',
            'Recovered bodies do not replace independently validated runtime pins.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve() in {args.executable.resolve(), Path(__file__).resolve(),
                                 Path(__file__).with_name('native_scalar_patterns.json').resolve()}:
        parser.error('output must not overwrite an input or discovery tool')
    data = args.executable.read_bytes()
    try:
        report = discover(pefile.PE(data=data))
    except (ValueError, pefile.PEFormatError) as error:
        parser.error(str(error))
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Recovered {len(report['candidates'])} scalar handler candidates")


if __name__ == '__main__':
    main()
