"""Read-only FPAC inventory and exact-entry extraction for FDK resource mod development."""
import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import zlib


@dataclass(frozen=True)
class Entry:
    name: str
    size: int
    offset: int
    name_crc32: int


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError('Truncated FPAC archive')
    return data


def inventory(path, *, allow_duplicates=False):
    with Path(path).open('rb') as stream:
        stream.seek(0, 2)
        length = stream.tell()
        stream.seek(0)
        magic, count, header_size, version = struct.unpack('<4sIII', read_exact(stream, 16))
        if magic != b'FPAC' or version != 1:
            raise ValueError('Expected FPAC version 1')
        if count > 1_000_000 or not 16 + count * 32 <= header_size <= min(length, 128 * 1024 * 1024):
            raise ValueError('Invalid FPAC directory bounds')
        directory = read_exact(stream, count * 32)
        result, names = [], set()
        for crc, padding, name_offset, size, data_offset in struct.iter_unpack('<IIQQQ', directory):
            if padding or not 16 + count * 32 <= name_offset < header_size:
                raise ValueError('Invalid FPAC name offset or padding')
            if data_offset < header_size or data_offset > length or size > length - data_offset:
                raise ValueError('Invalid FPAC resource bounds')
            stream.seek(name_offset)
            raw = stream.read(min(4096, header_size - name_offset))
            end = raw.find(b'\0')
            if end < 0:
                raise ValueError('Unterminated FPAC resource name')
            raw = raw[:end]
            name = raw.decode('utf-8')
            normalized = name.replace('\\', '/')
            parts = normalized.split('/')
            if not name or ':' in name or PurePosixPath(normalized).is_absolute() or any(
                    part in ('', '.', '..') for part in parts) or any(ord(ch) < 32 for ch in name):
                raise ValueError('Unsafe FPAC resource name')
            if normalized.casefold() in names and not allow_duplicates:
                raise ValueError('Duplicate FPAC resource name')
            if (zlib.crc32(raw) ^ 0xFFFFFFFF) != crc:
                raise ValueError('FPAC resource name checksum mismatch')
            names.add(normalized.casefold())
            result.append(Entry(name, size, data_offset, crc))
        return result


def extract(path, entry_name, destination, *, entry_index=None):
    directory = inventory(path, allow_duplicates=entry_index is not None)
    if entry_index is not None and (type(entry_index) is not int or not 0 <= entry_index < len(directory)):
        raise ValueError('Entry index outside archive directory')
    entries = [entry for index, entry in enumerate(directory) if entry.name == entry_name and
               (entry_index is None or index == entry_index)]
    if len(entries) != 1:
        raise ValueError('Resource name must match exactly one archive entry')
    entry = entries[0]
    destination = Path(destination)
    if destination.resolve() == Path(path).resolve() or (destination.exists() and destination.samefile(path)):
        raise ValueError('Extraction destination cannot replace the archive')
    destination.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    with Path(path).open('rb') as source, destination.open('wb') as output:
        source.seek(entry.offset)
        remaining = entry.size
        while remaining:
            data = read_exact(source, min(remaining, 1024 * 1024))
            output.write(data)
            digest.update(data)
            remaining -= len(data)
    return {**asdict(entry), 'directory_index': directory.index(entry) if entry_index is None else entry_index,
            'sha256': digest.hexdigest(), 'destination': str(destination)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--filter', default='', help='Substring in archived resource names')
    parser.add_argument('--extract', help='Exact archive entry name; requires --output')
    parser.add_argument('--allow-duplicates', action='store_true', help='Inventory duplicate names with explicit directory indices')
    parser.add_argument('--entry-index', type=int, help='Extract this exact directory index and name, including from duplicate-name archives')
    parser.add_argument('--output', type=Path, help='Inventory JSON or extracted file destination')
    args = parser.parse_args()
    if args.entry_index is not None and not args.extract:
        parser.error('--entry-index requires --extract')
    if args.allow_duplicates and args.extract:
        parser.error('--allow-duplicates is inventory-only; extraction requires --entry-index')
    if args.extract:
        if not args.output:
            parser.error('--extract requires an explicit --output file')
        print(json.dumps(extract(args.archive, args.extract, args.output, entry_index=args.entry_index), indent=2))
    else:
        result = [{**asdict(entry), 'directory_index': index} for index, entry in
                  enumerate(inventory(args.archive, allow_duplicates=args.allow_duplicates)) if args.filter in entry.name]
        text = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
        if args.output:
            if args.output.resolve() == args.archive.resolve() or (args.output.exists() and args.output.samefile(args.archive)):
                parser.error('Inventory output cannot replace the archive')
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding='utf-8')
        else:
            print(text, end='')


if __name__ == '__main__':
    main()
