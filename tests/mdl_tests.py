import base64
import math
from pathlib import Path
import struct
import sys
import unittest
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import mdl


def fixture(indices=(0, 1, 2), positions=None, normal=(0, 0, 126, 0), skin=False):
    positions = positions or (0, 0, 0, 1, 0, 0, 0, 1, 0)
    streams = [(0, 12, struct.pack('<9f', *positions)),
               (1, 4, struct.pack('<4b', *normal) * 3),
               (4, 8, struct.pack('<6f', 0, 0, 1, 0, 0, 1)),
               (7, 4, struct.pack('<3I', *indices))]
    if skin:
        streams.append((5, 16, bytes(48)))
    primitive = struct.pack('<5I', 1, 0, 3, 6, 0)
    metadata = struct.pack('<I', 1) + b'\x08triangle' + struct.pack('<I', len(primitive)) + primitive + struct.pack('<I', 0)
    payload = struct.pack('<I', len(streams))
    payload += b''.join(struct.pack('<5I', kind, len(data), stride, 0, 0) for kind, stride, data in streams)
    payload += b''.join(data for _, _, data in streams)
    return (struct.pack('<4sII', b'MDL ', 4, 0) + struct.pack('<2I', 1, len(metadata)) + metadata
            + struct.pack('<2I', 4, len(payload)) + payload + struct.pack('<I', 0xffffffff))


class MdlTests(unittest.TestCase):
    def test_vertex_colors_are_opt_in_and_follow_layout_flags(self):
        source = bytes((10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120))
        for flags in (1, 2, 3, 6, 7):
            model = mdl.parse(fixture())
            primitive = model['meshes'][0]['submeshes'][0]
            primitive['unknown'] = flags
            primitive['streams'].append(dict(kind=3, stride=4, count=3, data=source))
            default = mdl.gltf(model)
            self.assertNotIn('COLOR_0', default['meshes'][0]['primitives'][0]['attributes'])
            output = mdl.gltf(model, vertex_colors=True)
            index = output['meshes'][0]['primitives'][0]['attributes']['COLOR_0']
            accessor = output['accessors'][index]
            self.assertEqual((accessor['type'], accessor['componentType'], accessor['normalized'], accessor['count']), ('VEC4', 5121, True, 3))
            view = output['bufferViews'][accessor['bufferView']]
            data = base64.b64decode(output['buffers'][0]['uri'].split(',')[1])
            expected = bytes(v for b, g, r, a in struct.iter_unpack('<4B', source) for v in (r, g, b, a)) if flags & 1 else source
            self.assertEqual(data[view['byteOffset']:view['byteOffset']+view['byteLength']], expected)

    def test_vertex_color_refusals_and_absent_stream(self):
        model = mdl.parse(fixture())
        self.assertNotIn('COLOR_0', mdl.gltf(model, vertex_colors=True)['meshes'][0]['primitives'][0]['attributes'])
        primitive = model['meshes'][0]['submeshes'][0]
        color = dict(kind=3, stride=4, count=3, data=bytes(12))
        primitive['streams'].append(color)
        for flags in (0, 4):
            primitive['unknown'] = flags
            with self.assertRaisesRegex(ValueError, 'Unsupported vertex color encoding'):
                mdl.gltf(model, vertex_colors=True)
        primitive['unknown'] = 6
        color['stride'] = 16
        with self.assertRaisesRegex(ValueError, 'Unsupported vertex color encoding'):
            mdl.gltf(model, vertex_colors=True)
        color['stride'] = 4
        color['data'] = bytes(11)
        with self.assertRaisesRegex(ValueError, 'Invalid vertex color payload size'):
            mdl.gltf(model, vertex_colors=True)
        color['data'] = bytes(12)
        primitive['streams'].append(color.copy())
        with self.assertRaisesRegex(ValueError, 'Duplicate vertex color stream'):
            mdl.gltf(model, vertex_colors=True)

    def skinned_model(self):
        model = mdl.parse(fixture())
        identity = [float(r == c) for c in range(4) for r in range(4)]
        translated = identity.copy(); translated[13] = 1
        model['meshes'][0]['skin_bindings'] = [dict(name='Root', matrix=identity), dict(name='Child', matrix=translated)]
        primitive = model['meshes'][0]['submeshes'][0]
        for kind, data in [(5, struct.pack('<4f', .25, .75, -1e-19, 0)*3),
                           (6, struct.pack('<4I', 0, 1, 1, 1)*3)]:
            primitive['streams'].append(dict(kind=kind, data=data, stride=16, count=3))
        def node(name, kind, mesh=0xffffffff, translation=(0, 0, 0), children=()):
            return dict(name=name, kind=kind, mesh=mesh, translation=translation, quaternion=(0,0,0,1),
                        skin=2 if kind==1 else 0, rotation=(0,0,0), scale=(1,1,1), auxiliary=(0,0,0), children=list(children))
        model['hierarchy'] = dict(roots=[0,2], nodes=[node('Root',1,children=(1,)),node('Child',1,translation=(0,1,0)),node('Mesh',2,mesh=0)])
        return model

    def accessor_bytes(self, output, index):
        accessor=output['accessors'][index];view=output['bufferViews'][accessor['bufferView']]
        binary=base64.b64decode(output['buffers'][0]['uri'].split(',')[1])
        return binary[view['byteOffset']:view['byteOffset']+view['byteLength']]

    def test_skin_joint_remapping_inverse_direction_weights_and_roots(self):
        model=self.skinned_model();output=mdl.gltf(model,skinning=True)
        skin=output['skins'][0];self.assertEqual(skin['joints'],[0,1])
        self.assertEqual(output['nodes'][2]['skin'],0)
        self.assertNotIn('translation',output['nodes'][2])
        inverses=list(struct.iter_unpack('<16f',self.accessor_bytes(output,skin['inverseBindMatrices'])))
        self.assertEqual(inverses[1][13],-1)
        self.assertEqual(inverses[0][15],1)
        primitive=output['meshes'][0]['primitives'][0]
        joints=self.accessor_bytes(output,primitive['attributes']['JOINTS_0'])
        weights=self.accessor_bytes(output,primitive['attributes']['WEIGHTS_0'])
        self.assertEqual(struct.unpack_from('<4H',joints),(0,1,0,0))
        self.assertEqual(struct.unpack_from('<4f',weights),(.25,.75,0,0))
        self.assertEqual(model['hierarchy']['nodes'][0]['children'],[1])
        with self.assertRaises(ValueError):mdl.gltf(model,bind_pose=True,skinning=True)

    def test_skin_stream_and_binding_refusals(self):
        original=self.skinned_model()
        mutations=[
            lambda m:m['meshes'][0]['submeshes'][0]['streams'].pop(),
            lambda m:m['meshes'][0]['submeshes'][0]['streams'][-1].update(count=2),
            lambda m:m['meshes'][0]['submeshes'][0]['streams'][-1].update(data=struct.pack('<4I',0,2,0,0)*3),
            lambda m:m['meshes'][0]['submeshes'][0]['streams'][-2].update(data=struct.pack('<4f',math.nan,0,0,0)*3),
            lambda m:m['meshes'][0]['submeshes'][0]['streams'][-2].update(data=struct.pack('<4f',-.1,1.1,0,0)*3),
            lambda m:m['meshes'][0]['submeshes'][0]['streams'][-2].update(data=struct.pack('<4f',.25,.25,0,0)*3),
            lambda m:m['meshes'][0]['skin_bindings'][1].update(name='Missing'),
            lambda m:m['meshes'][0]['skin_bindings'][1].update(name='Root'),
            lambda m:m['meshes'][0]['skin_bindings'][1].update(matrix=[0]*16),
            lambda m:m['meshes'][0]['skin_bindings'][1]['matrix'].__setitem__(13,2),
            lambda m:m['hierarchy']['nodes'][2].update(translation=(1,0,0)),
            lambda m:m['hierarchy']['nodes'][1].update(name='Root'),
        ]
        for index,mutate in enumerate(mutations):
            model=self.skinned_model();mutate(model)
            with self.subTest(case=index),self.assertRaises(ValueError):mdl.gltf(model,skinning=True)
        matrix=original['meshes'][0]['skin_bindings'][1]['matrix'].copy();matrix[3]=1e-20
        inverse=mdl.inverse_bind(matrix)
        self.assertEqual([inverse[i] for i in (3,7,11,15)],[0,0,0,1])

    def test_skin_prunes_only_missing_bindings_with_zero_weight(self):
        model=self.skinned_model()
        model['meshes'][0]['skin_bindings'].insert(1,dict(name='Unused',matrix=[0]*16))
        primitive=model['meshes'][0]['submeshes'][0]
        primitive['streams'][-1]['data']=struct.pack('<4I',0,2,1,1)*3
        output=mdl.gltf(model,skinning=True)
        self.assertEqual(output['skins'][0]['joints'],[0,1])
        self.assertEqual(output['skins'][0]['extras']['fdk_unused_missing_bindings'],['Unused'])
        joints=output['meshes'][0]['primitives'][0]['attributes']['JOINTS_0']
        self.assertEqual(struct.unpack_from('<4H',self.accessor_bytes(output,joints)),(0,1,0,0))
        primitive['streams'][-1]['data']=struct.pack('<4I',0,1,0,0)*3
        with self.assertRaises(ValueError):mdl.gltf(model,skinning=True)

    def test_raw_inspection_preserves_uninterpreted_transforms_and_stream_hashes(self):
        import hashlib
        import json
        model = mdl.parse(fixture())
        model['hierarchy'] = {'roots': [0], 'nodes': [dict(name='rotated', kind=2, mesh=0,
            translation=(1, 2, 3), quaternion=(.2, .3, .4, .5), skin=7, rotation=(.1, .2, .3),
            scale=(2, 3, 4), auxiliary=(5, 6, 7), children=[])]}
        with self.assertRaises(ValueError):
            mdl.gltf(model, preserve_hierarchy=True)
        result = mdl.inspection(model)
        node = result['hierarchy']['nodes'][0]
        self.assertEqual(node['raw_vector4'], [.2, .3, .4, .5])
        self.assertEqual(node['raw_auxiliary3'], [5, 6, 7])
        self.assertEqual(node['raw_skin_flag'], 7)
        stream = model['meshes'][0]['submeshes'][0]['streams'][0]
        record = result['meshes'][0]['submeshes'][0]['streams'][0]
        self.assertEqual(record['sha256'], hashlib.sha256(stream['data']).hexdigest())
        self.assertEqual(record['byte_length'], len(stream['data']))
        self.assertEqual(json.loads(json.dumps(result, allow_nan=False))['hierarchy']['roots'], [0])
        node['children'].append(0)
        self.assertEqual(model['hierarchy']['nodes'][0]['children'], [])

    def png_fixture(self):
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(b'\0\xff\0\0\xff')) + chunk(b'IEND', b''))

    def test_embedded_diffuse_texture_wrap_and_uv_binding(self):
        model = mdl.parse(self.material_fixture())
        material = model['materials'][0]
        material['shader'] = 'map'; material['textures'][0]['slot'] = 0
        with tempfile.TemporaryDirectory() as directory:
            image = self.png_fixture(); Path(directory, 'diffuse.png').write_bytes(image)
            result = mdl.gltf(model, base_color_directory=directory)
            self.assertEqual(result['samplers'], [{'wrapS': 10497, 'wrapT': 33071}])
            self.assertEqual(result['materials'][0]['pbrMetallicRoughness']['baseColorTexture'], {'index': 0, 'texCoord': 0})
            self.assertEqual(base64.b64decode(result['images'][0]['uri'].split(',')[1]), image)
            material['uv_maps'] = [1]
            with self.assertRaisesRegex(ValueError, 'missing UV'):
                mdl.gltf(model, base_color_directory=directory)
            material['uv_maps'] = [0]; material['textures'][0]['name'] = '../diffuse'
            with self.assertRaisesRegex(ValueError, 'Unsafe'):
                mdl.gltf(model, base_color_directory=directory)

    def test_png_rejects_truncation_corruption_and_trailing_data(self):
        data = self.png_fixture()
        for end in range(len(data)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                mdl.png_image(data[:end])
        bad = bytearray(data); bad[29] ^= 1
        with self.assertRaisesRegex(ValueError, 'checksum'):
            mdl.png_image(bad)
        with self.assertRaisesRegex(ValueError, 'trailing'):
            mdl.png_image(data + b'x')

    def material_payload(self, parameter_type=6, parameter_words=None, material_count=1):
        def text(value):
            return bytes((len(value),)) + value.encode()
        widths = {0: 1, 1: 1, 2: 2, 3: 3, 4: 1, 5: 2, 6: 3, 7: 4, 8: 16, 0xffffffff: 0}
        if parameter_words is None:
            parameter_words = [0x3f800000] * widths.get(parameter_type, 0)
        material = text('wood') + text('standard') + text('standard')
        material += struct.pack('<I', 1) + text('diffuse') + struct.pack('<5I', 2, 7, 0, 2, 9)
        material += struct.pack('<I', 1) + text('tint') + struct.pack('<I', parameter_type)
        material += struct.pack('<' + 'I' * len(parameter_words), *parameter_words)
        material += struct.pack('<I', 1) + text('USE_COLOR') + struct.pack('<I', 1)
        material += struct.pack('<I', 1) + b'\0' + struct.pack('<I', 2) + b'\x03\x04'
        material += struct.pack('<5I', 10, 11, 12, 0x3f000000, 13)
        return struct.pack('<I', material_count) + material * material_count

    def material_fixture(self, **options):
        data = fixture(); payload = self.material_payload(**options)
        return data[:-4] + struct.pack('<II', 0, len(payload)) + payload + data[-4:]

    def test_material_metadata_preserves_bindings_and_primitive_assignment(self):
        model = mdl.parse(self.material_fixture())
        material = model['materials'][0]
        self.assertEqual(material['name'], 'wood')
        self.assertEqual(material['parameters'][0]['float_values'], [1, 1, 1])
        self.assertEqual(material['textures'][0], dict(name='diffuse', slot=2, flags0=7, wrap_u=0, wrap_v=2, flags1=9))
        self.assertEqual(material['tail_words'], [10, 11, 12, 0x3f000000, 13])
        result = mdl.gltf(model)
        self.assertEqual(result['materials'][0]['extras']['fdk_material'], material)
        self.assertEqual(result['meshes'][0]['primitives'][0]['material'], 0)
        self.assertNotIn('textures', result)

    def test_material_parameter_widths_and_malformed_data(self):
        for kind, width in enumerate((1, 1, 2, 3, 1, 2, 3, 4, 16)):
            with self.subTest(kind=kind):
                material = mdl.materials(self.material_payload(parameter_type=kind))[0]
                self.assertEqual(len(material['parameters'][0]['raw_words']), width)
                self.assertEqual(material['switches'], [{'name': 'USE_COLOR', 'value': 1}])
        self.assertEqual(mdl.materials(self.material_payload(parameter_type=0xffffffff))[0]['parameters'][0]['raw_words'], [])
        payload = self.material_payload()
        for end in range(len(payload)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                mdl.materials(payload[:end])
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            mdl.materials(self.material_payload(parameter_type=9))
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            mdl.materials(self.material_payload(parameter_type=4, parameter_words=[0x7fc00000]))
        with self.assertRaisesRegex(ValueError, 'material reference'):
            mdl.parse(self.material_fixture(material_count=0))
        with self.assertRaisesRegex(ValueError, 'Too many'):
            mdl.materials(struct.pack('<I', 10001))
        with self.assertRaisesRegex(ValueError, 'trailing'):
            mdl.materials(payload+b'x')

    def hierarchical_fixture(self, children=((1,), ()), meshes=(0xffffffff, 0), rotation=(0, 0, 0), auxiliary=(0, 0, 0)):
        records = bytearray(struct.pack('<I', 2))
        for index in range(2):
            name = ('root', 'triangle')[index].encode()
            records.extend(bytes((len(name),)) + name)
            records.extend(struct.pack('<II3f4fI3f3f3fI', 0 if index == 0 else 2, meshes[index],
                                       1, 2, 3, 0, 0, 0, 1, 0, *rotation, 2, 3, 4, *auxiliary, len(children[index])))
            records.extend(struct.pack('<' + 'I' * len(children[index]), *children[index]))
        data = fixture()
        return data[:-4] + struct.pack('<II', 2, len(records)) + records + data[-4:]

    def test_hierarchy_preserves_nodes_roots_translation_scale_and_mesh(self):
        model = mdl.parse(self.hierarchical_fixture())
        result = mdl.gltf(model, preserve_hierarchy=True)
        self.assertEqual(result['scenes'][0]['nodes'], [0])
        self.assertEqual(result['nodes'][0]['children'], [1])
        self.assertEqual(result['nodes'][1]['mesh'], 0)
        self.assertEqual(result['nodes'][1]['translation'], [1, 2, 3])
        self.assertEqual(result['nodes'][1]['scale'], [2, 3, 4])
        self.assertEqual(mdl.gltf(model)['nodes'], [{'name': 'triangle', 'mesh': 0}])

    def test_hierarchy_rejects_cycles_duplicate_parents_and_invalid_meshes(self):
        for children in (((1,), (0,)), ((1, 1), ()), ((2,), ()), ((0,), ())):
            with self.subTest(children=children), self.assertRaises(ValueError):
                mdl.parse(self.hierarchical_fixture(children=children))
        with self.assertRaisesRegex(ValueError, 'mesh reference'):
            mdl.parse(self.hierarchical_fixture(meshes=(0xffffffff, 1)))

    def test_hierarchy_requires_complete_metadata_and_verified_transforms(self):
        data = self.hierarchical_fixture()
        for end in range(len(fixture()) - 4, len(data)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                mdl.parse(data[:end])
        for options in ({'skin': 1}, {'kind': 1}):
            model = mdl.parse(self.hierarchical_fixture())
            model['hierarchy']['nodes'][0].update(options)
            with self.assertRaisesRegex(ValueError, 'static nodes'):
                mdl.gltf(model, preserve_hierarchy=True)
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            mdl.parse(self.hierarchical_fixture(rotation=(math.nan, 0, 0)))
        with self.assertRaisesRegex(ValueError, 'no hierarchy'):
            mdl.gltf(mdl.parse(fixture()), preserve_hierarchy=True)

    def test_rotation_and_pivot_match_original_engine_matrices(self):
        import json
        evidence = json.loads((Path(__file__).parent / 'fixtures' / 'mdl_transforms.json').read_text())
        for case in evidence['cases']:
            model = mdl.parse(self.hierarchical_fixture())
            source = model['hierarchy']['nodes'][1]
            source.update(case['source'])
            node = mdl.gltf(model, preserve_hierarchy=True)['nodes'][1]
            x, y, z, w = node.get('rotation', [0, 0, 0, 1])
            a, b, c = node['scale']
            actual = [(1-2*(y*y+z*z))*a, 2*(x*y+z*w)*a, 2*(x*z-y*w)*a, 0,
                      2*(x*y-z*w)*b, (1-2*(x*x+z*z))*b, 2*(y*z+x*w)*b, 0,
                      2*(x*z+y*w)*c, 2*(y*z-x*w)*c, (1-2*(x*x+y*y))*c, 0,
                      *node['translation'], 1]
            with self.subTest(name=case['name']):
                self.assertAlmostEqual(x*x+y*y+z*z+w*w, 1)
                for predicted, native in zip(actual, case['matrix']):
                    self.assertAlmostEqual(predicted, native, delta=3e-5)

    def test_hierarchy_rejects_nonunit_and_nonfinite_composed_rotations(self):
        for quaternion in ((0, 0, 0, 0), (0, 0, 0, 2), (.2, .3, .4, .5)):
            model = mdl.parse(self.hierarchical_fixture())
            model['hierarchy']['nodes'][0]['quaternion'] = quaternion
            with self.subTest(quaternion=quaternion), self.assertRaisesRegex(ValueError, 'unit quaternion'):
                mdl.gltf(model, preserve_hierarchy=True)
        model['hierarchy']['nodes'][0]['quaternion'] = (0, 0, 0, 1)
        model['hierarchy']['nodes'][0]['auxiliary'] = (math.inf, 0, 0)
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            mdl.gltf(model, preserve_hierarchy=True)

    def test_hierarchy_keeps_empty_mesh_nodes_and_remaps_geometry(self):
        model = mdl.parse(self.hierarchical_fixture())
        model['meshes'].insert(0, {'name': 'collision', 'skin_bindings': [], 'submeshes': []})
        model['hierarchy']['nodes'][0].update(kind=2, mesh=0)
        model['hierarchy']['nodes'][1]['mesh'] = 1
        result = mdl.gltf(model, preserve_hierarchy=True)
        self.assertEqual(result['nodes'][0]['extras'], {'fdk_empty_mesh_index': 0})
        self.assertEqual(result['nodes'][0]['children'], [1])
        self.assertEqual(result['nodes'][1]['mesh'], 0)

    def test_triangle_export_preserves_geometry_and_normalizes_normals(self):
        result = mdl.gltf(mdl.parse(fixture()))
        binary = base64.b64decode(result['buffers'][0]['uri'].split(',')[1])
        attributes = result['meshes'][0]['primitives'][0]['attributes']
        position = result['accessors'][attributes['POSITION']]
        self.assertEqual(position['min'], [0, 0, 0])
        self.assertEqual(position['max'], [1, 1, 0])
        normal = result['accessors'][attributes['NORMAL']]
        view = result['bufferViews'][normal['bufferView']]
        rows = struct.iter_unpack('<3f', binary[view['byteOffset']:view['byteOffset'] + view['byteLength']])
        for row in rows:
            self.assertAlmostEqual(math.hypot(*row), 1)
        self.assertTrue(all(v['byteOffset'] % 4 == 0 for v in result['bufferViews']))

    def test_every_truncation_is_rejected(self):
        data = fixture()
        for end in range(len(data)):
            with self.subTest(end=end), self.assertRaises(ValueError):
                mdl.parse(data[:end])

    def test_invalid_indices_and_nonfinite_positions(self):
        with self.assertRaisesRegex(ValueError, 'outside'):
            mdl.gltf(mdl.parse(fixture(indices=(0, 1, 3))))
        points = (math.nan, 0, 0, 1, 0, 0, 0, 1, 0)
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            mdl.gltf(mdl.parse(fixture(positions=points)))

    def test_bad_stream_reference_and_stride(self):
        data = bytearray(fixture())
        # First stream follows the header, metadata section, and stream section header/count.
        metadata_size = struct.unpack_from('<I', data, 16)[0]
        first = 20 + metadata_size + 12
        for offset, value in ((first + 8, 0), (first + 12, 1), (first + 16, 1)):
            mutated = bytearray(data)
            struct.pack_into('<I', mutated, offset, value)
            with self.assertRaisesRegex(ValueError, 'Invalid stream'):
                mdl.parse(mutated)

    def test_duplicate_sections_and_trailing_bytes(self):
        data = fixture()
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            mdl.parse(data[:-4] + struct.pack('<2I', 1, 0) + data[-4:])
        with self.assertRaisesRegex(ValueError, 'trailing'):
            mdl.parse(data + b'x')

    def test_zero_normal_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'zero normal'):
            mdl.gltf(mdl.parse(fixture(normal=(0, 0, 0, 0))))

    def test_empty_triangle_stream_is_rejected(self):
        model = mdl.parse(fixture())
        primitive = model['meshes'][0]['submeshes'][0]
        primitive['index_count'] = 0
        index = next(s for s in primitive['streams'] if s['kind'] == 7)
        index.update(count=0, data=memoryview(b''))
        with self.assertRaisesRegex(ValueError, 'triangle count'):
            mdl.gltf(model)

    def test_skin_stream_requires_explicit_bind_pose(self):
        model = mdl.parse(fixture(skin=True))
        with self.assertRaisesRegex(ValueError, 'bind-pose'):
            mdl.gltf(model)
        self.assertEqual(len(mdl.gltf(model, bind_pose=True)['meshes']), 1)


if __name__ == '__main__':
    unittest.main()
