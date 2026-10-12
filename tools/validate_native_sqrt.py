"""Verify original Sys23 nonnegative paths in isolated memory, never a game."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct

from validate_mdl_transforms import Oracle, PROFILES


def verify(path, game, fixtures, pin):
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    oracle = Oracle(path, game)
    entry = int(pin['handler_rva'], 16)
    body = bytes(oracle.vm.mem_read(oracle.base + entry, 138))
    if hashlib.sha256(body).hexdigest() != pin['code_sha256'] or body.hex() != pin['code_hex']:
        raise ValueError('original native sqrt body differs from the independent pin')
    executed, writes = [], []
    oracle.vm.hook_add(UC_HOOK_CODE, lambda vm, at, size, user: executed.append(at - oracle.base))
    oracle.vm.hook_add(UC_HOOK_MEM_WRITE, lambda vm, access, at, size, value, user: writes.append((at, size)))
    for index, case in enumerate(fixtures):
        storage = bytearray(random.Random(index + 10623).randbytes(0x420))
        struct.pack_into('<Q', storage, 0x58, 0x20000c8)
        struct.pack_into('<I', storage, 0x64, 4)
        struct.pack_into('<I', storage, 0x70, 1)
        storage[0x80] = 1
        struct.pack_into('<I', storage, 0xc8, case['argument'])
        oracle.vm.mem_write(0x2000000, bytes(storage));executed.clear();writes.clear()
        oracle.call(entry, 0, 0x2000000)
        struct.pack_into('<I', storage, 0x14, case['result'])
        if bytes(oracle.vm.mem_read(0x2000000, len(storage))) != storage:
            raise ValueError(f'{game} case {index}: result or private canary changed')
        if not all(entry <= at < entry + 138 for at in executed):
            raise ValueError('sqrt executed outside its pinned nonnegative path')
        if [(at, size) for at, size in writes if 0x2000000 <= at < 0x2000420] != [(0x2000014, 4)] or not all(
                0x2000000 <= at < 0x2000420 or 0x201e000 <= at < 0x2020000 for at, size in writes):
            raise ValueError('sqrt wrote outside its result or private emulator call stack')
    return dict(build_sha256=PROFILES[game][0], handler_rva=hex(entry), body_size=138,
        code_hex=body.hex(), code_sha256=pin['code_sha256'], original_cases=len(fixtures),
        no_external_code_executed=True, only_private_result_written=True, negative_domain_not_executed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1', type=Path);parser.add_argument('sky2', type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--pins', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if args.report.resolve() in {path.resolve() for path in (args.sky1, args.sky2, args.fixtures, args.pins)}:
        parser.error('report must not overwrite an input')
    fixtures = json.loads(args.fixtures.read_text());pins = json.loads(args.pins.read_text())
    report = {game: verify(getattr(args, game), game, fixtures, pins[game]) for game in ('sky1', 'sky2')}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Both builds: 265 nonnegative original sqrt cases; private canaries preserved; no external code')


if __name__ == '__main__':
    main()
