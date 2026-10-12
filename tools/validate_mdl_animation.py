"""Optional hash-pinned engine-code verifier for bounded motion sampling.

Runs original local executable code only in Unicorn memory. No game injection,
host function calls, resource writes, or playback claims. Requires the optional
model-verification dependencies. Coordinates remain in the source domain.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import random
import struct

from mdl_animation import LinearChannel, inspect, float_order, ordered_float, WIDTHS
from validate_mdl_transforms import Oracle, PROFILES

SAMPLERS = {'sky1': 0x4dd610, 'sky2': 0x617ad0}


class AnimationOracle(Oracle):
    def prepare(self, record):
        from unicorn.x86_const import UC_X86_REG_R8
        # Validate the bounded, supported graph before copying it to emulator memory.
        LinearChannel(record)
        keys = record['keys']
        body = bytearray(0x58)
        struct.pack_into('<3I', body, 0x34, record['raw_kind'], *record['raw_words'])
        struct.pack_into('<3Q', body, 0x40, 0x4000000, len(keys), len(keys))
        self.vm.mem_write(0x2000000, bytes(body))
        native = b''.join(struct.pack('<4ff', *(key['raw_values']+[0]*(4-len(key['raw_values']))),
                                    key['raw_coordinate']) + bytes.fromhex(key['raw_extra_hex']) for key in keys)
        self.vm.mem_write(0x4000000, native)
        self.width = len(keys[0]['raw_values'])
        self.vm.reg_write(UC_X86_REG_R8, 0x2001000)

    def sample(self, coordinate, game):
        from unicorn.x86_const import UC_X86_REG_R8, UC_X86_REG_XMM1
        self.vm.reg_write(UC_X86_REG_R8, 0x2001000)
        self.vm.reg_write(UC_X86_REG_XMM1, struct.unpack('<I', struct.pack('<f', coordinate))[0])
        self.call(SAMPLERS[game], 0x2000000)
        return list(struct.unpack('<4f', self.vm.mem_read(0x2001000, 16)))[:self.width]


def synthetic():
    rng = random.Random(618710)
    f32 = lambda value: struct.unpack('<f', struct.pack('<f', value))[0]
    records = []
    for index in range(120):
        kind = (9, 10, 11)[index % 3]
        keys = []
        time = f32(rng.uniform(-3, 3))
        for _ in range(rng.randint(1, 12)):
            value = [rng.uniform(-1, 1) for _ in range(4 if kind == 10 else 3)]
            if kind == 10:
                length = math.sqrt(sum(v*v for v in value))
                value = [v/length for v in value]
            keys.append(dict(raw_coordinate=time, raw_values=[f32(v) for v in value],
                             raw_extra_hex=struct.pack('<I4f', 0, 7, 11, -3, 4).hex()))
            time = f32(time+rng.uniform(.02, 3))
        records.append(dict(name=f'synthetic-{index}', target='fixture', raw_kind=kind,
                            raw_words=[0, 0], keys=keys))
    # Explicitly exercise the sign convention and unnormalized near-angle fallback.
    for name, end in [('opposite', (0,0,0,-1)), ('near-angle', (0,0,.01,math.sqrt(.9999)))]:
        records.append(dict(name=name,target='fixture',raw_kind=10,raw_words=[0,0],keys=[
            dict(raw_coordinate=2,raw_values=[0,0,0,1],raw_extra_hex='00'*20),
            dict(raw_coordinate=4,raw_values=[f32(v) for v in end],raw_extra_hex='00'*20)]))
    return records


def easing_records():
    """Uniform and mixed left-key modes; opaque tangent fields stay nonzero."""
    records = []
    for mode in (0, 4, 5, 6):
        for row in synthetic():
            row = copy.deepcopy(row)
            row['name'] += f'-mode{mode}'
            for key in row['keys']:
                key['raw_extra_hex'] = struct.pack('<I4f', mode, 7, 11, -3, 4).hex()
            records.append(row)
    for row in synthetic()[:30]:
        row = copy.deepcopy(row)
        row['name'] += '-mixed'
        for i, key in enumerate(row['keys']):
            key['raw_extra_hex'] = struct.pack('<I4f', (0, 4, 5, 6)[i % 4], 7, 11, -3, 4).hex()
        records.append(row)
    return records


def cubic_records():
    records=[]
    for suffix,modes in [('cubic',(3,)),('cubic-linear',(3,0)),('mixed-cubic',(0,3,4,3,5,3,6))]:
        for source in synthetic():
            row=copy.deepcopy(source);row['name']+='-'+suffix
            for i,key in enumerate(row['keys']):
                key['raw_extra_hex']=struct.pack('<I4f',modes[i%len(modes)],7,11,-3,4).hex()
            records.append(row)
    return records


def component_records():
    records=[]
    for kind in range(9):
        for modes in ((0,),(3,),(4,),(5,),(6,),(0,3,4,3,5,3,6)):
            for index,source in enumerate(synthetic()[::3][:12]):
                row=copy.deepcopy(source);row.update(raw_kind=kind,name=f'component-{kind}-modes{modes}-{index}')
                for i,key in enumerate(row['keys']):
                    key['raw_values']=key['raw_values'][kind%3:kind%3+1]
                    key['raw_extra_hex']=modes[i%len(modes)].to_bytes(4,'little').hex()+key['raw_extra_hex'][8:]
                records.append(row)
    return records


def scalar_curve_records():
    """All scalar destinations, clamped ratios, zero/negative distances and dispatch."""
    rng=random.Random(106202)
    f32=lambda value:struct.unpack('<f',struct.pack('<f',value))[0]
    sources=component_records();records=[]
    for kind in range(9):
        source=next(row for row in sources if row['raw_kind']==kind and len(row['keys'])>=3)
        for variant in range(12):
            row=copy.deepcopy(source);row['name']=f'scalar-curve-kind{kind}-variant{variant}'
            for i,key in enumerate(row['keys']):
                time=key['raw_coordinate'];offset=(.01,.1,1,100,0,-1)[variant%6]
                mode=2 if variant<6 else (2,0,2,3,2,4,2,5,2,6)[i%10]
                controls=[f32(time-offset),f32(rng.uniform(-2,2)),f32(time+offset),f32(rng.uniform(-2,2))]
                key['raw_extra_hex']=struct.pack('<I4f',mode,*controls).hex()
            records.append(row)
    return records


def step_records():
    records=[]
    sources=synthetic()+[row for row in component_records() if 'modes(0,)' in row['name']]
    for source in sources:
        variants=[(1,),(1,0,1,3,1,4,1,5,1,6),(3,1,0,1,4,1,5,1,6,1)]
        if source['raw_kind']<9:variants.append((2,1,2,0,2,3,2,4,2,5,2,6))
        for variant,modes in enumerate(variants):
            row=copy.deepcopy(source);row['name']+=f'-step{variant}'
            for i,key in enumerate(row['keys']):
                key['raw_extra_hex']=struct.pack('<I4f',modes[i%len(modes)],7,11,-3,4).hex()
            records.append(row)
    for kind in (0,9,10):
        source=next(row for row in sources if row['raw_kind']==kind and len(row['keys'])>=3)
        row=copy.deepcopy(source);row['name']=f'step-export-kind{kind}';row['keys']=row['keys'][:3]
        for time,key in zip((0,1,3),row['keys']):
            key['raw_coordinate']=time;key['raw_extra_hex']=struct.pack('<I4f',1,7,11,-3,4).hex()
        records.append(row)
    return records


def pre_post_records():
    sources=synthetic()[:18]+[row for row in component_records() if 'modes(0,)' in row['name'] and int(row['name'].split('-')[-1])<6]
    sources.extend(copy.deepcopy(next(row for row in sources if row['raw_kind']==kind and len(row['keys'])>1)) for kind in range(9))
    records=[]
    for index,source in enumerate(sources):
        for pre in range(6):
            for post in range(6):
                row=copy.deepcopy(source);row.update(name=f'prepost-source{index}-kind{source["raw_kind"]}-{pre}-{post}',raw_words=[pre,post])
                modes=(2,) if index>=72 else ((0,),(1,),(3,),(4,),(5,),(6,),(0,3,0,4,5,6))[index%7]
                for i,key in enumerate(row['keys']):key['raw_extra_hex']=struct.pack('<I4f',modes[i%len(modes)],7,11,-3,4).hex()
                records.append(row)
    return records


def vector_curve_records():
    records=[]
    for kind in (9,10,11):
        for source in [row for row in synthetic() if row['raw_kind']==kind][:12]:
            for variant,modes in enumerate(((2,),(2,0,2,3,2,4,2,5,2,6),(3,2,0,2,4,2,5,2,6,2))):
                for boundary in range(6):
                    row=copy.deepcopy(source);row.update(name=f'vector-curve-{source["name"]}-variant{variant}-boundary{boundary}',raw_words=[boundary,boundary])
                    for i,key in enumerate(row['keys']):
                        time=key['raw_coordinate'];value=key['raw_values'][0]
                        key['raw_extra_hex']=struct.pack('<I4f',modes[i%len(modes)],time-.5,value,time+.5,value).hex()
                    records.append(row)
    for kind in (9,10,11):
        values=(0,.08,.02) if kind==9 else (1,1,1) if kind==10 else (.9,1.1,1)
        times=(0,1,3);keys=[]
        for i,(time,value) in enumerate(zip(times,values)):
            incoming=(time-times[i-1])/3 if i else 1/3
            outgoing=(times[i+1]-time)/3 if i+1<len(times) else 2/3
            value=struct.unpack('<f',struct.pack('<f',value))[0]
            keys.append(dict(raw_coordinate=time,raw_values=[value]+[0]*(WIDTHS[kind]-1),
                raw_extra_hex=struct.pack('<I4f',2,time-incoming,value+.01,time+outgoing,value+.01).hex()))
        records.append(dict(name=f'vector-curve-export-kind{kind}',target='fixture',raw_kind=kind,raw_words=[0,0],keys=keys))
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sky1', type=Path); parser.add_argument('sky2', type=Path)
    parser.add_argument('--motion', type=Path, action='append', default=[])
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path)
    variants=parser.add_mutually_exclusive_group()
    variants.add_argument('--easing', action='store_true', help='Verify uniform/mixed modes 0/4/5/6 with dense segment samples')
    variants.add_argument('--cubic', action='store_true', help='Verify uniform/mixed mode 3, including delegation and endpoint-step fallback')
    variants.add_argument('--components', action='store_true', help='Verify all nine component kinds with uniform/mixed supported modes')
    variants.add_argument('--scalar-curves', action='store_true', help='Verify scalar mode 2 controls, clamped ratios and mixed-mode dispatch')
    variants.add_argument('--steps', action='store_true', help='Verify mode 1, mixed dispatch and float32 neighbors of every key boundary')
    variants.add_argument('--pre-post', action='store_true', help='Verify all36 boundary-mode pairs, repeats, offsets, reflection and native extrapolation')
    variants.add_argument('--vector-curves', action='store_true', help='Verify whole-vector mode2 zeroed components, mixed dispatch and boundary offsets')
    args = parser.parse_args()
    records = vector_curve_records() if args.vector_curves else pre_post_records() if args.pre_post else step_records() if args.steps else scalar_curve_records() if args.scalar_curves else component_records() if args.components else cubic_records() if args.cubic else easing_records() if args.easing else synthetic(); motions = []
    for path in args.motion:
        data = path.read_bytes(); clip = inspect(data)
        records.extend(clip['records'])
        motions.append(dict(path=str(path),sha256=hashlib.sha256(data).hexdigest(),records=len(clip['records'])))
    report = dict(scope='Isolated original bounded sampler code; source coordinate domain; no gameplay writes',
                  interpolation_modes=sorted({struct.unpack_from('<I',bytes.fromhex(key['raw_extra_hex']))[0]
                                              for row in records for key in row['keys']}),
                  channel_kinds=sorted({row['raw_kind'] for row in records}),
                  pre_post_modes=sorted({mode for row in records for mode in row['raw_words']}),
                  motions=motions, games={})
    fixtures = dict(profiles={game:dict(executable_sha256=PROFILES[game][0],sampler_rva=hex(SAMPLERS[game]))
                             for game in SAMPLERS},cases=[])
    for game in SAMPLERS:
        oracle = AnimationOracle(getattr(args, game), game)
        oracle.vm.mem_map(0x4000000, 0x400000)  # Maximum validated key storage: 100000 * 40.
        worst, count = 0, 0
        for index, record in enumerate(records):
            sampler = LinearChannel(record); oracle.prepare(record)
            times = sampler.times
            fractions = (.01,.1,.25,.49,.5,.51,.75,.9,.99) if args.easing or args.cubic or args.components or args.scalar_curves or args.steps else (.5,)
            coordinates = [times[0]-1, times[-1]+1, *times,
                           *(a+(b-a)*f for a,b in zip(times,times[1:]) for f in fractions)]
            if args.steps:
                coordinates.extend(ordered_float(float_order(time)+offset) for time in times for offset in (-2,-1,1,2))
            if args.pre_post:
                period=times[-1]-times[0] or 1
                coordinates=[times[0]-period*f for f in (.01,.25,.5,1,1.25,2,3,4.25)]+[times[-1]+period*f for f in (0,.01,.25,.5,1,1.25,2,3,4.25)]
                coordinates.extend(v for t in times for v in (ordered_float(float_order(t)-1),t,ordered_float(float_order(t)+1)))
                coordinates.extend(a+(b-a)*f for a,b in zip(times,times[1:]) for f in (.25,.5,.75))
            if args.vector_curves:
                period=times[-1]-times[0] or 1
                coordinates=[times[0]-period*f for f in (.1,.5,1,2.25)]+[times[-1]+period*f for f in (0,.1,.5,1,2.25)]
                coordinates.extend(v for t in times for v in (ordered_float(float_order(t)-1),t,ordered_float(float_order(t)+1)))
                coordinates.extend(a+(b-a)*f for a,b in zip(times,times[1:]) for f in (.01,.1,.25,.49,.5,.51,.75,.9,.99))
            native_rows = []
            for coordinate in coordinates:
                native = oracle.sample(coordinate, game)
                error = max(abs(a-b) for a,b in zip(native, sampler.sample(coordinate)))
                if not math.isfinite(error) or error > 2e-5:
                    raise ValueError(f'{game}: {record["name"]}, {coordinate}: error {error}')
                worst = max(worst,error); count += 1
                native_rows.append(dict(coordinate=coordinate,result=native))
            freeze = (index<18 or record['name'].startswith('step-export-') or record['name'].split('-step')[0].endswith('-0')) if args.steps else (index%12 in (0,4,5,6)) if args.scalar_curves else (index%12==0) if args.components else (index%122 in (0,1,2,3,4,5,120,121)) if args.cubic else (index % 122 in (0,1,2,120,121) or index>=488) if args.easing else (index<6 or record['name'] in ('opposite','near-angle'))
            if args.pre_post:
                freeze=index//36 in (0,1,2,4,7,10,13,16,18,24,30,36,42,48,54,60,66,72,73,74,75,76,77,78,79,80) and record['raw_words'][0]==record['raw_words'][1]
            if args.vector_curves:freeze=index//18 in (0,1,2,12,13,14,24,25,26) or record['name'].startswith('vector-curve-export-')
            if freeze:
                if game == 'sky1':
                    fixtures['cases'].append(dict(record=record,samples=native_rows))
                else:
                    first = next(row for row in fixtures['cases'] if row['record']['name']==record['name'])
                    if native_rows != first['samples']:
                        raise ValueError('The two original engines disagree on a frozen fixture')
        report['games'][game] = dict(records=len(records),samples=count,max_absolute_error=worst,
                                    **fixtures['profiles'][game])
        print(game,report['games'][game])
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if args.fixtures:
        args.fixtures.parent.mkdir(parents=True,exist_ok=True)
        args.fixtures.write_text(json.dumps(fixtures,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
