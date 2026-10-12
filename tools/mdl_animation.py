"""Inspect MDL v4 motion records and sample verified bounded channels."""
import argparse
import bisect
import hashlib
import json
import math
import struct
from pathlib import Path

from mdl import Reader, matrix_product, parse

# Independently decoded from the pinned engine's channel-width jump table.
WIDTHS = (1,1,1,1,1,1,1,1,1,3,4,3,1,2,3,4,1)


def native_fraction(coordinate,left,right):
    """Original SSE subtraction/division rounding, used by endpoint steps."""
    f32=lambda value:struct.unpack('<f',struct.pack('<f',value))[0]
    try:
        result=f32(f32(coordinate-left)/f32(right-left))
    except (OverflowError,struct.error,ZeroDivisionError) as error:
        raise ValueError('Native step fraction exceeds finite float32 arithmetic') from error
    if not math.isfinite(result):raise ValueError('Native step fraction exceeds finite float32 arithmetic')
    return result


def float_order(value):
    bits=struct.unpack('<I',struct.pack('<f',value))[0]
    return (~bits & 0xffffffff) if bits & 0x80000000 else bits ^ 0x80000000


def ordered_float(order):
    bits=(order ^ 0x80000000) if order & 0x80000000 else (~order & 0xffffffff)
    return struct.unpack('<f',struct.pack('<I',bits))[0]


def channel_destination(kind):
    """Verified binder fields; kinds3..5 address quaternion XYZ, not Euler."""
    if 0<=kind<9:
        return ('translation','quaternion','scale')[kind//3],kind%3
    if kind in (9,10,11):return {9:'translation',10:'quaternion',11:'scale'}[kind],None
    raise ValueError('Unsupported transform channel kind')


class LinearChannel:
    """Native bounded curves in source coordinates; no clip clock or blending.

    Restricts support to kinds 0..11 with pre/post modes 0..5. The engine
    switches to unnormalized linear quaternion interpolation at dot >= .999.
    Its shortest-arc convention negates the left quaternion, not the right.
    """
    def __init__(self, record):
        kind = record['raw_kind']
        words=record['raw_words']
        if kind not in range(12) or not isinstance(words,(list,tuple)) or len(words)!=2 or any(not isinstance(v,int) or v not in range(6) for v in words):
            raise ValueError('Only kinds 0..11 with pre/post modes 0..5 are supported')
        keys = record['keys']
        if not keys or len(keys) > 100000:
            raise ValueError('Sampling requires 1..100000 keys')
        times, values, modes, controls = [], [], [], []
        for key in keys:
            time, value = key['raw_coordinate'], tuple(key['raw_values'])
            extra = bytes.fromhex(key['raw_extra_hex'])
            if len(extra) != 20:
                raise ValueError('Invalid interpolation payload')
            mode = struct.unpack_from('<I', extra)[0]
            if mode not in (0, 1, 2, 3, 4, 5, 6):
                raise ValueError('Only interpolation modes 0/1/2/3/4/5/6 are supported')
            control=struct.unpack_from('<4f',extra,4)
            if mode==2 and not all(math.isfinite(v) for v in control):
                raise ValueError('Mode 2 requires finite curve controls')
            if len(value) != WIDTHS[kind] or not all(math.isfinite(v) for v in (time, *value)):
                raise ValueError('Invalid channel coordinate or values')
            if times and time <= times[-1]:
                raise ValueError('Key coordinates must strictly increase')
            if kind == 10 and abs(sum(v*v for v in value)-1) > 1e-5:
                raise ValueError('Quaternion keys must have unit length')
            times.append(time); values.append(value); modes.append(mode); controls.append(control)
        self.kind, self.times, self.values = kind, tuple(times), tuple(values)
        self.pre_post=tuple(words)
        self.modes = tuple(modes)
        self.controls = tuple(controls)
        self.step_segments=tuple(a==1 or a in (2,3) and b in (1,4,5,6) for a,b in zip(modes,modes[1:]))
        self.curve2_segments=tuple(a in (2,3) and b==2 for a,b in zip(modes,modes[1:]))
        if any(active and not all(math.isfinite(v) for v in controls[i][2:])
               for i,active in enumerate(self.curve2_segments)):
            raise ValueError('Mode 2 requires finite curve controls on the active segment')
        # The native mode2 polynomial computes only X. At switches into/out
        # of that path, a nonzero key tail can jump to/from zero. Tails at
        # interior keys shared by two mode2 segments remain unused.
        self.curve_component_jumps=any(
            ((self.curve2_segments[i-1] if i else False)!=
             (self.curve2_segments[i] if i<len(self.curve2_segments) else False)) and
            any(v!=0 for v in row[1:]) for i,row in enumerate(values))
        self.discontinuous=any(self.step_segments) or self.curve_component_jumps
        self.step_only=all(self.step_segments)

    def step_transitions(self):
        """First float32 coordinate choosing each segment's right endpoint."""
        result=[]
        for left,right,step in zip(self.times,self.times[1:],self.step_segments):
            if not step:continue
            low,high=float_order(left),float_order(right)
            if ordered_float(low)!=left or ordered_float(high)!=right:
                raise ValueError('Step coordinates must be exactly representable in float32')
            while low+1<high:
                middle=(low+high)//2
                if native_fraction(ordered_float(middle),left,right)>=1:high=middle
                else:low=middle
            result.append(ordered_float(high))
        return result

    def sample(self, coordinate):
        if not math.isfinite(coordinate):
            raise ValueError('Sampling coordinate must be finite')
        # The engine argument is float32. Round before selecting the segment.
        try:
            coordinate = struct.unpack('<f', struct.pack('<f', coordinate))[0]
        except (OverflowError, struct.error) as error:
            raise ValueError('Sampling coordinate exceeds float32') from error
        if not math.isfinite(coordinate):
            raise ValueError('Sampling coordinate exceeds float32')
        if len(self.times)==1:return list(self.values[0])
        if coordinate<self.times[0] or coordinate>=self.times[-1]:
            pre=coordinate<self.times[0];mode=self.pre_post[0 if pre else 1]
            endpoint=self.values[0 if pre else -1]
            if mode in (0,2):return list(endpoint)
            def fp(value):
                try:result=struct.unpack('<f',struct.pack('<f',value))[0]
                except (OverflowError,struct.error) as error:
                    raise ValueError('Pre/post arithmetic exceeds finite float32 storage') from error
                if not math.isfinite(result):raise ValueError('Pre/post arithmetic exceeds finite float32 storage')
                return result
            first,last=self.times[0],self.times[-1]
            distance=fp(first-coordinate if pre else coordinate-last)
            if mode==1:
                neighbor=self.values[1 if pre else -2]
                duration=fp(self.times[1]-first if pre else self.times[-2]-last)
                if not duration:raise ValueError('Pre/post key interval collapses in float32')
                # The native pre branch adds the first-to-second difference,
                # despite moving backwards in time. Post uses previous-last
                # over its negative time interval. Neither path uses Slerp.
                return [fp(fp(fp(fp(b-a)*distance)/duration)+a) for a,b in zip(endpoint,neighbor)]
            period=fp(last-first)
            if not period:raise ValueError('Pre/post period collapses in float32')
            divisor=fp(period+period) if mode==5 else period
            cycles=fp(math.ceil(fp(distance/divisor)))
            if mode==5:
                advance=fp(fp(cycles+cycles)*period)
                coordinate=(fp(last-abs(fp(fp(advance+coordinate)-last))) if pre else
                            fp(first+abs(fp(fp(coordinate-advance)-first))))
                return self._sample_inside(coordinate)
            advance=fp(cycles*period)
            coordinate=fp(coordinate+advance if pre else coordinate-advance)
            result=self._sample_inside(coordinate)
            if mode==4:
                right=bisect.bisect_left(self.times,coordinate)
                if self.kind==10 and 0<right<len(self.times) and not self.step_segments[right-1]:
                    effective=self.modes[right-1]
                    if effective in (2,3):effective=self.modes[right]
                    # The native quaternion interpolation return bypasses
                    # accumulated loop offsets; cubic and step paths do not.
                    if effective in (0,4,5,6):return result
                offsets=[fp(fp(b-a)*cycles) for a,b in zip(self.values[0],self.values[-1])]
                result=[fp(value-offset if pre else value+offset) for value,offset in zip(result,offsets)]
            return result
        return self._sample_inside(coordinate)

    def _sample_inside(self,coordinate):
        right = bisect.bisect_left(self.times, coordinate)
        if right == 0:
            return list(self.values[0])
        if right == len(self.times):
            return list(self.values[-1])
        left, end = self.values[right-1], self.values[right]
        fraction = (coordinate-self.times[right-1])/(self.times[right]-self.times[right-1])
        mode = self.modes[right-1]
        if self.step_segments[right-1]:
            return list(left if native_fraction(coordinate,self.times[right-1],self.times[right])<1 else end)
        if mode in (2,3):
            # The original sampler delegates to the right mode when it is not
            # the same curve mode. That second dispatch does not apply easing modes;
            # they take the native endpoint-step fallback instead.
            mode = self.modes[right]
            if mode == 3:
                previous = self.values[right-2] if right>1 else None
                following = self.values[right+1] if right+1<len(self.values) else None
                t2,t3 = fraction*fraction,fraction*fraction*fraction
                result=[]
                for i,(a,b) in enumerate(zip(left,end)):
                    tangent_a=(b-previous[i])*.5 if previous is not None else b-a
                    tangent_b=(following[i]-a)*.5 if following is not None else b-a
                    result.append((2*t3-3*t2+1)*a+(t3-2*t2+fraction)*tangent_a+
                                  (-2*t3+3*t2)*b+(t3-t2)*tangent_b)
                return result
            if mode == 2:
                duration=self.times[right]-self.times[right-1]
                incoming=self.times[right]-self.controls[right][0]
                outgoing=self.controls[right-1][2]-self.times[right-1]
                def ratio(distance):
                    value=duration/distance if distance else math.copysign(math.inf,distance)
                    return max(0.009999999776482582,min(100,value))
                a=ratio(incoming)*self.controls[right][1]
                b=ratio(outgoing)*self.controls[right-1][3]
                t=fraction;s=1-t
                value=s*s*s*left[0]+s*s*t*b+s*t*t*a+t*t*t*end[0]
                if not math.isfinite(value) or abs(value)>3.4028234663852886e38:
                    raise ValueError('Scalar curve result exceeds finite float32 storage')
                return [value]+[0.0]*(WIDTHS[self.kind]-1)
        if mode == 4:
            fraction *= fraction
        elif mode == 5:
            fraction *= 2-fraction
        elif mode == 6:
            fraction = 2*fraction*fraction if fraction < .5 else (4-2*fraction)*fraction-1
        if self.kind != 10:
            return [a+(b-a)*fraction for a,b in zip(left,end)]
        dot = sum(a*b for a,b in zip(left,end))
        if dot < 0:
            left, dot = tuple(-v for v in left), -dot
        if dot < 0.9990000128746033:
            angle = math.acos(min(1, dot))
            divisor = math.sin(angle)
            a, b = math.sin((1-fraction)*angle)/divisor, math.sin(fraction*angle)/divisor
        else:
            a, b = 1-fraction, fraction
        return [a*x+b*y for x,y in zip(left,end)]


def inspect(data):
    if len(data)>128*1024*1024:
        raise ValueError('Animation source exceeds 128 MiB')
    reader=Reader(data)
    magic,version,header_word=reader.unpack('<4sII')
    if magic!=b'MDL ' or version!=4:
        raise ValueError('Expected uncompressed MDL version 4')
    sections={}
    while True:
        kind=reader.u32()
        if kind==0xffffffff:
            reader.done();break
        if kind in sections:
            raise ValueError('Duplicate MDL section')
        sections[kind]=reader.take(reader.u32())
    if 3 not in sections:
        raise ValueError('MDL has no animation records')
    tracks=Reader(sections[3]);count=tracks.u32()
    if count>10000:
        raise ValueError('Too many animation records')
    records=[];total=0
    for _ in range(count):
        name,target=tracks.name(),tracks.name()
        kind,word1,word2,keys=tracks.unpack('<4I')
        if not name or not target or '\0' in name+target or kind>=len(WIDTHS):
            raise ValueError('Invalid animation name or channel kind')
        total+=keys
        if keys>100000 or total>1000000:
            raise ValueError('Too many animation keys')
        width=WIDTHS[kind];rows=[]
        for _ in range(keys):
            coordinate=tracks.unpack('<f')[0]
            values=tracks.unpack('<'+'f'*width)
            extra=tracks.take(20)
            if not all(math.isfinite(v) for v in (coordinate,*values)):
                raise ValueError('Nonfinite animation key')
            rows.append(dict(raw_coordinate=coordinate,raw_values=list(values),raw_extra_hex=bytes(extra).hex()))
        records.append(dict(name=name,target=target,raw_kind=kind,raw_words=[word1,word2],component_count=width,keys=rows))
    tail=tracks.unpack('<2f');tracks.done()
    if not all(math.isfinite(v) for v in tail):
        raise ValueError('Nonfinite animation trailer')
    return dict(format='fdkss.mdl-animation-inspection',format_version=1,source_sha256=hashlib.sha256(data).hexdigest(),
                mdl_version=version,raw_header_word=header_word,records=records,raw_trailer=list(tail),
                sections={str(k):dict(byte_length=len(v),sha256=hashlib.sha256(v).hexdigest()) for k,v in sections.items()},
                limitations=['Full clip application and coordinate units are not verified; raw fields retain their source representation.',
                             'This is inspection metadata, not animation conversion or playback.'])


def composed_quaternion(source):
    """Euler XYZ multiplied on the left of the raw source quaternion."""
    dimensions = {'translation': 3, 'quaternion': 4, 'rotation': 3, 'scale': 3, 'auxiliary': 3}
    for key, width in dimensions.items():
        if len(source[key]) != width or not all(math.isfinite(v) for v in source[key]):
            raise ValueError('Invalid pose transform')
    sx, sy, sz = [math.sin(angle / 2) for angle in source['rotation']]
    cx, cy, cz = [math.cos(angle / 2) for angle in source['rotation']]
    a, b, c, d = (sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
                  cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz)
    e, f, g, h = source['quaternion']
    return (d*e+a*h+b*g-c*f, d*f+b*h+c*e-a*g,
            d*g+c*h+a*f-b*e, d*h-a*e-b*f-c*g)


def raw_transform_matrix(source):
    """Native local matrix with the raw composed quaternion, without normalization."""
    x, y, z, w = composed_quaternion(source)
    rotation = ((1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)),
                (2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)),
                (2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)))
    pivot, scale = source['auxiliary'], source['scale']
    t = [source['translation'][i]+pivot[i]-sum(rotation[i][j]*pivot[j]*scale[j] for j in range(3)) for i in range(3)]
    result = [rotation[r][c]*scale[c] if r<3 and c<3 else
              t[r] if r<3 and c==3 else float(r==c) for c in range(4) for r in range(4)]
    if not all(math.isfinite(v) and abs(v)<=3.4028234663852886e38 for v in result):
        raise ValueError('Pose matrix exceeds finite float storage')
    return result


def sample_pose(model, motion, coordinate):
    """One unblended clip applied to copied outfit nodes in source coordinates.

    Missing targets follow verified native binding behavior and are recorded.
    Ambiguous names, duplicate destinations and unsupported modes are refused.
    This does not interpret elapsed seconds, looping or other active clips.
    """
    if not math.isfinite(coordinate):
        raise ValueError('Sampling coordinate must be finite')
    try:
        coordinate = struct.unpack('<f', struct.pack('<f', coordinate))[0]
    except (OverflowError, struct.error) as error:
        raise ValueError('Sampling coordinate exceeds float32') from error
    if not math.isfinite(coordinate):
        raise ValueError('Sampling coordinate exceeds float32')
    tree = model.get('hierarchy')
    if not tree or not tree['nodes'] or len(tree['nodes'])>10000:
        raise ValueError('Pose requires a bounded model hierarchy')
    names = {node['name']: i for i, node in enumerate(tree['nodes'])}
    if len(names)!=len(tree['nodes']):
        raise ValueError('Pose requires unique hierarchy names')
    nodes = [dict(source, **{key: list(source[key]) for key in
             ('translation', 'quaternion', 'rotation', 'scale', 'auxiliary')}) for source in tree['nodes']]
    assigned, bound, missing = set(), [], []
    if len(motion['records'])>10000:
        raise ValueError('Too many animation records')
    for record in motion['records']:
        channel = LinearChannel(record)
        index = names.get(record['target'])
        if index is None:
            missing.append(dict(name=record['name'], target=record['target'], raw_kind=channel.kind))
            continue
        if nodes[index]['kind'] not in (0, 1):
            raise ValueError('Animation target is not a verified DAG transform kind')
        key,component = channel_destination(channel.kind)
        components=range(len(nodes[index][key])) if component is None else (component,)
        destinations={(index,key,c) for c in components}
        if destinations & assigned:
            raise ValueError('Multiple animation records write the same pose field')
        assigned.update(destinations)
        if component is None:nodes[index][key] = channel.sample(coordinate)
        else:nodes[index][key][component] = channel.sample(coordinate)[0]
        binding=dict(name=record['name'], target=record['target'], node=index, field=key)
        if component is not None:binding['component']=component
        bound.append(binding)
    local = [raw_transform_matrix(node) for node in nodes]
    identity = [float(r==c) for c in range(4) for r in range(4)]
    world, pending = {}, [(i,identity) for i in tree['roots']]
    while pending:
        index, parent = pending.pop()
        if not isinstance(index,int) or not 0<=index<len(nodes) or index in world:
            raise ValueError('Invalid, repeated or cyclic pose hierarchy')
        world[index] = matrix_product(parent,local[index])
        if not all(math.isfinite(v) and abs(v)<=3.4028234663852886e38 for v in world[index]):
            raise ValueError('Pose world matrix exceeds finite float storage')
        pending.extend((child,world[index]) for child in nodes[index]['children'])
    if len(world)!=len(nodes):
        raise ValueError('Unreachable pose hierarchy node')
    return dict(format='fdkss.mdl-sampled-pose',format_version=1,
                raw_coordinate=coordinate,motion_sha256=motion['source_sha256'],
                roots=list(tree['roots']),bound_records=bound,missing_targets=missing,
                nodes=[dict(name=node['name'],kind=node['kind'],children=list(node['children']),
                            translation=node['translation'],quaternion=node['quaternion'],
                            rotation=node['rotation'],scale=node['scale'],pivot=node['auxiliary'],
                            local_matrix=local[i],world_matrix=world[i]) for i,node in enumerate(nodes)],
                matrix_order='column-major',
                limitations=['One clip in raw source coordinates; clock, looping and blending are not applied.',
                             'Raw quaternion matrices are preserved; this is not standard glTF animation conversion.',
                             'Python math approximates native float arithmetic; numerical error is verified separately.'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model',type=Path);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--outfit',type=Path,help='Sample motion against this outfit hierarchy')
    parser.add_argument('--coordinate',type=float,help='Raw source coordinate for a sampled pose')
    args=parser.parse_args()
    if (args.outfit is None)!=(args.coordinate is None):
        parser.error('--outfit and --coordinate must be provided together')
    sources=[args.model]+([args.outfit] if args.outfit is not None else [])
    if any(args.output.resolve()==source.resolve() or args.output.exists() and args.output.samefile(source) for source in sources):
        parser.error('Inspection output cannot replace the source')
    result=inspect(args.model.read_bytes())
    if args.outfit is not None:
        outfit=args.outfit.read_bytes()
        result=sample_pose(parse(outfit),result,args.coordinate)
        result['model_sha256']=hashlib.sha256(outfit).hexdigest()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,separators=(',',':'),allow_nan=False),encoding='utf-8')
    if args.outfit is not None:
        print(f'Sampled {len(result["nodes"])} nodes, {len(result["bound_records"])} bound records, {len(result["missing_targets"])} missing targets')
    else:
        print(f'Inspected {len(result["records"])} records, {sum(len(r["keys"]) for r in result["records"])} keys')


if __name__=='__main__':main()
