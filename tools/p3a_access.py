"""Bounded P3A extraction through installed PH3 tooling with independent content checks.

Layout reference: ph3at/p3a-format, commit
8bd31f36a9bb4a3cfaf1dc186eae8d1e6174243b (MIT). No upstream code is copied.
Requires xxhash for independent header and extracted-content verification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

MAX_FILES = 100_000
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_FILE = 128 * 1024 * 1024


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_name(raw):
    end = raw.find(b'\0')
    if end < 0:
        raise ValueError('Unterminated P3A path')
    name = raw[:end].decode('utf-8')
    parts = name.split('/')
    reserved = {'con', 'prn', 'aux', 'nul'} | {f'{p}{i}' for p in ('com', 'lpt') for i in range(1, 10)}
    if not name or '\\' in name or any(ord(c) < 32 or c in ':*?"<>|' for c in name) or any(
            p in ('', '.', '..') or p.endswith((' ', '.')) or p.split('.')[0].casefold() in reserved for p in parts):
        raise ValueError('Unsafe P3A path')
    return name


def inventory(path, max_expanded=MAX_EXPANDED, *, max_archive=MAX_ARCHIVE, max_file=MAX_FILE):
    import xxhash
    path = Path(path)
    length = path.stat().st_size
    if not 32 <= length <= max_archive <= 64 * 1024**3 or not 0 < max_expanded <= 64 * 1024**3 or not 0 < max_file <= 64 * 1024**3:
        raise ValueError('P3A archive or extraction budget out of bounds')
    with path.open('rb') as stream:
        header = stream.read(32)
        magic, flags, version, count, checksum = struct.unpack('<8sIIQQ', header)
        if magic != b'PH3ARCV\0' or flags & ~1 or version != 1200 or count > MAX_FILES:
            raise ValueError('Unsupported P3A header/version/flags/count')
        if xxhash.xxh64(header[:24]).intdigest() != checksum:
            raise ValueError('P3A header checksum mismatch')
        entry_size = 296
        index_start = 32
        if version == 1200:
            extended = stream.read(16)
            if len(extended) != 16:
                raise ValueError('Truncated P3A extended header')
            ext_hash, ext_size, entry_size = struct.unpack('<QII', extended)
            if ext_size != 16 or entry_size != 304:
                raise ValueError('Unsupported P3A extended header layout')
            if xxhash.xxh64(extended[8:]).intdigest() != ext_hash:
                raise ValueError('P3A extended header checksum mismatch')
            index_start += ext_size
        index_end = index_start + count * entry_size
        if index_end > length:
            raise ValueError('P3A index outside archive')
        entries, names, expanded = [], set(), 0
        for _ in range(count):
            raw = stream.read(entry_size)
            name = safe_name(raw[:256])
            key = name.casefold()
            if key in names:
                raise ValueError('Duplicate P3A path')
            names.add(key)
            compression, packed, unpacked, offset, packed_hash = struct.unpack_from('<5Q', raw, 256)
            expanded += unpacked
            if compression > 3 or (compression == 3 and not flags & 1) or offset < index_end or offset > length or packed > length - offset:
                raise ValueError('Invalid P3A entry range/compression')
            if compression == 0 and packed != unpacked or unpacked > max_file or expanded > max_expanded:
                raise ValueError('P3A expanded-size budget exceeded or invalid raw entry')
            entries.append(dict(name=name, compression=compression, compressed_size=packed,
                                size=unpacked, offset=offset, compressed_xxh64=f'{packed_hash:016x}',
                                content_xxh64=f'{struct.unpack_from("<Q", raw, 296)[0]:016x}' if version == 1200 else None))
        for name in names:
            parts = name.split('/')
            if any('/'.join(parts[:i]) in names for i in range(1, len(parts))):
                raise ValueError('P3A file/directory path conflict')
        data_start = index_end
        dictionary_info = None
        if flags & 1:
            dictionary = stream.read(16)
            if len(dictionary) != 16:
                raise ValueError('Truncated P3A dictionary')
            dictionary_magic, size = struct.unpack('<8sQ', dictionary)
            if dictionary_magic != b'P3ADICT\0' or size > length - index_end - 16:
                raise ValueError('Invalid P3A dictionary')
            data_start += 16 + size
            dictionary_info = {'offset': index_end + 16, 'size': size}
        ranges = sorted((e['offset'], e['offset'] + e['compressed_size']) for e in entries if e['compressed_size'])
        if any(a < data_start for a, b in ranges) or any(a < prev_b for (_, prev_b), (a, _) in zip(ranges, ranges[1:])):
            raise ValueError('Overlapping P3A data or dictionary/index intersection')
    return dict(version=version, expanded_bytes=expanded, entries=entries, dictionary=dictionary_info)


def extract_entry(archive, entry_name, destination):
    """Read one checksummed entry from a large archive without expanding its peers."""
    import xxhash
    archive, destination = Path(archive).resolve(), Path(destination).resolve()
    if destination.exists() or destination.is_relative_to(archive.parent):
        raise ValueError('Exact-entry destination must be new and outside the archive directory')
    before = sha256(archive)
    index = inventory(archive, 64 * 1024**3, max_archive=64 * 1024**3, max_file=64 * 1024**3)
    entries = [e for e in index['entries'] if e['name'] == entry_name]
    if len(entries) != 1:
        raise ValueError('Entry name must match exactly one indexed path')
    entry = entries[0]
    if entry['compressed_size'] > MAX_FILE or entry['size'] > MAX_FILE:
        raise ValueError('Selected compressed input exceeds budget')
    with archive.open('rb') as stream:
        stream.seek(entry['offset']); packed = stream.read(entry['compressed_size'])
        if len(packed) != entry['compressed_size'] or f'{xxhash.xxh64(packed).intdigest():016x}' != entry['compressed_xxh64']:
            raise ValueError('Selected packed content checksum mismatch')
        dictionary = None
        if entry['compression'] == 3:
            info = index['dictionary']
            if info['size'] > 16 * 1024 * 1024:
                raise ValueError('Dictionary exceeds selected-entry budget')
            stream.seek(info['offset']); dictionary = stream.read(info['size'])
    if entry['compression'] == 0:
        unpacked = packed
    elif entry['compression'] == 1:
        import lz4.block
        unpacked = lz4.block.decompress(packed, uncompressed_size=entry['size'])
    else:
        import io
        import zstandard
        parameters = zstandard.get_frame_parameters(packed)
        if parameters.window_size > MAX_FILE:
            raise ValueError('Zstandard window exceeds selected-entry budget')
        dictionary = zstandard.ZstdCompressionDict(dictionary) if dictionary is not None else None
        with zstandard.ZstdDecompressor(dict_data=dictionary).stream_reader(io.BytesIO(packed)) as reader:
            unpacked = reader.read(entry['size'] + 1)
    if len(unpacked) != entry['size'] or f'{xxhash.xxh64(unpacked).intdigest():016x}' != entry['content_xxh64']:
        raise ValueError('Selected expanded content checksum/size mismatch')
    if sha256(archive) != before:
        raise ValueError('Archive changed during selected-entry extraction')
    destination.parent.mkdir(parents=True,exist_ok=True)
    with destination.open('xb') as output:
        output.write(unpacked)
    return dict(archive=str(archive), archive_sha256=before, entry=entry,
                output=str(destination), output_sha256=hashlib.sha256(unpacked).hexdigest(),
                originals_unchanged=True, runtime_acceptance=False)


def extract(archive, tool, workspace, max_expanded=MAX_EXPANDED):
    if not 0 < max_expanded <= 4 * 1024**3:
        raise ValueError('Full extraction expansion budget must be at most 4096 MiB')
    import xxhash
    archive, tool, workspace = (Path(p).resolve() for p in (archive, tool, workspace))
    if not tool.is_file() or workspace.exists() or workspace.is_relative_to(archive.parent):
        raise ValueError('Use an existing tool and a new workspace outside the source archive directory')
    before, tool_hash = sha256(archive), sha256(tool)
    index = inventory(archive, max_expanded)
    if sha256(archive) != before:
        raise ValueError('Archive changed during inventory')
    workspace.mkdir(parents=True)
    inputs, output = workspace / 'input', workspace / 'extracted'
    inputs.mkdir(); output.mkdir()
    copied = inputs / archive.name
    shutil.copy2(archive, copied)
    if sha256(copied) != before:
        raise ValueError('Copied archive does not match baseline')
    manifest = dict(format='fdkss.p3a_access', version=1, archive=str(archive), archive_sha256=before,
                    tool=str(tool), tool_sha256=tool_hash, workspace=str(workspace),
                    format_reference='ph3at/p3a-format@8bd31f36a9bb4a3cfaf1dc186eae8d1e6174243b',
                    index=index, commands=[], state='started', runtime_acceptance=False)
    def record():
        (workspace / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    record()
    try:
        for command in ('verify', 'extract'):
            argv = [str(tool), command, str(copied)]
            if sha256(tool) != tool_hash:
                raise ValueError('PH3 tool identity changed')
            result = subprocess.run(argv, cwd=output, capture_output=True, timeout=300)
            (workspace / (command + '.log')).write_bytes(result.stdout + result.stderr)
            manifest['commands'].append(dict(argv=argv, cwd=str(output), exit_code=result.returncode))
            record()
            if result.returncode:
                raise ValueError(f'PH3 {command} failed; inspect recorded log')
        actual = {str(p.relative_to(output)).replace('\\', '/') for p in output.rglob('*') if p.is_file()}
        expected = {e['name'] for e in index['entries']}
        if actual != expected:
            raise ValueError('Extracted file set differs from validated index')
        for entry in index['entries']:
            path = output / entry['name']
            if path.is_symlink() or not path.resolve().is_relative_to(output) or path.stat().st_size != entry['size']:
                raise ValueError('Extracted path/size mismatch')
            with path.open('rb') as stream:
                digest = xxhash.xxh64()
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            if entry['content_xxh64'] is not None and digest.hexdigest() != entry['content_xxh64']:
                raise ValueError('Extracted P3A content checksum mismatch')
            entry['output_sha256'] = sha256(path)
        if sha256(archive) != before or sha256(copied) != before:
            raise ValueError('Source/copied archive changed')
        manifest['state'] = 'offline_content_verified'
        manifest['source_unchanged'] = True
        record()
    except Exception as error:
        manifest['state'] = 'failed'
        manifest['error'] = str(error)
        record()
        raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--tool', type=Path)
    parser.add_argument('--workspace', type=Path, help='New isolated directory, outside source archive directory')
    parser.add_argument('--entry', help='Select one exact indexed name without expanding the archive')
    parser.add_argument('--output', type=Path, help='New selected-entry destination')
    parser.add_argument('--manifest', type=Path, help='New selected-entry provenance JSON')
    parser.add_argument('--max-expanded-mib', type=int, default=512)
    args = parser.parse_args()
    if args.entry and (not args.output or not args.manifest or args.tool or args.workspace):
        parser.error('--entry requires --output and --manifest; full-extraction arguments do not apply')
    if not args.entry and (not args.tool or not args.workspace or args.output or args.manifest):
        parser.error('Full extraction requires --tool and --workspace')
    try:
        if args.entry:
            if args.manifest.exists() or args.manifest.resolve() == args.output.resolve() or args.manifest.resolve().is_relative_to(args.archive.resolve().parent):
                raise ValueError('Selected-entry manifest must be a new independent file')
            result = extract_entry(args.archive, args.entry, args.output)
            args.manifest.parent.mkdir(parents=True,exist_ok=True)
            with args.manifest.open('x',encoding='utf-8') as stream:
                json.dump(result,stream,indent=2);stream.write('\n')
        else:
            result = extract(args.archive, args.tool, args.workspace, args.max_expanded_mib * 1024**2)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
    print(f"Verified selected entry; manifest: {args.manifest}" if args.entry else
          f"Verified {len(result['index']['entries'])} files; manifest: {args.workspace / 'manifest.json'}")


if __name__ == '__main__':
    main()
