"""Execute pinned min/max handlers in isolated Unicorn memory, never a game."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import struct

from validate_mdl_transforms import Oracle, PROFILES


def verify(path, game, cases):
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    entries = {'sky1': (0x3d7310, 0x3d7430), 'sky2': (0x4c4200, 0x4c4320)}
    hashes = ('a92a56347b06fc99c6a9c01ccff5a5d340e6ef65858b4926fc497703c4922d08',
              '4d00e4fb20ab4172919409bd78fcdb8882665ed1a3d74888084736d8d9653876')
    oracle = Oracle(path, game)
    executed, writes, rows = [], [], {}
    oracle.vm.hook_add(UC_HOOK_CODE, lambda vm, at, size, user: executed.append(at - oracle.base))
    oracle.vm.hook_add(UC_HOOK_MEM_WRITE, lambda vm, access, at, size, value, user: writes.append((at, size)))
    for index, operation in enumerate(('min', 'max')):
        entry = entries[game][index]
        body = bytes(oracle.vm.mem_read(oracle.base + entry, 280))
        if hashlib.sha256(body).hexdigest() != hashes[index]:
            raise ValueError('original native selection body changed')
        frozen = next(row['cases'] for row in cases if row['operation'] == operation)
        for count, sample in enumerate(frozen):
            a, b = sample['arguments']
            storage = bytearray(random.Random(count + 10625).randbytes(0x420))
            struct.pack_into('<Q', storage, 0x58, 0x20000c8)
            struct.pack_into('<I', storage, 0x64, 8)
            struct.pack_into('<I', storage, 0x70, 2)
            storage[0x80] = 1
            struct.pack_into('<II', storage, 0xc8, b, a)
            oracle.vm.mem_write(0x2000000, bytes(storage))
            executed.clear(); writes.clear()
            oracle.call(entry, 0, 0x2000000)
            struct.pack_into('<I', storage, 0x14, sample['result'])
            if bytes(oracle.vm.mem_read(0x2000000, len(storage))) != storage or writes != [(0x2000014, 4)]:
                raise ValueError(f'{game} {operation} case {count}: result or private storage mismatch')
            if not all(entry <= at < entry + 280 for at in executed):
                raise ValueError('native selection executed outside its pinned body')
        rows[operation] = dict(command=25 + index, handler_rva=hex(entry), body_size=280,
            code_hex=body.hex(), code_sha256=hashes[index], original_emulator_cases=len(frozen),
            only_return_register_written=True, no_external_code_executed=True)
    return dict(build_sha256=PROFILES[game][0], operations=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1', type=Path)
    parser.add_argument('sky2', type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if args.report.resolve() in {path.resolve() for path in (args.sky1, args.sky2, args.fixtures)}:
        parser.error('report must not overwrite an input')
    cases = json.loads(args.fixtures.read_text())
    report = {game: verify(getattr(args, game), game, cases) for game in ('sky1', 'sky2')}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('Both builds: 256 min and 256 max original-code cases; only return register written')


if __name__ == '__main__':
    main()
