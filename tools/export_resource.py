"""Export a pinned Sky resource through its verified engine stream and active overlays."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
from console import execute
from windows_process import Process


def export(pid, mod, resource, output, max_bytes=256 * 1024 * 1024):
    if not resource.isascii() or any(ord(ch) < 32 for ch in resource):
        raise ValueError('Resource paths must use printable ASCII')
    if max_bytes < 1 or max_bytes > 0x7fffffff:
        raise ValueError('Maximum size must be within 1..INT32_MAX')
    output = Path(output).resolve()
    with Process(pid) as process:
        game_directory = process.path.parent.resolve()
        if output == game_directory or game_directory in output.parents:
            raise ValueError('Export destination must be outside the game installation')
    digest = execute(pid, mod, "return fdk.call('game.build_sha256')")
    if digest not in ('d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf',
                      '4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf'):
        raise ValueError('Engine resource export requires a supported pinned Sky build')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', suffix='.tmp', delete=False) as target:
            temporary = Path(target.name)
            offset, total = 0, None
            checksum = hashlib.sha256()
            while total is None or offset < total:
                # Hex keeps binary bytes intact through the text console protocol.
                reply = execute(pid, mod, f"""
                    local data, size = fdk.invoke('engine.resource_read', {json.dumps(resource)}, {offset}, 4096)
                    return (data:gsub('.', function(ch) return string.format('%02x', string.byte(ch)) end)), size
                """)
                encoded, reported = reply.rsplit('\t', 1)
                length = int(reported)
                if length < 0 or length > max_bytes:
                    raise ValueError(f'Resource exceeds export limit: {length} bytes')
                if total is not None and total != length:
                    raise RuntimeError('Resource length changed during export')
                total = length
                data = bytes.fromhex(encoded)
                if len(data) > 4096 or offset + len(data) > total or not data and offset < total:
                    raise RuntimeError('Engine returned an incomplete or invalid resource page')
                target.write(data)
                checksum.update(data)
                offset += len(data)
        os.replace(temporary, output)
        temporary = None
        return {'resource': resource, 'size_bytes': total, 'sha256': checksum.hexdigest(),
                'build_sha256': digest, 'destination': str(output)}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('resource', help='Relative engine resource path, including language mapping')
    parser.add_argument('--mod', default='HelloFDK')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-bytes', type=int, default=256 * 1024 * 1024)
    args = parser.parse_args()
    print(json.dumps(export(args.pid, args.mod, args.resource, args.output, args.max_bytes), indent=2))


if __name__ == '__main__':
    main()
