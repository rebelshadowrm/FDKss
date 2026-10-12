"""Bake one verified bounded MDL clip into standard glTF with raw-matrix factors.

Each source transform becomes three TRS nodes. This preserves the native raw
quaternion matrix at samples, including its nonunit scale/shear, without silently
normalizing the source. Between samples, adaptive baking checks matrix error at
quarter, midpoint and three-quarter coordinates. This is a sampled error check,
not a proof of a continuous global bound or native multi-clip playback.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct

from mdl import gltf, parse, matrix_product
from mdl_animation import LinearChannel, channel_destination, composed_quaternion, inspect, raw_transform_matrix


def multiply(a,b):
    x,y,z,w=a; e,f,g,h=b
    return [w*e+x*h+y*g-z*f,w*f+y*h+z*e-x*g,w*g+z*h+x*f-y*e,w*h-x*e-y*f-z*g]


def normalize(q):
    length=math.sqrt(sum(v*v for v in q))
    if not math.isfinite(length) or length<1e-12:
        raise ValueError('Degenerate factor quaternion')
    return [v/length for v in q]


def factors(source):
    """T(t+p) R(axis,angle) A S(k,k,1) A^-1 S(source) T(-p).

    A rotates Z to the raw quaternion's axis. The raw matrix fixes that axis;
    in its perpendicular plane it acts as a uniform stretch and rotation.
    """
    q=composed_quaternion(source)
    # q and -q produce the same matrix. Canonicalize to reduce factor jumps.
    if q[3]<0:q=tuple(-v for v in q)
    x,y,z,w=q;squared_length=x*x+y*y+z*z;length=math.sqrt(squared_length)
    if length<1e-12:
        align=[0,0,0,1];rotation=align;stretch=1
    else:
        axis=[x/length,y/length,z/length]
        if axis[2]<-1+1e-12:
            align=[1,0,0,0]
        else:
            align=normalize([-axis[1],axis[0],0,1+axis[2]])
        real,imaginary=1-2*squared_length,2*w*length
        stretch=math.hypot(real,imaginary)
        # A zero planar stretch is a valid rank-one matrix. Its planar angle
        # is immaterial; choose zero and let adaptive baking check neighbors.
        half=math.atan2(imaginary,real)/2
        rotation=[*(v*math.sin(half) for v in axis),math.cos(half)]
    pivot=source['auxiliary']
    return [dict(translation=[a+b for a,b in zip(source['translation'],pivot)],
                 rotation=normalize(multiply(rotation,align)),scale=[stretch,stretch,1]),
            dict(translation=[0,0,0],rotation=[-align[0],-align[1],-align[2],align[3]],scale=list(source['scale'])),
            dict(translation=[-v for v in pivot],rotation=[0,0,0,1],scale=[1,1,1])]


def trs_matrix(trs):
    x,y,z,w=trs['rotation'];a,b,c=trs['scale'];t=trs['translation']
    return [(1-2*(y*y+z*z))*a,2*(x*y+z*w)*a,2*(x*z-y*w)*a,0,
            2*(x*y-z*w)*b,(1-2*(x*x+z*z))*b,2*(y*z+x*w)*b,0,
            2*(x*z+y*w)*c,2*(y*z-x*w)*c,(1-2*(x*x+y*y))*c,0,*t,1]


def factor_matrix(rows):
    return matrix_product(matrix_product(trs_matrix(rows[0]),trs_matrix(rows[1])),trs_matrix(rows[2]))


def interpolate(a,b,fraction):
    result={key:[x+(y-x)*fraction for x,y in zip(a[key],b[key])] for key in ('translation','scale')}
    qa,qb=normalize(a['rotation']),normalize(b['rotation']);dot=sum(x*y for x,y in zip(qa,qb))
    if dot<0:qb=[-v for v in qb];dot=-dot
    dot=min(1,dot)
    if dot>1-1e-12:
        q=normalize([x+(y-x)*fraction for x,y in zip(qa,qb)])
    else:
        angle=math.acos(dot);divisor=math.sin(angle)
        wa,wb=math.sin((1-fraction)*angle)/divisor,math.sin(fraction*angle)/divisor
        q=[wa*x+wb*y for x,y in zip(qa,qb)]
    result['rotation']=q
    return result


def f32(value):
    try:value=struct.unpack('<f',struct.pack('<f',value))[0]
    except (OverflowError,struct.error) as error:raise ValueError('Bake value exceeds float32') from error
    if not math.isfinite(value):raise ValueError('Nonfinite bake value')
    return value


def bake(model,motion,*,max_error=1e-5,max_samples=4096,seconds_per_unit=1.0,source_range=None,vertex_colors=False):
    if not math.isfinite(max_error) or not 1e-8<=max_error<=.01:
        raise ValueError('Matrix error tolerance must be in [1e-8,.01]')
    if not isinstance(max_samples,int) or not 2<=max_samples<=4096:
        raise ValueError('Bake sample limit must be in [2,4096]')
    if not math.isfinite(seconds_per_unit) or seconds_per_unit<=0:
        raise ValueError('Seconds per source unit must be finite and positive')
    tree=model.get('hierarchy')
    if not tree or len(tree['nodes'])>10000:raise ValueError('Bake requires a bounded hierarchy')
    names={node['name']:i for i,node in enumerate(tree['nodes'])}
    if len(names)!=len(tree['nodes']):raise ValueError('Bake requires unique node names')
    bindings={};missing=[];times=set();occupied=set()
    if len(motion['records'])>10000:raise ValueError('Too many animation records')
    for record in motion['records']:
        channel=LinearChannel(record);index=names.get(record['target'])
        if index is None:
            missing.append(dict(name=record['name'],target=record['target'],raw_kind=channel.kind));continue
        if channel.curve_component_jumps:
            raise ValueError('Vector mode2 component jumps cannot share continuous factor interpolation')
        if tree['nodes'][index]['kind'] not in (0,1):raise ValueError('Unsupported animation target node')
        key,component=channel_destination(channel.kind)
        components=range(len(tree['nodes'][index][key])) if component is None else (component,)
        destinations={(index,key,c) for c in components}
        if destinations & occupied:raise ValueError('Duplicate animation destination')
        occupied.update(destinations)
        bindings[index,key,component]=channel;times.update(channel.times)
    if not bindings:raise ValueError('No motion tracks bind to this outfit')
    if source_range is None and any(mode not in (0,2) for channel in bindings.values() for mode in channel.pre_post):
        raise ValueError('Extending pre/post modes require an explicit source range')
    if source_range is not None:
        if not isinstance(source_range,(tuple,list)) or len(source_range)!=2 or not all(math.isfinite(v) for v in source_range):
            raise ValueError('Source range requires two finite coordinates')
        range_start,range_end=map(f32,source_range)
        if range_end<=range_start:raise ValueError('Source range must increase in float32')
        times={range_start,range_end}|{t for t in times if range_start<t<range_end}
        seed_work=0
        for channel in bindings.values():
            first,last=channel.times[0],channel.times[-1]
            if first==last:continue
            for pre,active,mode in ((True,range_start<first,channel.pre_post[0]),(False,range_end>last,channel.pre_post[1])):
                if not active:continue
                if channel.discontinuous and mode not in (0,2):
                    raise ValueError('Extended pre/post on stepwise channels is not verified for factor export')
                if mode not in (3,4,5):continue
                period=f32(last-first)
                if period!=last-first:raise ValueError('Pre/post event period loses source precision')
                if mode==3 and channel.values[0]!=channel.values[-1]:
                    raise ValueError('Repeated endpoint jumps cannot share continuous factor interpolation')
                if mode==4 and channel.kind==10 and channel.values[0]!=channel.values[-1]:
                    raise ValueError('Quaternion loop-offset return paths may introduce endpoint jumps')
                stride=2*period if mode==5 else period
                low=math.floor((range_start-last)/stride)-1;high=math.ceil((range_end-first)/stride)+1
                if high-low>max_samples:raise ValueError('Pre/post cycles exceed bake sample limit')
                seed_work+=(high-low+1)*len(channel.times)*(2 if mode==5 else 1)
                if seed_work>1000000:raise ValueError('Pre/post event search exceeds one million candidates')
                for cycle in range(low,high+1):
                    for key in channel.times:
                        phases=(key,2*first-key) if mode==5 else (key,)
                        for phase in phases:
                            coordinate=f32(phase+cycle*stride)
                            if range_start<coordinate<range_end and (coordinate<first if pre else coordinate>last):
                                times.add(coordinate)
                                if len(times)>max_samples:raise ValueError('Pre/post events exceed bake sample limit')
                if len(times)>max_samples:raise ValueError('Pre/post events exceed bake sample limit')
    animated=sorted({entry[0] for entry in bindings})
    stepped_fields=set();stepped_components=set();mixed_fields=set()
    for index in animated:
        for field in ('translation','quaternion','scale'):
            channels=[channel for entry,channel in bindings.items() if entry[:2]==(index,field)]
            if any(channel.discontinuous for channel in channels):
                if any(channel.discontinuous and not channel.step_only for channel in channels):
                    raise ValueError('Discontinuous and continuous segments in one channel cannot share factor interpolation')
                if not all(channel.step_only for channel in channels):
                    if field=='quaternion':
                        raise ValueError('Discontinuous and continuous quaternion channels in one field cannot share factor interpolation')
                    mixed_fields.add((index,field))
                else:stepped_fields.add((index,field))
                for entry,channel in bindings.items():
                    if entry[:2]==(index,field) and channel.step_only:
                        components=range(len(tree['nodes'][index][field])) if entry[2] is None else (entry[2],)
                        stepped_components.update((index,field,c) for c in components)
    stepped={index for index,field,component in stepped_components}
    def factor_field(factor_index,key):
        if key=='rotation' or key=='scale' and factor_index==0:return 'quaternion'
        return key
    times=sorted(times)
    if len(times)>max_samples:raise ValueError('Source keys exceed bake sample limit')
    # glTF interpolation uses relative seconds; preserve source start in extras.
    start=times[0]
    if len(times)==1:times.append(f32(start+1/seconds_per_unit))
    if times[-1]<=start:raise ValueError('Invalid bake coordinate range')
    source_end=times[-1]
    # Bake in the actual glTF float32 time domain. Subtracting a source start
    # can collapse adjacent coordinates after rounding; source-domain baking
    # followed by a final time conversion would miss that representation error.
    times=sorted({f32((t-start)*seconds_per_unit) for t in times})
    if len(times)<2 or times[0]!=0:raise ValueError('Bake time range collapses in float32')
    def coordinate_at(seconds):return f32(start+seconds/seconds_per_unit)
    # STEP jumps follow the native float32 endpoint test, which can switch
    # before the recorded key. Search the actual float32 seconds domain too.
    events=set()
    for entry,channel in bindings.items():
        if not channel.step_only or not channel.discontinuous:continue
        for transition in channel.step_transitions():
            if transition<=start or transition>source_end:continue
            low=0;high=struct.unpack('<I',struct.pack('<f',times[-1]))[0]
            if coordinate_at(times[-1])<transition:
                raise ValueError('Step transition lies outside representable bake seconds')
            while low+1<high:
                middle=(low+high)//2;seconds=struct.unpack('<f',struct.pack('<I',middle))[0]
                if coordinate_at(seconds)>=transition:high=middle
                else:low=middle
            seconds=struct.unpack('<f',struct.pack('<I',high))[0]
            if coordinate_at(seconds)<transition:
                raise ValueError('Step transition cannot be represented in bake seconds')
            if seconds!=(transition-start)*seconds_per_unit or coordinate_at(seconds)!=transition:
                raise ValueError('Step transition time loses precision after source offset/scaling')
            events.add(seconds)
    times=sorted(set(times)|events)
    if len(times)>max_samples:raise ValueError('Step events exceed bake sample limit')
    storage_units=sum(1+3*sum((i,field) in mixed_fields for field in ('translation','scale')) for i in animated)
    if storage_units*max_samples>1000000:raise ValueError('Bake node/sample budget exceeds one million')
    def source_at(index,coordinate):
        source=dict(tree['nodes'][index])
        for key in ('translation','quaternion','scale'):
            channel=bindings.get((index,key,None))
            if channel:source[key]=channel.sample(coordinate)
            else:
                source[key]=list(source[key])
                for component in range(len(source[key])):
                    channel=bindings.get((index,key,component))
                    if channel:source[key][component]=channel.sample(coordinate)[0]
        return source
    sample_error=0
    def rows_at(seconds):
        nonlocal sample_error
        coordinate=coordinate_at(seconds)
        result={}
        for i in animated:
            # Store actual float32 outputs before checking interpolation error.
            result[i]=[{k:[f32(v) for v in row[k]] for k in row} for row in factors(source_at(i,coordinate))]
            error=max(abs(a-b) for a,b in zip(factor_matrix(result[i]),raw_transform_matrix(source_at(i,coordinate))))
            if error>max_error:raise ValueError('Stored factor sample exceeds requested matrix error')
            sample_error=max(sample_error,error)
        return result
    samples={t:rows_at(t) for t in times};checked=0;worst=0
    pending=list(zip(times,times[1:]))
    while pending:
        left,right=pending.pop();error=0
        for fraction in (.25,.5,.75):
            probe=f32(left+(right-left)*fraction)
            coordinate=coordinate_at(probe)
            actual_fraction=(probe-left)/(right-left)
            for i in animated:
                candidate=[interpolate(a,b,actual_fraction) for a,b in zip(samples[left][i],samples[right][i])]
                endpoint=samples[right if probe>=right else left][i]
                for factor_index,row in enumerate(candidate):
                    for key in row:
                        if (i,factor_field(factor_index,key)) in stepped_fields:
                            row[key]=endpoint[factor_index][key]
                        elif (i,factor_field(factor_index,key)) in mixed_fields:
                            for component in range(3):
                                if (i,factor_field(factor_index,key),component) in stepped_components:
                                    row[key][component]=endpoint[factor_index][key][component]
                expected=raw_transform_matrix(source_at(i,coordinate))
                error=max(error,max(abs(a-b) for a,b in zip(factor_matrix(candidate),expected)))
                checked+=1
        if error<=max_error:
            worst=max(worst,error);continue
        middle=f32((left+right)/2)
        if middle<=left or middle>=right or len(samples)>=max_samples:
            raise ValueError(f'Bake cannot meet matrix error tolerance within {max_samples} samples')
        samples[middle]=rows_at(middle);pending.extend([(left,middle),(middle,right)])
    times=sorted(samples)
    seconds=times
    # Construct and validate bind geometry first, retaining inverse binds and
    # skin-joint indexes. Original joint indexes become the factor end nodes.
    output=gltf(model,skinning=True,vertex_colors=vertex_colors)
    nodes=output['nodes'];count=len(tree['nodes'])
    detached={i for i,node in enumerate(nodes) if 'skin' in node}
    heads={};plans={};next_node=count
    for i in range(count):
        if i in detached:continue
        plan=[]
        if (i,'translation') in mixed_fields:plan.extend(('translation',c) for c in range(3))
        plan.extend((0,1))
        if (i,'scale') in mixed_fields:plan.extend(('scale',c) for c in range(3))
        heads[i]=next_node;plans[i]=plan;next_node+=len(plan)
    def expanded_rows(i,rows):
        result=[]
        for part in plans[i]:
            if isinstance(part,int):
                row={key:list(value) for key,value in rows[part].items()}
                if part==0 and (i,'translation') in mixed_fields:row['translation']=[0,0,0]
                if part==1 and (i,'scale') in mixed_fields:row['scale']=[1,1,1]
            else:
                field,component=part
                row=dict(translation=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])
                row[field][component]=rows[0 if field=='translation' else 1][field][component]
            result.append(row)
        return result
    if any(i in detached for i in animated):raise ValueError('Animated skinned mesh placement is not verified')
    original_children=[list(node.get('children',[])) for node in nodes]
    for i,source in enumerate(tree['nodes']):
        rows=factors(source_at(i,start))
        rows=[{k:[f32(v) for v in row[k]] for k in row} for row in rows]
        for key in ('translation','rotation','scale','matrix'):nodes[i].pop(key,None)
        if i in detached:
            # The skin exporter already moved identity mesh nodes to scene roots.
            continue
        nodes[i].update(rows[2]);nodes[i]['children']=[heads[c] for c in original_children[i]]
        if not nodes[i]['children']:del nodes[i]['children']
        for offset,(part,row) in enumerate(zip(plans[i],expanded_rows(i,rows))):
            suffix=('__fdk_raw_head' if part==0 else '__fdk_raw_mid') if isinstance(part,int) else f'__fdk_{part[0]}_axis_{part[1]}'
            child=heads[i]+offset+1 if offset+1<len(plans[i]) else i
            nodes.append(dict(row,name=source['name']+suffix,children=[child]))
    output['scenes'][0]['nodes']=[i if i in detached else heads[i] for i in output['scenes'][0]['nodes']]
    binary=bytearray(base64.b64decode(output['buffers'][0]['uri'].split(',',1)[1]))
    def accessor(rows,width,kind):
        while len(binary)%4:binary.append(0)
        offset=len(binary)
        for row in rows:binary.extend(struct.pack('<'+'f'*width,*row))
        view=len(output['bufferViews']);output['bufferViews'].append(dict(buffer=0,byteOffset=offset,byteLength=len(binary)-offset))
        index=len(output['accessors']);entry=dict(bufferView=view,componentType=5126,count=len(rows),type=kind)
        if kind=='SCALAR':entry.update(min=[rows[0][0]],max=[rows[-1][0]])
        output['accessors'].append(entry);return index
    time_accessor=accessor([[v] for v in seconds],1,'SCALAR')
    animation=dict(name='MDL baked motion',samplers=[],channels=[])
    for i in animated:
        expanded=[expanded_rows(i,samples[t][i]) for t in times]
        for factor_index,part in enumerate(plans[i]):
            for key,width,kind in [('translation',3,'VEC3'),('rotation',4,'VEC4'),('scale',3,'VEC3')]:
                values=[rows[factor_index][key] for rows in expanded]
                if all(v==values[0] for v in values[1:]):continue
                if key=='rotation':
                    for n in range(1,len(values)):
                        if sum(a*b for a,b in zip(values[n-1],values[n]))<0:values[n]=[-v for v in values[n]]
                value_accessor=accessor(values,width,kind);index=len(animation['samplers'])
                is_step=((i,factor_field(part,key)) in stepped_fields if isinstance(part,int) else (i,*part) in stepped_components)
                mode='STEP' if is_step else 'LINEAR'
                animation['samplers'].append(dict(input=time_accessor,output=value_accessor,interpolation=mode))
                animation['channels'].append(dict(sampler=index,target=dict(node=heads[i]+factor_index,path=key)))
    if not animation['channels']:raise ValueError('Motion has no changing channels')
    output['animations']=[animation]
    output['buffers'][0]=dict(byteLength=len(binary),uri='data:application/octet-stream;base64,'+base64.b64encode(binary).decode())
    output['extras'].update(scope='One unblended clip with bounded continuous curves and per-field STEP interpolation baked through raw quaternion matrix factors; shader reconstruction is incomplete.',
                           fdk_motion_sha256=motion['source_sha256'],fdk_bake=dict(source_start=start,source_end=source_end,
                           seconds_per_source_unit=seconds_per_unit,samples=len(times),max_checked_matrix_error=worst,
                           max_stored_sample_matrix_error=sample_error,
                           requested_matrix_error=max_error,checked_node_coordinates=checked,missing_targets=missing,
                           interpolation_modes=sorted({mode for channel in bindings.values() for mode in channel.modes}),
                           step_nodes=sorted(stepped),step_event_seconds=sorted(events),
                           step_fields=[dict(node=i,field=field) for i,field in sorted(stepped_fields)],
                           step_components=[dict(node=i,field=field,component=c) for i,field,c in sorted(stepped_components)],
                           split_axis_fields=[dict(node=i,field=field) for i,field in sorted(mixed_fields)],
                           explicit_source_range=source_range is not None,
                           pre_post_modes=sorted({mode for channel in bindings.values() for mode in channel.pre_post}),
                           limitations=['Error checked at quarter, midpoint and three-quarter samples; no continuous global bound.',
                                        'STEP events preserve representable float32 source jumps; real-valued sub-ULP playback times are not a native clock proof.',
                                        'Source coordinate units, looping and multi-clip blending are not inferred.']))
    return output


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('outfit',type=Path);parser.add_argument('motion',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--max-matrix-error',type=float,default=1e-5)
    parser.add_argument('--max-samples',type=int,default=4096)
    parser.add_argument('--seconds-per-source-unit',type=float,default=1)
    parser.add_argument('--source-start',type=float);parser.add_argument('--source-end',type=float)
    parser.add_argument('--vertex-colors',action='store_true',help='Preserve packed MDL v4 vertex colors with approximate shader behavior')
    args=parser.parse_args()
    if (args.source_start is None)!=(args.source_end is None):parser.error('Source start/end must be provided together')
    if any(args.output.resolve()==p.resolve() or args.output.exists() and args.output.samefile(p) for p in (args.outfit,args.motion)):
        parser.error('Output cannot replace a source')
    data=args.outfit.read_bytes()
    source_range=None if args.source_start is None else (args.source_start,args.source_end)
    output=bake(parse(data),inspect(args.motion.read_bytes()),max_error=args.max_matrix_error,max_samples=args.max_samples,seconds_per_unit=args.seconds_per_source_unit,source_range=source_range,vertex_colors=args.vertex_colors)
    output['extras']['fdk_model_sha256']=hashlib.sha256(data).hexdigest()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(output,separators=(',',':'),allow_nan=False),encoding='utf-8')
    summary={k:v for k,v in output['extras']['fdk_bake'].items() if k not in ('missing_targets','limitations')}
    summary['missing_targets']=len(output['extras']['fdk_bake']['missing_targets'])
    print(summary)


if __name__=='__main__':main()
