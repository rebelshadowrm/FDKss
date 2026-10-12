import copy,hashlib,json,math,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from mdl_animation import inspect,WIDTHS,LinearChannel,raw_transform_matrix,sample_pose,float_order,ordered_float

def channel(kind=9,values=((0,1,2),(4,5,6)),times=(2,4)):
    return dict(raw_kind=kind,raw_words=[0,0],keys=[dict(raw_coordinate=t,raw_values=list(v),raw_extra_hex='00'*20) for t,v in zip(times,values)])

def fixture(kind=9,coordinate=2.5):
    record=b'\x05track\x04Root'+struct.pack('<4I',kind,3,7,1)
    record+=struct.pack('<f',coordinate)+struct.pack('<'+'f'*WIDTHS[kind],*range(WIDTHS[kind]))+bytes(range(20))
    section=struct.pack('<I',1)+record+struct.pack('<2f',0,4)
    return struct.pack('<4sII2I',b'MDL ',4,42,3,len(section))+section+struct.pack('<I',0xffffffff)

class AnimationTests(unittest.TestCase):
    def test_frozen_original_raw_pose_matrices(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_raw_pose.json').read_text())
        for case in fixtures['cases']:
            with self.subTest(case=case['name']):
                self.assertLess(max(abs(a-b) for a,b in zip(raw_transform_matrix(case['source']),case['matrix'])),3e-5)
    def test_pose_binding_copy_missing_targets_and_composition(self):
        parent=dict(name='Root',kind=0,translation=[1,2,3],quaternion=[0,0,0,1],rotation=[0,0,0],scale=[1,1,1],auxiliary=[0,0,0],children=[1])
        child=dict(parent,name='Child',translation=[4,5,6],children=[])
        model=dict(hierarchy=dict(nodes=[parent,child],roots=[0]))
        row=dict(channel(),name='motion',target='Child')
        missing=dict(channel(),name='unused',target='Other')
        motion=dict(records=[row,missing],source_sha256='fixture')
        before=copy.deepcopy((model,motion))
        pose=sample_pose(model,motion,3)
        self.assertEqual(pose['nodes'][1]['translation'],[2,3,4])
        self.assertEqual(pose['nodes'][1]['world_matrix'][12:15],[3,5,7])
        self.assertEqual(len(pose['bound_records']),1)
        self.assertEqual(pose['missing_targets'][0]['target'],'Other')
        self.assertEqual((model,motion),before)
        pose['nodes'][1]['translation'][0]=99
        self.assertEqual(model['hierarchy']['nodes'][1]['translation'],[4,5,6])
        # Near-angle fallback's nonunit result must survive into the matrix.
        end=[0,0,.01,math.sqrt(.9999)]
        rotation=dict(channel(10,((0,0,0,1),end)),name='rotation',target='Child')
        posed=sample_pose(model,dict(motion,records=[rotation]),3)['nodes'][1]
        self.assertLess(sum(v*v for v in posed['quaternion']),1)
        self.assertAlmostEqual(posed['local_matrix'][0],1-2*.005**2)
    def test_pose_refuses_ambiguous_and_invalid_graphs(self):
        node=dict(name='Root',kind=0,translation=[0,0,0],quaternion=[0,0,0,1],rotation=[0,0,0],scale=[1,1,1],auxiliary=[0,0,0],children=[])
        model=dict(hierarchy=dict(nodes=[node],roots=[0]))
        row=dict(channel(),name='motion',target='Root')
        motion=dict(records=[row],source_sha256='fixture')
        for coordinate in (math.nan,math.inf,1e50):
            with self.assertRaises(ValueError):sample_pose(model,motion,coordinate)
        with self.assertRaises(ValueError):sample_pose(model,dict(motion,records=[row,row]),3)
        for tree in [dict(nodes=[node,node],roots=[0,1]),dict(nodes=[dict(node,children=[0])],roots=[0]),
                     dict(nodes=[node],roots=[]),dict(nodes=[node],roots=[9]),dict(nodes=[dict(node,kind=2)],roots=[0])]:
            with self.assertRaises(ValueError):sample_pose(dict(hierarchy=tree),motion,3)
        for changed in [dict(node,quaternion=[0,0,0]),dict(node,scale=[math.inf,1,1]),dict(node,translation=[1e50,0,0])]:
            with self.assertRaises(ValueError):raw_transform_matrix(changed)
    def test_frozen_original_engine_samples(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_sampling.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    for actual,expected in zip(sampler.sample(sample['coordinate']),sample['result']):
                        self.assertLessEqual(abs(actual-expected),2e-6)
    def test_frozen_original_easing_and_mixed_segments(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_easing.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    for actual,expected in zip(sampler.sample(sample['coordinate']),sample['result']):
                        self.assertLessEqual(abs(actual-expected),2e-6)
    def test_frozen_original_cubic_neighbors_and_mode_delegation(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_cubic.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    for actual,expected in zip(sampler.sample(sample['coordinate']),sample['result']):
                        self.assertLessEqual(abs(actual-expected),2e-6)
    def test_frozen_original_component_samples(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_components.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    self.assertLessEqual(abs(sampler.sample(sample['coordinate'])[0]-sample['result'][0]),2e-6)
    def test_frozen_original_scalar_curves_and_dispatch(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_scalar_curves.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    self.assertLessEqual(abs(sampler.sample(sample['coordinate'])[0]-sample['result'][0]),2e-5)
    def test_curve_controls_copy_and_finite_guards(self):
        row=channel(0,((0,),(1,)),(0,1))
        for key in row['keys']:
            t=key['raw_coordinate'];v=key['raw_values'][0]
            key['raw_extra_hex']=struct.pack('<I4f',2,t-1/3,v,t+1/3,v).hex()
        sampler=LinearChannel(row);before=sampler.sample(.25)
        row['keys'][0]['raw_extra_hex']='00'*20
        self.assertEqual(sampler.sample(.25),before)
        self.assertFalse(sampler.discontinuous)
        for index in range(4):
            for value in (math.nan,math.inf,-math.inf):
                controls=[0,0,1,1];controls[index]=value
                bad=copy.deepcopy(row);bad['keys'][0]['raw_extra_hex']=struct.pack('<I4f',2,*controls).hex()
                with self.assertRaisesRegex(ValueError,'finite curve controls'):LinearChannel(bad)
        for kind in (9,10,11):
            bad=channel(kind,((0,0,0,1),(0,0,0,1))) if kind==10 else channel(kind)
            bad['keys'][0]['raw_extra_hex']=struct.pack('<I4f',2,0,math.inf,1,1).hex()
            with self.assertRaisesRegex(ValueError,'finite curve controls'):LinearChannel(bad)
        row['keys'][0]['raw_extra_hex']=struct.pack('<I4f',2,0,0,1,1).hex()
        row['keys'][1]['raw_extra_hex']=struct.pack('<I4f',4,0,0,1,1).hex()
        self.assertTrue(LinearChannel(row).discontinuous)
        row['keys'][0]['raw_extra_hex']=struct.pack('<I4f',3,0,0,1,math.inf).hex()
        row['keys'][1]['raw_extra_hex']=struct.pack('<I4f',2,0,0,1,1).hex()
        with self.assertRaisesRegex(ValueError,'finite curve controls'):LinearChannel(row)
    def test_frozen_original_steps_boundaries_and_dispatch(self):
        import bisect
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_steps.json').read_text())
        early=0
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    self.assertLessEqual(max(abs(a-b) for a,b in zip(sampler.sample(sample['coordinate']),sample['result'])),2e-6)
            if all(mode==1 for mode in sampler.modes):
                transitions=sampler.step_transitions()
                early+=sum(t<key for t,key in zip(transitions,sampler.times[1:]))
                for sample in case['samples']:
                    coordinate=struct.unpack('<f',struct.pack('<f',sample['coordinate']))[0]
                    value=sampler.values[bisect.bisect_right(transitions,coordinate)]
                    self.assertEqual(list(value),sample['result'])
                for transition,left,right in zip(transitions,sampler.values,sampler.values[1:]):
                    self.assertEqual(sampler.sample(transition),list(right))
                    self.assertEqual(sampler.sample(ordered_float(float_order(transition)-1)),list(left))
        self.assertGreater(early,0)
    def test_step_pose_preserves_fields_and_source(self):
        node=dict(name='Root',kind=0,translation=[1,2,3],quaternion=[0,0,0,1],rotation=[0,0,0],scale=[1,1,1],auxiliary=[0,0,0],children=[])
        row=dict(channel(0,((4,),(8,)),(0,1)),name='step',target='Root')
        for key in row['keys']:key['raw_extra_hex']=struct.pack('<I4f',1,7,11,-3,4).hex()
        model=dict(hierarchy=dict(nodes=[node],roots=[0]));motion=dict(records=[row],source_sha256='fixture')
        before=copy.deepcopy((model,motion))
        self.assertEqual(sample_pose(model,motion,.99)['nodes'][0]['translation'],[4,2,3])
        self.assertEqual(sample_pose(model,motion,1)['nodes'][0]['translation'],[8,2,3])
        self.assertEqual((model,motion),before)
        row['keys'][0]['raw_coordinate']=-3e38;row['keys'][1]['raw_coordinate']=3e38
        with self.assertRaisesRegex(ValueError,'finite float32 arithmetic'):LinearChannel(row).sample(0)
    def test_frozen_original_pre_post_modes_and_quaternion_offset_paths(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_prepost.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    self.assertLessEqual(max(abs(a-b) for a,b in zip(sampler.sample(sample['coordinate']),sample['result'])),2e-6)
    def test_pre_post_copy_single_key_native_pre_slope_and_pose(self):
        row=channel(0,((0,),(1,),(4,)),(2,3,5));row.update(name='boundary',target='Root',raw_words=[1,1])
        sampler=LinearChannel(row);row['raw_words'][0]=0
        self.assertEqual(sampler.sample(0),[2])
        self.assertEqual(sampler.sample(7),[7])
        node=dict(name='Root',kind=0,translation=[8,2,3],quaternion=[0,0,0,1],rotation=[0,0,0],scale=[1,1,1],auxiliary=[0,0,0],children=[])
        model=dict(hierarchy=dict(nodes=[node],roots=[0]));row['raw_words']=[1,1]
        motion=dict(records=[row],source_sha256='fixture');before=copy.deepcopy((model,motion))
        self.assertEqual(sample_pose(model,motion,0)['nodes'][0]['translation'],[2,2,3])
        self.assertEqual((model,motion),before)
        for kind,value in ((0,(2,)),(9,(1,2,3)),(10,(0,0,0,1))):
            for mode in range(6):
                one=channel(kind,(value,),(2,));one['raw_words']=[mode,mode]
                self.assertEqual(LinearChannel(one).sample(-100),list(value))
                self.assertEqual(LinearChannel(one).sample(100),list(value))
        bad=channel(0,((0,),(1,)),(-3e38,3e38));bad['raw_words']=[5,5]
        with self.assertRaises(ValueError):LinearChannel(bad).sample(-3.4e38)
    def test_frozen_original_whole_vector_curves_and_offsets(self):
        fixtures=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_vector_curves.json').read_text())
        for case in fixtures['cases']:
            sampler=LinearChannel(case['record'])
            for sample in case['samples']:
                with self.subTest(track=case['record']['name'],coordinate=sample['coordinate']):
                    self.assertLessEqual(max(abs(a-b) for a,b in zip(sampler.sample(sample['coordinate']),sample['result'])),2e-6)
    def test_vector_curve_zeroed_components_raw_quaternion_and_source_copy(self):
        fixture=json.loads((Path(__file__).parent/'fixtures'/'mdl_animation_vector_curve_pose.json').read_text())
        node=fixture['source']
        model=dict(hierarchy=dict(nodes=[node],roots=[0]))
        row=fixture['record']
        motion=dict(records=[row],source_sha256='fixture');before=copy.deepcopy((model,motion))
        posed=sample_pose(model,motion,fixture['coordinate'])['nodes'][0]
        for native in fixture['games'].values():
            self.assertEqual(posed['quaternion'],native['sampled_quaternion'])
            self.assertEqual(posed['local_matrix'],native['original_local_matrix'])
        self.assertEqual((model,motion),before)
        self.assertTrue(LinearChannel(row).curve_component_jumps)
        self.assertEqual(LinearChannel(row).sample(0),[0,0,0,1])
        self.assertEqual(LinearChannel(row).sample(1),[0,0,1,0])
        # A right-only unused mode2 marker does not zero a linear segment.
        row['keys'][0]['raw_extra_hex']='00'*20
        self.assertFalse(LinearChannel(row).curve_component_jumps)
        self.assertNotEqual(LinearChannel(row).sample(.5),[0,0,0,0])
    def test_component_pose_copy_partial_quaternion_and_overlap(self):
        node=dict(name='Root',kind=0,translation=[1,2,3],quaternion=[0,0,0,1],rotation=[0,0,0],scale=[1,1,1],auxiliary=[0,0,0],children=[])
        model=dict(hierarchy=dict(nodes=[node],roots=[0]))
        rows=[dict(channel(kind,((0,),(1,))),name=f'component-{kind}',target='Root') for kind in range(9)]
        motion=dict(records=rows,source_sha256='fixture');before=copy.deepcopy((model,motion))
        pose=sample_pose(model,motion,3)
        self.assertEqual(pose['nodes'][0]['translation'],[.5]*3)
        self.assertEqual(pose['nodes'][0]['scale'],[.5]*3)
        self.assertEqual(pose['nodes'][0]['quaternion'],[.5,.5,.5,1])
        self.assertEqual(len(pose['bound_records']),9)
        self.assertEqual([r['component'] for r in pose['bound_records']],[0,1,2]*3)
        self.assertEqual((model,motion),before)
        whole=dict(channel(),name='whole',target='Root')
        for duplicates in ([rows[0],rows[0]],[whole,rows[0]],[rows[0],whole]):
            with self.assertRaises(ValueError):sample_pose(model,dict(motion,records=duplicates),3)
    def test_cubic_endpoint_tangents_and_discontinuous_fallback(self):
        row=channel(values=((0,0,0),(2,2,2),(0,0,0)),times=(0,1,3))
        for key in row['keys']:key['raw_extra_hex']=struct.pack('<I4f',3,7,11,-3,4).hex()
        sampler=LinearChannel(row)
        self.assertFalse(sampler.discontinuous)
        # Endpoints use one-sided differences; the interior tangent is half
        # the neighboring value difference, independent of nonuniform times.
        self.assertEqual(sampler.sample(.5),[1.25]*3)
        self.assertEqual(sampler.sample(2),[1.25]*3)
        row['keys'][1]['raw_extra_hex']=struct.pack('<I4f',4,7,11,-3,4).hex()
        sampler=LinearChannel(row)
        self.assertTrue(sampler.discontinuous)
        self.assertEqual(sampler.sample(.75),[0]*3)
        self.assertEqual(sampler.sample(1),[2]*3)
    def test_easing_left_mode_and_payload_copy(self):
        for mode,expected in ((4,.0625),(5,.4375),(6,.125)):
            row=channel(values=((0,0,0),(1,1,1)),times=(0,1))
            row['keys'][0]['raw_extra_hex']=struct.pack('<I4f',mode,7,11,-3,4).hex()
            sampler=LinearChannel(row)
            row['keys'][0]['raw_extra_hex']='00'*20
            self.assertEqual(sampler.sample(.25),[expected]*3)
            self.assertEqual(sampler.sample(.5),[{4:.25,5:.75,6:.5}[mode]]*3)
            self.assertEqual(sampler.sample(0),[0]*3)
            self.assertEqual(sampler.sample(1),[1]*3)
    def test_clamp_and_vector_interpolation(self):
        for kind in (9,11):
            sample=LinearChannel(channel(kind)).sample
            self.assertEqual(sample(-1),[0,1,2])
            self.assertEqual(sample(3),[2,3,4])
            self.assertEqual(sample(4),[4,5,6])
            self.assertEqual(sample(10),[4,5,6])
        self.assertEqual(LinearChannel(channel(times=(2,),values=((1,2,3),))).sample(99),[1,2,3])
    def test_quaternion_shortest_arc_and_linear_threshold(self):
        sampler=LinearChannel(channel(10,((0,0,0,1),(0,0,1,0))))
        for actual,expected in zip(sampler.sample(3),(0,0,math.sqrt(.5),math.sqrt(.5))):
            self.assertAlmostEqual(actual,expected)
        negative=LinearChannel(channel(10,((0,0,0,-1),(0,0,0,1))))
        self.assertEqual(negative.sample(3),[0,0,0,1])
        # Below-angle fallback does not normalize; this matters for native parity.
        end=(0,0,.01,math.sqrt(1-.01**2))
        midpoint=LinearChannel(channel(10,((0,0,0,1),end))).sample(3)
        self.assertEqual(midpoint,[0,0,.005,(1+end[3])/2])
        self.assertLess(sum(v*v for v in midpoint),1)
    def test_sampler_refuses_unsupported_or_invalid_inputs(self):
        cases=[]
        for field,value in [('raw_kind',12),('raw_words',[6,0]),('keys',[])]:
            row=channel();row[field]=value;cases.append(row)
        for times in ((2,2),(4,2),(2,math.inf)):
            cases.append(channel(times=times))
        for field,value in [('raw_extra_hex','07'+'00'*19),('raw_extra_hex','00'),('raw_values',[1,2]),('raw_values',[math.nan,2,3])]:
            row=channel();row['keys'][0][field]=value;cases.append(row)
        cases.append(channel(10,((0,0,0,2),(0,0,0,1))))
        for row in cases:
            with self.assertRaises(ValueError):LinearChannel(row)
        for coordinate in (math.nan,math.inf,1e50):
            with self.assertRaises(ValueError):LinearChannel(channel()).sample(coordinate)
    def test_widths_values_and_opaque_bytes_are_preserved(self):
        for kind,width in enumerate(WIDTHS):
            data=fixture(kind);result=inspect(data);record=result['records'][0]
            self.assertEqual(record['component_count'],width)
            self.assertEqual(record['raw_words'],[3,7])
            self.assertEqual(record['keys'][0]['raw_values'],list(range(width)))
            self.assertEqual(record['keys'][0]['raw_extra_hex'],bytes(range(20)).hex())
            self.assertEqual(result['source_sha256'],hashlib.sha256(data).hexdigest())
    def test_truncation_nonfinite_counts_kinds_and_trailing_bytes(self):
        data=fixture()
        for end in range(len(data)):
            with self.subTest(end=end),self.assertRaises(ValueError):inspect(data[:end])
        for value in (math.nan,math.inf):
            with self.assertRaises(ValueError):inspect(fixture(coordinate=value))
        for offset,value in ((20,10001),(35,17),(47,100001)):
            corrupted=bytearray(data);struct.pack_into('<I',corrupted,offset,value)
            with self.subTest(offset=offset),self.assertRaises(ValueError):inspect(corrupted)
        with self.assertRaises(ValueError):inspect(data+b'junk')

if __name__=='__main__':unittest.main()
