"""Optional research verifier: emulate hash-pinned engine math, never inject.

Requires pefile and unicorn. These are verification dependencies, not exporter
dependencies. All original code, including CRT trig, executes in emulator memory.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct

import mdl

PROFILES = {
    'sky1': ('4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf',
             0x4aa190, 0x2b8330, 0x1d4430),
    'sky2': ('d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf',
             0x5dced0, 0x33c270, 0x2267b0),
}


class Oracle:
    def __init__(self, path, game):
        import pefile
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_64
        data = Path(path).read_bytes()
        profile = PROFILES[game]
        if hashlib.sha256(data).hexdigest() != profile[0]:
            raise ValueError('Executable does not match the verified transform profile')
        pe = pefile.PE(data=data)
        image = pe.get_memory_mapped_image()
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.calculate, self.euler_matrix, self.matrix_quat = profile[1:]
        self.vm = Uc(UC_ARCH_X86, UC_MODE_64)
        self.vm.mem_map(self.base, (len(image) + 4095) & ~4095)
        self.vm.mem_write(self.base, image)
        self.vm.mem_map(0x2000000, 0x20000)
        self.vm.mem_map(0x3000000, 0x1000)

    def call(self, rva, rcx, rdx=0):
        from unicorn.x86_const import UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RSP, UC_X86_REG_RIP
        stack = 0x201f008
        self.vm.mem_write(stack, struct.pack('<Q', 0x3000000))
        for reg, value in ((UC_X86_REG_RCX, rcx), (UC_X86_REG_RDX, rdx), (UC_X86_REG_RSP, stack)):
            self.vm.reg_write(reg, value)
        self.vm.emu_start(self.base + rva, 0x3000000, count=100000)
        if self.vm.reg_read(UC_X86_REG_RIP) != 0x3000000:
            raise ValueError('Original transform code exceeded the instruction budget')

    def transform(self, node):
        address = 0x2000000
        body = bytearray(0x100)
        for offset, key in ((0x80, 'translation'), (0xb0, 'quaternion'), (0xc0, 'rotation'), (0xe0, 'scale')):
            values = node[key]
            struct.pack_into('<' + 'f' * len(values), body, offset, *values)
        self.vm.mem_write(address, bytes(body))
        self.call(self.euler_matrix, address + 0x1000, address + 0xc0)
        self.call(self.matrix_quat, address + 0xd0, address + 0x1000)
        pivot = node['auxiliary']
        self.vm.mem_write(address + 0xf0, struct.pack('<Q', address + 0x1100 if any(pivot) else 0))
        self.vm.mem_write(address + 0x1100, struct.pack('<3f', *pivot))
        self.call(self.calculate, address)
        return list(struct.unpack('<16f', self.vm.mem_read(address, 64)))


def gltf_matrix(node):
    translation, quaternion = mdl.static_transform(node)
    x, y, z, w = quaternion
    a, b, c = node['scale']
    return [(1-2*(y*y+z*z))*a, 2*(x*y+z*w)*a, 2*(x*z-y*w)*a, 0,
            2*(x*y-z*w)*b, (1-2*(x*x+z*z))*b, 2*(y*z+x*w)*b, 0,
            2*(x*z+y*w)*c, 2*(y*z-x*w)*c, (1-2*(x*x+y*y))*c, 0, *translation, 1]


def cases():
    rng = random.Random(614)
    f32 = lambda x: struct.unpack('<f', struct.pack('<f', x))[0]
    result = []
    for index in range(200):
        raw = [rng.uniform(-1, 1) for _ in range(4)]
        norm = math.sqrt(sum(value * value for value in raw))
        result.append(dict(name=f'random-{index}', source=dict(
            translation=[f32(rng.uniform(-10, 10)) for _ in range(3)],
            quaternion=[f32(value / norm) for value in raw],
            rotation=[f32(rng.uniform(-6, 6)) for _ in range(3)],
            scale=[f32(rng.uniform(-4, 4)) for _ in range(3)],
            auxiliary=[f32(rng.uniform(-5, 5)) for _ in range(3)])))
    for index, (rotation, scale, pivot) in enumerate([
            ((0, 0, 0), (2, 3, 4), (1, 2, 3)),
            ((.2, .3, .4), (2, 3, 4), (0, 0, 0)),
            ((.2, .3, .4), (2, 3, 4), (1, 2, 3)),
            ((math.pi/2, 0, 0), (0, -2, 3), (1, 0, 2))]):
        result.append(dict(name=f'edge-{index}', source=dict(
            translation=[4, 5, 6], quaternion=[0, 0, 0, 1],
            rotation=[f32(value) for value in rotation], scale=list(scale), auxiliary=list(pivot))))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1', type=Path)
    parser.add_argument('sky2', type=Path)
    parser.add_argument('--model', type=Path, action='append', default=[])
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path)
    args = parser.parse_args()
    samples = cases()
    models = []
    for path in args.model:
        data = path.read_bytes()
        model = mdl.parse(data)
        if not model['hierarchy']:
            raise ValueError('Sample has no hierarchy')
        for node in model['hierarchy']['nodes']:
            samples.append(dict(name=f'{path.name}:{node["name"]}', source={key: node[key] for key in
                ('translation', 'quaternion', 'rotation', 'scale', 'auxiliary')}))
        models.append(dict(path=str(path), sha256=hashlib.sha256(data).hexdigest()))
    report = dict(models=models, games={}, scope='Original local transform code in isolated x86-64 emulation; no game writes')
    fixtures = dict(profiles=PROFILES, cases=[])
    for game in PROFILES:
        oracle = Oracle(getattr(args, game), game)
        worst = 0
        for index, case in enumerate(samples):
            native = oracle.transform(case['source'])
            error = max(abs(a-b) for a, b in zip(native, gltf_matrix(case['source'])))
            if not math.isfinite(error) or error >= 3e-5:
                raise ValueError(f'{game} {case["name"]}: matrix error {error}')
            worst = max(worst, error)
            if game == 'sky1' and (index < 8 or case['name'].startswith('edge-')):
                fixtures['cases'].append(dict(**case, matrix=native))
            if game == 'sky2' and (index < 8 or case['name'].startswith('edge-')):
                first = next(row for row in fixtures['cases'] if row['name'] == case['name'])
                if native != first['matrix']:
                    raise ValueError('The two original engines disagree on a fixture')
        report['games'][game] = dict(cases=len(samples), max_absolute_error=worst, executable_sha256=PROFILES[game][0])
        print(game, report['games'][game])
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if args.fixtures:
        args.fixtures.parent.mkdir(parents=True, exist_ok=True)
        args.fixtures.write_text(json.dumps(fixtures, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
