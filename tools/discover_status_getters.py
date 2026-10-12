"""Recover status-like compressed dispatchers and uint32 leaf-read candidates.

Offline code patterns are investigation evidence, not callable ABI verification
or authorization to enable an unknown build. No recovered code is executed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


DISPATCH = re.compile(
    rb'\x83\xfa(?P<maximum>[\x00-\x7f])(?P<default>\x77.|\x0f\x87.{4})'
    rb'\x48\x63\xc2\x4c\x8d\x05(?P<base>.{4})'
    rb'\x41\x0f\xb6\x84\x00(?P<selectors>.{4})'
    rb'\x41\x8b\x94\x80(?P<targets>.{4})\x49\x03\xd0\xff\xe2', re.DOTALL)


def contained(start, end, ranges):
    return any(a <= start < end <= b for a, b in ranges)


def leaf(image, target, code_end):
    if target + 4 <= code_end and image[target:target + 2] == b'\x8b\x41' and image[target + 3] == 0xc3:
        offset = struct.unpack_from('<b', image, target + 2)[0]
        size = 4
    elif target + 7 <= code_end and image[target:target + 2] == b'\x8b\x81' and image[target + 6] == 0xc3:
        offset = struct.unpack_from('<i', image, target + 2)[0]
        size = 7
    else:
        return None
    if not 0 < offset <= 0x8000 or offset % 4:
        return None
    return dict(status_offset=offset, body_hex=image[target:target + size].hex(), body_size=size)


def scan(image, executable):
    ranges = sorted(executable)
    for index, (start, end) in enumerate(ranges):
        if not 0 <= start < end <= len(image):
            raise ValueError('executable range is outside the mapped image')
        if index and start < ranges[index - 1][1]:
            raise ValueError('executable ranges overlap')
    candidates = []
    for start, end in ranges:
        for match in DISPATCH.finditer(image, start, end):
            maximum = match.group('maximum')[0]
            # The recognized addressing sequence uses R8 as the image base.
            base = match.end('base') + struct.unpack('<i', match.group('base'))[0]
            indices = struct.unpack('<i', match.group('selectors'))[0]
            table = struct.unpack('<i', match.group('targets'))[0]
            if base != 0 or maximum < 5 or table % 4:
                continue
            if not match.end() <= table < indices or not contained(match.start(), indices + maximum + 1, ranges):
                continue
            selector_indices = image[indices:indices + maximum + 1]
            target_count = max(selector_indices) + 1
            # Observed encodings put the selector bytes directly after the
            # dense target table. Require every table entry to be referenced.
            if table + target_count * 4 != indices or set(selector_indices) != set(range(target_count)):
                continue
            targets = list(struct.unpack_from(f'<{target_count}I', image, table))
            if any(not match.end() <= target < table for target in targets) or len(set(targets)) != target_count:
                continue
            raw_default = match.group('default')
            width = 1 if len(raw_default) == 2 else 4
            default = match.end('default') + int.from_bytes(raw_default[-width:], 'little', signed=True)
            if default not in targets or default + 3 > table or image[default:default + 3] != b'\x33\xc0\xc3':
                continue
            leaves = {target: leaf(image, target, table) for target in targets}
            leaves = {target: row for target, row in leaves.items() if row is not None}
            if len({row['status_offset'] for row in leaves.values()}) < 6:
                continue
            if any(target < other < target + row['body_size'] for target, row in leaves.items() for other in targets):
                continue
            paths = [dict(selector=selector, target_rva=hex(targets[index]), **leaves[targets[index]])
                     for selector, index in enumerate(selector_indices) if targets[index] in leaves]
            raw = image[match.start():indices + maximum + 1]
            if len(candidates) == 256:
                raise ValueError('status getter discovery exceeds 256 candidates')
            candidates.append(dict(getter_candidate_rva=hex(match.start()),
                encoding='near-ja' if width == 4 else 'short-ja', selector_maximum=maximum,
                selector_table_rva=hex(indices), target_table_rva=hex(table),
                default_target_rva=hex(default), target_count=target_count,
                candidate_span_size=len(raw), candidate_span_hex=raw.hex(),
                candidate_span_sha256=hashlib.sha256(raw).hexdigest(),
                leaf_read_candidates=paths,
                unverified_selectors=[i for i, index in enumerate(selector_indices) if targets[index] not in leaves]))
    return sorted(candidates, key=lambda row: int(row['getter_candidate_rva'], 16))


def discover(pe):
    if pe.FILE_HEADER.Machine != 0x8664 or not 0 < pe.OPTIONAL_HEADER.SizeOfImage <= 512 * 1024 * 1024:
        raise ValueError('expected a bounded AMD64 PE image')
    image = pe.get_memory_mapped_image()
    executable = [(section.VirtualAddress, min(len(image), section.VirtualAddress +
                   max(section.Misc_VirtualSize, section.SizeOfRawData)))
                  for section in pe.sections if section.Characteristics & 0x20000000]
    return dict(format_version=1, candidates=scan(image, executable), limitations=[
        'Only two observed compressed dispatcher encodings are recognized.',
        'A status-like pattern does not prove class identity, function boundaries or a callable ABI.',
        'Only reported load-and-return paths are decoded; other branches remain unverified.',
        'Offsets are raw uint32 read candidates, without gameplay meanings or object layouts.',
        'Candidate bytes do not replace independently validated complete runtime pins.',
        'Multiple candidates remain ambiguous; unknown builds are never enabled automatically.'])


def main():
    import pefile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve() == args.executable.resolve():
        parser.error('output must not overwrite the executable')
    data = args.executable.read_bytes()
    try:
        report = discover(pefile.PE(data=data))
    except (ValueError, pefile.PEFormatError) as error:
        parser.error(str(error))
    report['build_sha256'] = hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Recovered {len(report['candidates'])} status getter candidates")


if __name__ == '__main__':
    main()
