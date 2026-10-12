import base64,copy,json,math,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from mdl_animation_gltf import bake,factors,factor_matrix,interpolate,trs_matrix
from mdl_animation import raw_transform_matrix
import mdl_tests
from mdl import inspection

def record(kind=9,values=((0,1,0),(0,3,0))):
    return dict(name='Child track',target='Child',raw_kind=kind,raw_words=[0,0],
                keys=[dict(raw_coordinate=t,raw_values=list(v),raw_extra_hex='00'*20) for t,v in zip((2,4),values)])

def accessor(output,index):
    entry=output['accessors'][index];view=output['bufferViews'][entry['bufferView']]
    binary=base64.b64decode(output['buffers'][0]['uri'].split(',',1)[1])
    width={'SCALAR':1,'VEC3':3,'VEC4':4}[entry['type']]
    return list(struct.iter_unpack('<'+'f'*width,binary[view['byteOffset']:view['byteOffset']+view['byteLength']]))

class AnimationGltfTests(unittest.TestCase):
    def test_vertex_colors_survive_animation_buffer_extension(self):
        model=mdl_tests.MdlTests().skinned_model();motion=dict(source_sha256='fixture',records=[record()])
        source=bytes((10,20,30,40))*3
        primitive=model['meshes'][0]['submeshes'][0];primitive['unknown']=6
        primitive['streams'].append(dict(kind=3,stride=4,count=3,data=source))
        output=bake(model,motion,vertex_colors=True)
        index=output['meshes'][0]['primitives'][0]['attributes']['COLOR_0']
        entry=output['accessors'][index];view=output['bufferViews'][entry['bufferView']]
        binary=base64.b64decode(output['buffers'][0]['uri'].split(',',1)[1])
        self.assertEqual(binary[view['byteOffset']:view['byteOffset']+view['byteLength']],source)
        channel=output['animations'][0]['channels'][0];sampler=output['animations'][0]['samplers'][channel['sampler']]
        self.assertEqual(accessor(output,sampler['output']),[(0,1,0),(0,3,0)])
        self.assertNotIn('COLOR_0',bake(model,motion)['meshes'][0]['primitives'][0]['attributes'])

    def test_component_bake_preserves_other_fields_and_refuses_overlap(self):
        model=mdl_tests.MdlTests().skinned_model()
        row=dict(record(kind=0,values=((0,),(2,))),name='Child x')
        motion=dict(source_sha256='fixture',records=[row]);before=(inspection(model),copy.deepcopy(motion))
        output=bake(model,motion)
        self.assertEqual((inspection(model),motion),before)
        c=next(c for c in output['animations'][0]['channels'] if c['target']['path']=='translation')
        sampler=output['animations'][0]['samplers'][c['sampler']]
        values=accessor(output,sampler['output'])
        self.assertEqual(values[0][0],0);self.assertEqual(values[-1][0],2)
        child=model['hierarchy']['nodes'][1]
        expected=[a+b for a,b in zip(child['translation'][1:],child['auxiliary'][1:])]
        self.assertTrue(all(list(value[1:])==expected for value in values))
        for records in ([row,row],[row,record()],[record(),row]):
            with self.assertRaisesRegex(ValueError,'Duplicate'):bake(model,dict(motion,records=records))
    def test_raw_native_fixture_factors_and_degenerate_axes(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_raw_pose.json').read_text())
        for case in fixtures['cases']:
            matrix=factor_matrix(factors(case['source']))
            self.assertLess(max(abs(a-b) for a,b in zip(matrix,case['matrix'])),3e-5)
            self.assertLess(max(abs(a-b) for a,b in zip(matrix,raw_transform_matrix(case['source']))),1e-10)
        node=fixtures['cases'][0]['source']
        for quaternion in ((0,0,0,1),(0,0,-1,0),(0,0,0,0),(0,0,1,-.1)):
            source=dict(node,quaternion=quaternion,rotation=[0,0,0])
            self.assertLess(max(abs(a-b) for a,b in zip(factor_matrix(factors(source)),raw_transform_matrix(source))),1e-10)
        singular=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_singular.json').read_text())
        for case in singular['cases']:
            actual=factor_matrix(factors(case['source']))
            for expected in case['original_matrices'].values():
                self.assertLess(max(abs(a-b) for a,b in zip(actual,expected)),2e-6)
        collapsed=factors(dict(node,quaternion=[.5,.5,0,0],rotation=[0,0,0]))
        self.assertEqual(collapsed[0]['scale'][:2],[0,0])
        self.assertTrue(all(math.isfinite(v) for row in collapsed for values in row.values() for v in values))
    def test_baking_crosses_rank_one_raw_rotation_with_copied_skin(self):
        model=mdl_tests.MdlTests().skinned_model()
        model['hierarchy']['nodes'][1]['quaternion']=[0,0,1,0]
        model['meshes'][0]['skin_bindings'][1]['matrix']=[-1,0,0,0,0,-1,0,0,0,0,1,0,0,1,0,1]
        records=[]
        for kind,values in ((3,(.5,.5)),(4,(.49,.51)),(5,(0,0)),(0,(0,.1))):
            row=record(kind,tuple((v,) for v in values))
            for key,time in zip(row['keys'],(0,1)):key['raw_coordinate']=time
            records.append(row)
        motion=dict(source_sha256='singular-fixture',records=records)
        before=(inspection(model),copy.deepcopy(motion));output=bake(model,motion)
        self.assertEqual((inspection(model),motion),before)
        self.assertLessEqual(output['extras']['fdk_bake']['max_checked_matrix_error'],1e-5)
        animation=output['animations'][0]
        scales=[accessor(output,animation['samplers'][c['sampler']]['output'])
                for c in animation['channels'] if c['target']['path']=='scale']
        self.assertTrue(any(row[0]==0 and row[1]==0 for values in scales for row in values))
        with self.assertRaises(ValueError):bake(model,motion,max_samples=2)
    def test_baked_translation_hierarchy_skin_and_copied_source(self):
        model=mdl_tests.MdlTests().skinned_model();motion=dict(source_sha256='fixture',records=[record()])
        before=(inspection(model),copy.deepcopy(motion))
        output=bake(model,motion,seconds_per_unit=.5)
        self.assertEqual((inspection(model),motion),before)
        self.assertEqual(output['skins'][0]['joints'],[0,1])
        self.assertEqual(output['nodes'][2]['skin'],0)
        self.assertIn(2,output['scenes'][0]['nodes'])
        self.assertEqual(len(output['nodes']),7) # two factors for each joint, no orphan mesh helpers
        animation=output['animations'][0]
        self.assertEqual(len(animation['channels']),1)
        channel=animation['channels'][0];sampler=animation['samplers'][channel['sampler']]
        self.assertEqual(accessor(output,sampler['input']),[(0,),(1,)])
        self.assertEqual(accessor(output,sampler['output']),[(0,1,0),(0,3,0)])
        self.assertEqual(channel['target']['path'],'translation')
        self.assertEqual(output['extras']['fdk_bake']['source_start'],2)
        visited=set();pending=output['scenes'][0]['nodes'].copy()
        while pending:
            i=pending.pop();self.assertNotIn(i,visited);visited.add(i);pending.extend(output['nodes'][i].get('children',[]))
        self.assertEqual(visited,set(range(len(output['nodes']))))
    def test_nonunit_interpolation_is_baked_without_normalizing_raw_source(self):
        model=mdl_tests.MdlTests().skinned_model()
        translation=record(values=((0,1,0),(0,1,0)))
        translation['keys'].insert(1,dict(translation['keys'][0],raw_coordinate=3))
        motion=dict(source_sha256='fixture',records=[record(10,((0,0,0,1),(0,0,.04,math.sqrt(.9984)))),translation])
        output=bake(model,motion)
        channels=output['animations'][0]['channels']
        self.assertIn('scale',{c['target']['path'] for c in channels})
        self.assertLessEqual(output['extras']['fdk_bake']['max_checked_matrix_error'],1e-5)
        # Native near-angle interpolation carries a small planar stretch. Its
        # preservation requires a scale channel, even though source scale is 1.
        scales=[accessor(output,output['animations'][0]['samplers'][c['sampler']]['output']) for c in channels if c['target']['path']=='scale']
        self.assertTrue(any(any(row[0]<1 for row in rows) for rows in scales))
    def test_easing_and_cubic_exports_follow_frozen_native_samples(self):
        model=mdl_tests.MdlTests().skinned_model()
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_easing.json').read_text())
        cubic=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_cubic.json').read_text())
        for mode in (3,4,5,6):
            case=next(case for case in (cubic if mode==3 else fixtures)['cases']
                      if case['record']['name']==('synthetic-0-cubic' if mode==3 else f'synthetic-0-mode{mode}'))
            track=dict(case['record'],target='Child')
            output=bake(model,dict(source_sha256='fixture',records=[track]))
            channels=output['animations'][0]['channels']
            channel=next(c for c in channels if c['target']['path']=='translation')
            sampler=output['animations'][0]['samplers'][channel['sampler']]
            times=[row[0] for row in accessor(output,sampler['input'])]
            values=accessor(output,sampler['output'])
            self.assertIn(mode,output['extras']['fdk_bake']['interpolation_modes'])
            start=output['extras']['fdk_bake']['source_start']
            import bisect
            for sample in case['samples']:
                seconds=sample['coordinate']-start
                right=bisect.bisect_left(times,seconds)
                if right==0:actual=values[0]
                elif right==len(times):actual=values[-1]
                else:
                    fraction=(seconds-times[right-1])/(times[right]-times[right-1])
                    actual=[a+(b-a)*fraction for a,b in zip(values[right-1],values[right])]
                for a,b in zip(actual,sample['result']):self.assertLess(abs(a-b),2e-5)
    def test_scalar_curve_export_follows_frozen_native_samples(self):
        import bisect
        model=mdl_tests.MdlTests().skinned_model()
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_scalar_curves.json').read_text())
        # Negative control distances exercise the native lower ratio clamp.
        case=next(case for case in fixtures['cases'] if case['record']['name']=='scalar-curve-kind0-variant5')
        track=dict(case['record'],target='Child')
        output=bake(model,dict(source_sha256='fixture',records=[track]))
        animation=output['animations'][0]
        channel=next(c for c in animation['channels'] if c['target']['path']=='translation')
        sampler=animation['samplers'][channel['sampler']]
        times=[row[0] for row in accessor(output,sampler['input'])];values=accessor(output,sampler['output'])
        start=output['extras']['fdk_bake']['source_start']
        self.assertIn(2,output['extras']['fdk_bake']['interpolation_modes'])
        for sample in case['samples']:
            seconds=sample['coordinate']-start;right=bisect.bisect_left(times,seconds)
            if right==0:actual=values[0][0]
            elif right==len(times):actual=values[-1][0]
            else:
                fraction=(seconds-times[right-1])/(times[right]-times[right-1])
                actual=values[right-1][0]+(values[right][0]-values[right-1][0])*fraction
            self.assertLess(abs(actual-sample['result'][0]),2e-5)
    def test_step_export_uses_frozen_endpoint_values_and_preserves_source(self):
        import bisect
        model=mdl_tests.MdlTests().skinned_model()
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_steps.json').read_text())
        case=next(case for case in fixtures['cases'] if case['record']['name']=='step-export-kind9')
        track=dict(case['record'],target='Child');motion=dict(source_sha256='fixture',records=[track])
        before=(inspection(model),copy.deepcopy(motion));output=bake(model,motion)
        self.assertEqual((inspection(model),motion),before)
        animation=output['animations'][0]
        channel=next(c for c in animation['channels'] if c['target']['path']=='translation')
        sampler=animation['samplers'][channel['sampler']]
        self.assertEqual(sampler['interpolation'],'STEP')
        self.assertEqual(output['extras']['fdk_bake']['step_nodes'],[1])
        times=[row[0] for row in accessor(output,sampler['input'])];values=accessor(output,sampler['output'])
        for sample in case['samples']:
            row=values[max(0,bisect.bisect_right(times,sample['coordinate'])-1)]
            self.assertEqual(list(row),sample['result'])
    def test_step_and_continuous_axes_and_nodes_coexist(self):
        model=mdl_tests.MdlTests().skinned_model()
        stepped=record(0,((0,),(1,)))
        for time,key in zip((0,1),stepped['keys']):
            key['raw_coordinate']=time;key['raw_extra_hex']=struct.pack('<I4f',1,7,11,-3,4).hex()
        smooth=dict(record(1,((0,),(1,))),name='smooth')
        motion=dict(source_sha256='fixture',records=[stepped,smooth])
        same_node=bake(model,motion)
        self.assertEqual(same_node['extras']['fdk_bake']['split_axis_fields'],[dict(node=1,field='translation')])
        self.assertEqual(len(same_node['nodes']),10)
        self.assertEqual({s['interpolation'] for s in same_node['animations'][0]['samplers']},{'STEP','LINEAR'})
        smooth['target']=model['hierarchy']['nodes'][0]['name']
        output=bake(model,motion)
        self.assertEqual({s['interpolation'] for s in output['animations'][0]['samplers']},{'STEP','LINEAR'})
        stepped['keys']=[dict(stepped['keys'][0],raw_coordinate=t,raw_values=[v]) for t,v in ((-2,0),(.125,1),(3,2))]
        with self.assertRaisesRegex(ValueError,'loses precision'):bake(model,dict(motion,records=[stepped]))
    def test_same_node_independent_fields_use_their_own_interpolation(self):
        import itertools
        fields=('translation','quaternion','scale')
        values=(((0,1,0),(.1,1,0)),((0,0,0,1),(0,0,.1,math.sqrt(.99))),((1,1,1),(1.02,.98,1.01)))
        for modes in itertools.product((0,1),repeat=3):
            if len(set(modes))==1:continue
            model=mdl_tests.MdlTests().skinned_model();tracks=[]
            for kind,mode,pair in zip((9,10,11),modes,values):
                row=record(kind,pair)
                for key,time in zip(row['keys'],(0,1)):
                    key['raw_coordinate']=time;key['raw_extra_hex']=struct.pack('<I4f',mode,0,0,0,0).hex()
                tracks.append(row)
            motion=dict(source_sha256='mixed-fields-fixture',records=tracks)
            before=(inspection(model),copy.deepcopy(motion));output=bake(model,motion)
            self.assertEqual((inspection(model),motion),before)
            self.assertEqual(output['extras']['fdk_bake']['step_fields'],
                [dict(node=1,field=field) for field,mode in sorted(zip(fields,modes)) if mode==1])
            animation=output['animations'][0]
            for channel in animation['channels']:
                target=channel['target'];node=output['nodes'][target['node']]['name']
                field='quaternion' if target['path']=='rotation' or target['path']=='scale' and node.endswith('__fdk_raw_head') else target['path']
                expected='STEP' if modes[fields.index(field)]==1 else 'LINEAR'
                self.assertEqual(animation['samplers'][channel['sampler']]['interpolation'],expected)
            self.assertLessEqual(output['extras']['fdk_bake']['max_checked_matrix_error'],1e-5)
        mixed=record(3,((0,),(.1,)));other=record(4,((0,),(.1,)))
        for key in mixed['keys']:key['raw_extra_hex']=struct.pack('<I4f',1,0,0,0,0).hex()
        with self.assertRaisesRegex(ValueError,'one field'):bake(model,dict(motion,records=[mixed,other]))
    def test_mixed_translation_and_scale_axes_preserve_chain_skin_and_pivot(self):
        import itertools
        combinations=[v for v in itertools.product((0,1),repeat=3) if len(set(v))>1]
        for translation_modes,scale_modes in itertools.product(combinations,repeat=2):
            model=mdl_tests.MdlTests().skinned_model()
            model['hierarchy']['nodes'][1]['auxiliary']=[.2,-.3,.4]
            tracks=[]
            for field,base_kind,modes in (('translation',0,translation_modes),('scale',6,scale_modes)):
                for component,mode in enumerate(modes):
                    initial=model['hierarchy']['nodes'][1][field][component]
                    row=record(base_kind+component,((initial,),(initial+.02*(component+1),)))
                    for key,time in zip(row['keys'],(0,1)):
                        key['raw_coordinate']=time;key['raw_extra_hex']=struct.pack('<I4f',mode,0,0,0,0).hex()
                    tracks.append(row)
            q=record(10,((0,0,0,1),(0,0,.1,math.sqrt(.99))))
            for key,time in zip(q['keys'],(0,1)):key['raw_coordinate']=time
            motion=dict(source_sha256='mixed-axes-fixture',records=tracks+[q]);before=(inspection(model),copy.deepcopy(motion))
            output=bake(model,motion)
            self.assertEqual((inspection(model),motion),before)
            self.assertEqual(output['extras']['fdk_bake']['split_axis_fields'],[dict(node=1,field='scale'),dict(node=1,field='translation')])
            self.assertEqual(len(output['nodes']),13)
            self.assertEqual(output['skins'][0]['joints'],[0,1])
            self.assertEqual(output['nodes'][2]['skin'],0)
            visited=set();pending=list(output['scenes'][0]['nodes'])
            while pending:
                index=pending.pop();self.assertNotIn(index,visited);visited.add(index)
                pending.extend(output['nodes'][index].get('children',[]))
            self.assertEqual(visited,set(range(len(output['nodes']))))
            animation=output['animations'][0]
            for channel in animation['channels']:
                target=channel['target'];name=output['nodes'][target['node']]['name']
                if '_axis_' not in name:continue
                field='translation' if '_translation_axis_' in name else 'scale'
                component=int(name[-1]);modes=translation_modes if field=='translation' else scale_modes
                sampler=animation['samplers'][channel['sampler']]
                self.assertEqual(sampler['interpolation'],'STEP' if modes[component] else 'LINEAR')
                for values in accessor(output,sampler['output']):
                    self.assertTrue(all(values[c]==(0 if field=='translation' else 1) for c in range(3) if c!=component))
            self.assertLessEqual(output['extras']['fdk_bake']['max_checked_matrix_error'],1e-5)
    def test_explicit_pre_post_range_preserves_native_samples_and_sources(self):
        import bisect
        model=mdl_tests.MdlTests().skinned_model()
        track=dict(record(0,((0,),(1,))),raw_words=[1,1])
        motion=dict(source_sha256='fixture',records=[track]);before=(inspection(model),copy.deepcopy(motion))
        output=bake(model,motion,source_range=(0,6))
        self.assertEqual((inspection(model),motion),before)
        meta=output['extras']['fdk_bake'];self.assertEqual((meta['source_start'],meta['source_end']),(0,6))
        self.assertTrue(meta['explicit_source_range']);self.assertEqual(meta['pre_post_modes'],[1])
        animation=output['animations'][0];c=next(c for c in animation['channels'] if c['target']['path']=='translation')
        sampler=animation['samplers'][c['sampler']];times=[v[0] for v in accessor(output,sampler['input'])];values=accessor(output,sampler['output'])
        # Native pre extrapolation rises towards the second value backwards;
        # post extrapolation continues the final slope forwards.
        for coordinate,expected in ((0,1),(1,.5),(2,0),(3,.5),(4,1),(5,1.5),(6,2)):
            right=bisect.bisect_left(times,coordinate)
            if right==0:actual=values[0][0]
            else:
                fraction=(coordinate-times[right-1])/(times[right]-times[right-1])
                actual=values[right-1][0]+(values[right][0]-values[right-1][0])*fraction
            self.assertAlmostEqual(actual,expected,places=6)
    def test_repeated_event_budget_endpoint_jumps_and_quaternion_refusals(self):
        model=mdl_tests.MdlTests().skinned_model()
        track=dict(record(0,((0,),(1,))),raw_words=[3,3]);motion=dict(source_sha256='fixture',records=[track])
        with self.assertRaisesRegex(ValueError,'explicit source range'):bake(model,motion)
        with self.assertRaisesRegex(ValueError,'endpoint jumps'):bake(model,motion,source_range=(0,6))
        track['raw_words']=[4,4]
        with self.assertRaisesRegex(ValueError,'sample limit'):bake(model,motion,source_range=(-1e6,1e6),max_samples=16)
        track['raw_words']=[5,5]
        output=bake(model,motion,source_range=(0,8))
        self.assertEqual((output['extras']['fdk_bake']['source_start'],output['extras']['fdk_bake']['source_end']),(0,8))
        quaternion=dict(record(10,((0,0,0,1),(0,0,1,0))),raw_words=[4,4])
        with self.assertRaisesRegex(ValueError,'Quaternion loop-offset'):bake(model,dict(motion,records=[quaternion]),source_range=(0,6))
        for source_range in ((1,1),(2,1),(math.nan,2),(0,math.inf),(0,)):
            with self.assertRaises(ValueError):bake(model,motion,source_range=source_range)
    def test_whole_vector_curve_export_matches_frozen_components(self):
        import bisect
        model=mdl_tests.MdlTests().skinned_model()
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_vector_curves.json').read_text())
        case=next(case for case in fixtures['cases'] if case['record']['name']=='vector-curve-export-kind9')
        track=dict(case['record'],target='Child');motion=dict(source_sha256='fixture',records=[track])
        before=(inspection(model),copy.deepcopy(motion));output=bake(model,motion)
        self.assertEqual((inspection(model),motion),before)
        animation=output['animations'][0];c=next(c for c in animation['channels'] if c['target']['path']=='translation')
        sampler=animation['samplers'][c['sampler']];times=[r[0] for r in accessor(output,sampler['input'])];values=accessor(output,sampler['output'])
        for sample in case['samples']:
            t=sample['coordinate'];right=bisect.bisect_left(times,t)
            if right==0:actual=values[0]
            elif right==len(times):actual=values[-1]
            else:
                fraction=(t-times[right-1])/(times[right]-times[right-1])
                actual=[a+(b-a)*fraction for a,b in zip(values[right-1],values[right])]
            self.assertLessEqual(max(abs(a-b) for a,b in zip(actual,sample['result'])),2e-5)
        bad=copy.deepcopy(track);bad['keys'][0]['raw_values'][1]=1
        with self.assertRaisesRegex(ValueError,'Vector mode2 component jumps'):bake(model,dict(motion,records=[bad]))
        ignored=copy.deepcopy(track);ignored['keys'][1]['raw_values'][1]=1
        middle=bake(model,dict(motion,records=[ignored]))
        self.assertEqual(middle,output)
    def test_refusals_and_bake_budget(self):
        model=mdl_tests.MdlTests().skinned_model();motion=dict(source_sha256='fixture',records=[record()])
        for kwargs in [dict(max_error=0),dict(max_error=math.nan),dict(max_samples=1),dict(max_samples=5000),dict(seconds_per_unit=0),dict(seconds_per_unit=math.inf)]:
            with self.assertRaises(ValueError):bake(model,motion,**kwargs)
        for records in [[record(),record()],[dict(record(),target='Missing')],[dict(record(),raw_words=[1,0])],[dict(record(),target='Mesh')]]:
            with self.assertRaises(ValueError):bake(model,dict(motion,records=records))
        rotation=record(10,((0,0,0,1),(math.sqrt(.5),0,0,math.sqrt(.5))))
        with self.assertRaises(ValueError):bake(model,dict(motion,records=[rotation]),max_samples=2)
        stepped=record()
        stepped['keys'].insert(1,dict(stepped['keys'][0],raw_coordinate=3))
        for key,mode in zip(stepped['keys'],(3,4,4)):key['raw_extra_hex']=struct.pack('<I4f',mode,7,11,-3,4).hex()
        with self.assertRaisesRegex(ValueError,'Discontinuous'):bake(model,dict(motion,records=[stepped]))

if __name__=='__main__':unittest.main()
