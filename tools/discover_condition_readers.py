"""Recover bounded condition-vector reader candidates without pinned RVAs.

Patterns cover two observed register/branch encodings. Results are internal
reader fragments and layout candidates, never callable signatures or automatic
authorization to enable an adapter on an unknown executable.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def pattern(second):
    # MOV RAX,[R13+context link], nullable character/status links, manager load.
    prefix = (rb'\x49\x8b\x85(?P<context>.{4})\x48\x85\xc0(?P<no_actor>\x74.)'
              rb'\x48\x8b\x88(?P<link>.{4})\x48\x85\xc9(?P<no_link>\x74.)')
    manager = rb'\x48\x8b\xb1' if second else rb'\x4c\x8b\xb1'
    count = rb'\x8b\x86' if second else rb'\x41\x8b\x86'
    pointer = rb'\x48\x8b\x96' if second else rb'\x49\x8b\x96'
    branch = rb'\x0f\x84.{4}' if second else rb'\x74.'
    jump = rb'\xe9.{4}' if second else rb'\xeb.'
    return re.compile(prefix + manager + rb'(?P<manager>.{4})' +
        count + rb'(?P<count>.{4})' + pointer + rb'(?P<pointer>.{4})' +
        rb'\x48\x8d\x0c\xc0\x48\x8d\x04\xca\x48\x3b\xd0(?P<empty>' + branch + rb')' +
        (b'' if second else rb'\x0f\x1f\x44\x00\x00') +
        rb'(?P<key>\x83\x3a\x13)(?P<hit>' + branch + rb')'
        rb'\x48\x83\xc2\x48\x48\x3b\xd0(?P<loop>\x75.)(?P<miss>' + jump + rb')', re.DOTALL)


def branch_target(match, name):
    raw = match.group(name)
    width = 4 if len(raw) > 2 else 1
    return match.end(name) + int.from_bytes(raw[-width:], 'little', signed=True)


def scan(image, executable):
    candidates = []
    for start, end in executable:
        if not 0 <= start <= end <= len(image):
            raise ValueError('executable range is outside the mapped image')
        for second in (False, True):
            for match in pattern(second).finditer(image, start, end):
                fields = {name: struct.unpack('<i', match.group(name))[0]
                          for name in ('context', 'link', 'manager', 'count', 'pointer')}
                if any(not 0 < n <= 0x8000 for n in fields.values()):
                    continue
                if any(fields[name] % alignment for name, alignment in
                       [('context', 8), ('link', 8), ('manager', 8), ('pointer', 8), ('count', 4)]):
                    continue
                if fields['count'] != fields['pointer'] + 8:
                    continue
                # Both nullable links converge on the count load; the reader's
                # manager register may already hold a fallback at this point.
                count_instruction = match.start('count') - (2 if second else 3)
                if branch_target(match, 'no_actor') != count_instruction or branch_target(match, 'no_link') != count_instruction:
                    continue
                hit, miss = branch_target(match, 'hit'), branch_target(match, 'miss')
                if branch_target(match, 'loop') != match.start('key') or branch_target(match, 'empty') != miss:
                    continue
                if hit == miss or min(hit, miss) < match.end() or not all(
                    any(a <= target < b for a, b in executable) for target in (hit, miss)):
                    continue
                if len(candidates) == 256:
                    raise ValueError('condition reader discovery exceeds 256 candidates')
                raw = match.group()
                candidates.append(dict(reader_fragment_rva=hex(match.start()), fragment_size=len(raw),
                    code_hex=raw.hex(), code_sha256=hashlib.sha256(raw).hexdigest(),
                    encoding='rsi-near-branches' if second else 'r14-short-branches',
                    source_context_actor_offset=fields['context'], character_link_offset=fields['link'],
                    manager_link_offset=fields['manager'], vector_pointer_offset=fields['pointer'],
                    vector_count_offset=fields['count'], record_stride=72, raw_key_offset=0,
                    searched_raw_key=19, found_branch_rva=hex(hit), missing_branch_rva=hex(miss)))
    return sorted(candidates, key=lambda row: int(row['reader_fragment_rva'], 16))


def discover(pe):
    if pe.FILE_HEADER.Machine != 0x8664 or not 0 < pe.OPTIONAL_HEADER.SizeOfImage <= 512 * 1024 * 1024:
        raise ValueError('expected a bounded AMD64 PE image')
    image = pe.get_memory_mapped_image()
    executable = [(section.VirtualAddress, min(len(image), section.VirtualAddress +
                   max(section.Misc_VirtualSize, section.SizeOfRawData)))
                  for section in pe.sections if section.Characteristics & 0x20000000]
    rows = scan(image, executable)
    layouts = sorted({tuple(row[name] for name in ('character_link_offset', 'manager_link_offset',
                     'vector_pointer_offset', 'vector_count_offset', 'record_stride')) for row in rows})
    return dict(format_version=1, candidates=rows,
                distinct_layouts=[dict(zip(('character_link_offset', 'manager_link_offset',
                    'vector_pointer_offset', 'vector_count_offset', 'record_stride'), layout)) for layout in layouts],
                limitations=['Only two observed instruction encodings are recognized.',
                    'Matches are internal reader fragments, not function starts or callable ABIs.',
                    'Raw offsets require independent class, owner and live-chain verification.',
                    'No capacity, active-condition, expiration or effective-stat interpretation.',
                    'No adapter is enabled automatically, even with one distinct layout.'])


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
    print(f"Recovered {len(report['candidates'])} reader candidates; {len(report['distinct_layouts'])} distinct layouts")


if __name__ == '__main__':
    main()
