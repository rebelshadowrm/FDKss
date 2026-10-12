"""Bounded MDL v4 mesh inspection and static glTF export; preserves engine local coordinates."""
import argparse
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


class Reader:
    def __init__(self, data):
        self.data, self.offset = memoryview(data), 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.offset:
            raise ValueError('MDL field extends beyond its section')
        start = self.offset
        self.offset += size
        return self.data[start:self.offset]

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def u32(self):
        return self.unpack('<I')[0]

    def name(self):
        size = self.unpack('<B')[0]
        return bytes(self.take(size)).decode('utf-8')

    def done(self):
        if self.offset != len(self.data):
            raise ValueError('Unexpected trailing section data')


def materials(data):
    reader = Reader(data)
    def count(limit=10000):
        value = reader.u32()
        if value > limit:
            raise ValueError('Too many material records')
        return value
    def text():
        value = reader.name()
        if '\0' in value:
            raise ValueError('NUL in material name')
        return value
    output = []
    widths = (1, 1, 2, 3, 1, 2, 3, 4, 16)
    for _ in range(count()):
        material = {'name': text(), 'shader': text(), 'shader_variant': text()}
        textures = []
        for _ in range(count(1024)):
            name = text()
            slot, flags0, wrap_u, wrap_v, flags1 = reader.unpack('<5I')
            textures.append(dict(name=name, slot=slot, flags0=flags0,
                                 wrap_u=wrap_u, wrap_v=wrap_v, flags1=flags1))
        parameters = []
        for _ in range(count(1024)):
            name, kind = text(), reader.u32()
            if kind == 0xffffffff:
                words = []
            elif kind < len(widths):
                words = list(reader.unpack('<' + 'I' * widths[kind]))
            else:
                raise ValueError('Unsupported material parameter encoding')
            parameter = {'name': name, 'type': kind, 'raw_words': words}
            if kind in (4, 5, 6):
                values = list(struct.unpack('<' + 'f' * len(words), struct.pack('<' + 'I' * len(words), *words)))
                if not all(math.isfinite(v) for v in values):
                    raise ValueError('Nonfinite material parameter')
                parameter['float_values'] = values
            parameters.append(parameter)
        switches = [{'name': text(), 'value': reader.u32()} for _ in range(count(1024))]
        uv_maps = list(reader.take(count(1024)))
        auxiliary = list(reader.take(count(1024)))
        tail = list(reader.unpack('<5I'))
        material.update(textures=textures, parameters=parameters, switches=switches,
                        uv_maps=uv_maps, auxiliary=auxiliary, tail_words=tail)
        output.append(material)
    reader.done()
    return output


def hierarchy(data, mesh_count):
    reader = Reader(data)
    count = reader.u32()
    if not 1 <= count <= 10000:
        raise ValueError('Invalid hierarchy node count')
    nodes = []
    for _ in range(count):
        name = reader.name()
        kind, mesh = reader.unpack('<II')
        translation = reader.unpack('<3f')
        quaternion = reader.unpack('<4f')
        skin = reader.u32()
        rotation = reader.unpack('<3f')
        scale = reader.unpack('<3f')
        auxiliary = reader.unpack('<3f')
        child_count = reader.u32()
        if child_count > count or kind not in (0, 1, 2) or not name or '\0' in name:
            raise ValueError('Invalid hierarchy node metadata')
        if (kind == 2 and mesh != 0xffffffff and mesh >= mesh_count) or (kind != 2 and mesh != 0xffffffff):
            raise ValueError('Invalid hierarchy mesh reference')
        if not all(math.isfinite(v) for row in (translation, quaternion, rotation, scale, auxiliary) for v in row):
            raise ValueError('Nonfinite hierarchy transform')
        children = [reader.u32() for _ in range(child_count)]
        nodes.append(dict(name=name, kind=kind, mesh=mesh, translation=translation,
                          quaternion=quaternion, skin=skin, rotation=rotation,
                          scale=scale, auxiliary=auxiliary, children=children))
    reader.done()
    parents = [None] * count
    for index, node in enumerate(nodes):
        for child in node['children']:
            if child >= count or child == index or parents[child] is not None:
                raise ValueError('Invalid or multiply-parented hierarchy child')
            parents[child] = index
    roots = [index for index, parent in enumerate(parents) if parent is None]
    pending = roots.copy()
    visited = 0
    while pending:
        visited += 1
        pending.extend(nodes[pending.pop()]['children'])
    if visited != count:
        raise ValueError('Hierarchy contains a cycle')
    return {'nodes': nodes, 'roots': roots}


def parse(data):
    reader = Reader(data)
    magic, version, flags = reader.unpack('<4sII')
    if magic != b'MDL ' or version != 4:
        raise ValueError('Only uncompressed MDL version 4 is verified by this exporter')
    sections = {}
    while True:
        kind = reader.u32()
        if kind == 0xffffffff:
            reader.done()
            break
        if kind in sections:
            raise ValueError('Duplicate MDL section')
        sections[kind] = reader.take(reader.u32())
    if 1 not in sections or 4 not in sections:
        raise ValueError('MDL has no mesh metadata or stream section')
    metadata = Reader(sections[1])
    mesh_count = metadata.u32()
    if mesh_count > 10000:
        raise ValueError('Too many meshes')
    meshes = []
    for _ in range(mesh_count):
        name = metadata.name()
        record = Reader(metadata.take(metadata.u32()))
        submesh_count = record.u32()
        if submesh_count > 10000:
            raise ValueError('Too many submeshes')
        submeshes = []
        for _ in range(submesh_count):
            material, index_count, unknown = record.unpack('<III')
            submeshes.append({'material': material, 'index_count': index_count,
                              'unknown': unknown, 'streams': []})
        bone_count = record.u32()
        if bone_count > 10000:
            raise ValueError('Too many skin bindings')
        bones = []
        for _ in range(bone_count):
            bone_name = record.name()
            matrix = record.unpack('<16f')
            if not all(math.isfinite(v) for v in matrix):
                raise ValueError('Nonfinite skin matrix')
            bones.append({'name': bone_name, 'matrix': matrix})
        record.done()
        collision = metadata.take(metadata.u32())
        meshes.append({'name': name, 'submeshes': submeshes, 'skin_bindings': bones,
                       'collision_bytes': len(collision)})
    metadata.done()
    streams = Reader(sections[4])
    count = streams.u32()
    if count > 100000:
        raise ValueError('Too many vertex streams')
    headers = [streams.unpack('<IIIII') for _ in range(count)]
    for kind, size, stride, mesh, submesh in headers:
        if not stride or size % stride or mesh >= len(meshes) or submesh >= len(meshes[mesh]['submeshes']):
            raise ValueError('Invalid stream stride, size or mesh reference')
        payload = streams.take(size)
        meshes[mesh]['submeshes'][submesh]['streams'].append({'kind': kind, 'stride': stride,
                                                            'count': size // stride, 'data': payload})
    streams.done()
    material_records = materials(sections[0]) if 0 in sections else None
    if material_records is not None and any(submesh['material'] >= len(material_records)
                                            for mesh in meshes for submesh in mesh['submeshes']):
        raise ValueError('Invalid primitive material reference')
    return {'version': version, 'flags': flags, 'meshes': meshes,
            'materials': material_records,
            'hierarchy': hierarchy(sections[2], len(meshes)) if 2 in sections else None,
            'sections': {kind: len(value) for kind, value in sections.items()},
            'source_sha256': hashlib.sha256(data).hexdigest()}


def inspection(model):
    """Copy parsed metadata without guessing unknown transforms or shader state."""
    meshes = []
    for mesh in model['meshes']:
        primitives = []
        for primitive in mesh['submeshes']:
            streams = [dict(kind=stream['kind'], stride=stream['stride'], count=stream['count'],
                            byte_length=len(stream['data']), sha256=hashlib.sha256(stream['data']).hexdigest())
                       for stream in primitive['streams']]
            primitives.append(dict(material=primitive['material'], index_count=primitive['index_count'],
                                   raw_unknown=primitive['unknown'], streams=streams))
        meshes.append(dict(name=mesh['name'], collision_bytes=mesh['collision_bytes'],
                           skin_bindings=[dict(name=bone['name'], raw_matrix=list(bone['matrix']))
                                          for bone in mesh['skin_bindings']], submeshes=primitives))
    tree = model.get('hierarchy')
    nodes = [dict(index=index, name=node['name'], kind=node['kind'], mesh_index=node['mesh'],
                  translation=list(node['translation']), raw_vector4=list(node['quaternion']),
                  raw_skin_flag=node['skin'], raw_rotation3=list(node['rotation']), scale=list(node['scale']),
                  raw_auxiliary3=list(node['auxiliary']), children=node['children'].copy())
             for index, node in enumerate(tree['nodes'])] if tree else []
    return dict(format_version=1, mdl_version=model['version'], raw_flags=model['flags'],
                source_sha256=model['source_sha256'], section_sizes=model['sections'].copy(),
                materials=copy.deepcopy(model.get('materials')), meshes=meshes,
                hierarchy=dict(roots=tree['roots'].copy(), nodes=nodes) if tree else None,
                limitations=['Raw node vectors and skin matrices are preserved without interpreting unknown transform composition.',
                             'Stream payloads remain in the source MDL; descriptors and hashes identify them.',
                             'Animation chunks, collision structures and full shader semantics are not decoded.'])


def geometry(submesh):
    streams = submesh['streams']
    positions = [s for s in streams if s['kind'] == 0]
    indices = [s for s in streams if s['kind'] == 7]
    if len(positions) != 1 or positions[0]['stride'] != 12 or len(indices) != 1 or indices[0]['stride'] != 4:
        raise ValueError('Primitive needs one verified float3 position and uint32 index stream')
    position, index = positions[0], indices[0]
    if not position['count'] or not index['count'] or index['count'] != submesh['index_count'] or index['count'] % 3:
        raise ValueError('Invalid triangle count')
    points = list(struct.iter_unpack('<3f', position['data']))
    if not all(math.isfinite(v) for p in points for v in p):
        raise ValueError('Nonfinite vertex position')
    if any(i[0] >= len(points) for i in struct.iter_unpack('<I', index['data'])):
        raise ValueError('Index is outside the vertex stream')
    for stream in streams:
        if stream['kind'] != 7 and stream['count'] != len(points):
            raise ValueError('Vertex stream counts disagree')
    return position, index, points


def png_image(data):
    if len(data) > 64 * 1024 * 1024 or data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Expected a bounded PNG texture')
    reader = Reader(data[8:])
    first, pixels, ended = True, False, False
    while reader.offset < len(reader.data):
        size = reader.unpack('>I')[0]
        kind = bytes(reader.take(4))
        payload = bytes(reader.take(size))
        crc = reader.unpack('>I')[0]
        if zlib.crc32(kind + payload) != crc:
            raise ValueError('Invalid PNG chunk checksum')
        if first:
            if kind != b'IHDR' or len(payload) != 13:
                raise ValueError('Invalid PNG header')
            width, height, bits, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            if (not width or not height or width > 8192 or height > 8192 or width * height > 16 * 1024 * 1024 or
                    bits != 8 or color not in (2, 6) or compression or filtering or interlace not in (0, 1)):
                raise ValueError('Unsupported PNG texture dimensions or encoding')
            first = False
        elif kind == b'IHDR':
            raise ValueError('Duplicate PNG header')
        if kind == b'IDAT':
            pixels = True
        if kind == b'IEND':
            if payload or not pixels:
                raise ValueError('Invalid PNG end marker')
            reader.done(); ended = True; break
    if not ended:
        raise ValueError('Truncated PNG texture')
    return data


def static_transform(source):
    """MDL v4 DAG: T(t+p) R(EulerXYZ * quaternion) S T(-p)."""
    vectors = [source[key] for key in ('translation', 'quaternion', 'rotation', 'scale', 'auxiliary')]
    if not all(math.isfinite(value) for vector in vectors for value in vector):
        raise ValueError('Nonfinite hierarchy transform')
    raw = source['quaternion']
    norm = sum(value * value for value in raw)
    if abs(norm - 1) > 1e-5:
        raise ValueError('Hierarchy rotation requires a unit quaternion')
    sx, sy, sz = [math.sin(angle / 2) for angle in source['rotation']]
    cx, cy, cz = [math.cos(angle / 2) for angle in source['rotation']]
    # Column-vector Euler matrix is Rz * Ry * Rx; the native quaternion helper
    # multiplies this Euler quaternion on the left of the file quaternion.
    a, b, c, d = (sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
                  cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz)
    e, f, g, h = raw
    q = (d*e+a*h+b*g-c*f, d*f+b*h+c*e-a*g,
         d*g+c*h+a*f-b*e, d*h-a*e-b*f-c*g)
    length = math.sqrt(sum(value * value for value in q))
    x, y, z, w = (value / length for value in q)
    rotation = ((1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)),
                (2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)),
                (2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)))
    pivot = source['auxiliary']
    scaled_pivot = [pivot[i] * source['scale'][i] for i in range(3)]
    translation = [source['translation'][i] + pivot[i] -
                   sum(rotation[i][j] * scaled_pivot[j] for j in range(3)) for i in range(3)]
    if not all(math.isfinite(value) for value in translation):
        raise ValueError('Nonfinite composed hierarchy transform')
    return translation, [x, y, z, w]


def matrix_product(a, b):
    return [sum(a[k*4+r] * b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]


def inverse_bind(matrix):
    if len(matrix) != 16 or not all(math.isfinite(v) for v in matrix):
        raise ValueError('Nonfinite or invalid skin binding matrix')
    if any(abs(matrix[i] - value) > 1e-6 for i, value in ((3, 0), (7, 0), (11, 0), (15, 1))):
        raise ValueError('Skin binding matrix must be affine')
    matrix = list(matrix)
    for i, value in ((3, 0), (7, 0), (11, 0), (15, 1)):
        matrix[i] = value
    rows = [[matrix[c*4+r] for c in range(4)] + [float(r == c) for c in range(4)] for r in range(4)]
    for c in range(4):
        pivot = max(range(c, 4), key=lambda r: abs(rows[r][c]))
        if abs(rows[pivot][c]) <= 1e-12:
            raise ValueError('Singular skin binding matrix')
        rows[c], rows[pivot] = rows[pivot], rows[c]
        divisor = rows[c][c]
        rows[c] = [v / divisor for v in rows[c]]
        for r in range(4):
            if r != c:
                factor = rows[r][c]
                rows[r] = [a-factor*b for a, b in zip(rows[r], rows[c])]
    result = [rows[r][c+4] for c in range(4) for r in range(4)]
    for i, value in ((3, 0), (7, 0), (11, 0), (15, 1)):
        result[i] = value
    if not all(math.isfinite(v) and abs(v) <= 3.4028234663852886e38 for v in result):
        raise ValueError('Skin inverse matrix exceeds finite float storage')
    return result


def hierarchy_world(tree):
    identity = [float(r == c) for c in range(4) for r in range(4)]
    world = {}
    pending = [(i, identity) for i in tree['roots']]
    while pending:
        i, parent = pending.pop()
        if i in world:
            raise ValueError('Repeated or cyclic skin hierarchy')
        source = tree['nodes'][i]
        t, (x, y, z, w) = static_transform(source)
        a, b, c = source['scale']
        local = [(1-2*(y*y+z*z))*a, 2*(x*y+z*w)*a, 2*(x*z-y*w)*a, 0,
                 2*(x*y-z*w)*b, (1-2*(x*x+z*z))*b, 2*(y*z+x*w)*b, 0,
                 2*(x*z+y*w)*c, 2*(y*z-x*w)*c, (1-2*(x*x+y*y))*c, 0, *t, 1]
        world[i] = matrix_product(parent, local)
        if not all(math.isfinite(v) for v in world[i]):
            raise ValueError('Nonfinite composed skin hierarchy')
        pending.extend((child, world[i]) for child in source['children'])
    if len(world) != len(tree['nodes']):
        raise ValueError('Unreachable skin hierarchy node')
    return world


def skin_streams(submesh, joint_count, vertex_count):
    weights = [s for s in submesh['streams'] if s['kind'] == 5]
    joints = [s for s in submesh['streams'] if s['kind'] == 6]
    if len(weights) != 1 or len(joints) != 1 or not joint_count:
        raise ValueError('Skin requires one weight and one joint stream with bindings')
    weights, joints = weights[0], joints[0]
    if any(s['stride'] != 16 or s['count'] != vertex_count or len(s['data']) != vertex_count*16 for s in (weights, joints)):
        raise ValueError('Unsupported or mismatched skin stream encoding')
    joint_data, weight_data = bytearray(), bytearray()
    for values, indices in zip(struct.iter_unpack('<4f', weights['data']), struct.iter_unpack('<4I', joints['data'])):
        if any(i >= joint_count or i > 65535 for i in indices):
            raise ValueError('Skin joint index exceeds binding table')
        if any(not math.isfinite(v) or v < -1e-8 or v > 1+1e-6 for v in values):
            raise ValueError('Nonfinite or out-of-range skin weight')
        values = [max(0, v) for v in values]
        total = sum(values)
        if abs(total-1) > 1e-5:
            raise ValueError('Skin weights must sum to one')
        weight_data.extend(struct.pack('<4f', *(v/total for v in values)))
        joint_data.extend(struct.pack('<4H', *(index if weight > 0 else 0 for index, weight in zip(indices, values))))
    return joint_data, weight_data


def gltf(model, bind_pose=False, preserve_hierarchy=False, base_color_directory=None, skinning=False, vertex_colors=False):
    if skinning and bind_pose:
        raise ValueError('Skin export cannot be combined with discarded bind-pose skinning')
    preserve_hierarchy = preserve_hierarchy or skinning
    output = {'asset': {'version': '2.0', 'generator': 'FDKss MDL v4 static exporter'},
              'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [],
              'buffers': [], 'bufferViews': [], 'accessors': [],
              'extras': {'source_sha256': model['source_sha256'],
                         'scope': 'Static primitives in engine local coordinates; hierarchy, animations and shader materials are not reconstructed.'}}
    binary = bytearray()
    if vertex_colors:
        output['extras']['vertex_color_scope'] = 'Packed MDL v4 color bytes converted to normalized RGBA; glTF multiplies base color, while FDK shader switches may ignore or interpret vertex colors differently.'
    if model.get('materials'):
        output['materials'] = [{'name': material['name'],
                                'pbrMetallicRoughness': {'metallicFactor': 0, 'roughnessFactor': 1},
                                'extras': {'fdk_material': material}}
                               for material in model['materials']]
    if base_color_directory is not None:
        if not model.get('materials'):
            raise ValueError('Base-color export requires material metadata')
        directory = Path(base_color_directory).resolve()
        total = 0
        for index, material in enumerate(model['materials']):
            if material['shader'] != 'map':
                raise ValueError('Base-color approximation supports only the map shader')
            diffuse = [t for t in material['textures'] if t['slot'] == 0]
            if len(diffuse) != 1:
                raise ValueError('Expected one map diffuse texture in slot 0')
            texture = diffuse[0]; name = texture['name']
            if not name or any(c in name for c in '/\\:') or name in ('.', '..'):
                raise ValueError('Unsafe texture filename')
            path = (directory / (name + '.png')).resolve()
            if path.parent != directory or path.stat().st_size > 64 * 1024 * 1024:
                raise ValueError('Texture file exceeds the directory or byte limit')
            data = png_image(path.read_bytes()); total += len(data)
            if total > 128 * 1024 * 1024:
                raise ValueError('Embedded textures exceed the byte limit')
            wraps = {0: 10497, 1: 33648, 2: 33071}
            if texture['wrap_u'] not in wraps or texture['wrap_v'] not in wraps:
                raise ValueError('Unsupported texture wrap mode')
            if not material['uv_maps'] or material['uv_maps'][0] >= 8:
                raise ValueError('Unsupported diffuse UV binding')
            uv = material['uv_maps'][0]
            output.setdefault('images', []).append({'name': name, 'uri': 'data:image/png;base64,' + base64.b64encode(data).decode()})
            output.setdefault('samplers', []).append({'wrapS': wraps[texture['wrap_u']], 'wrapT': wraps[texture['wrap_v']]})
            output.setdefault('textures', []).append({'source': index, 'sampler': index})
            output['materials'][index]['pbrMetallicRoughness']['baseColorTexture'] = {'index': index, 'texCoord': uv}
        output['extras']['base_color_scope'] = 'Map shader slot-0 diffuse approximation; FDK lighting, blending, masks, normal maps and toon shading are not reproduced.'
    def accessor(data, kind, component, count, target, minimum=None, maximum=None, normalized=False):
        binary.extend(b'\0' * (-len(binary) % 4))
        offset = len(binary)
        binary.extend(data)
        view = len(output['bufferViews'])
        output['bufferViews'].append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data)})
        if target is not None:
            output['bufferViews'][-1]['target'] = target
        result = {'bufferView': view, 'componentType': component, 'count': count, 'type': kind}
        if normalized:
            result['normalized'] = True
        if minimum is not None:
            result.update(min=minimum, max=maximum)
        output['accessors'].append(result)
        return len(output['accessors']) - 1
    mesh_indices = {}
    for source_index, mesh in enumerate(model['meshes']):
        if (mesh['skin_bindings'] or any(s['kind'] in (5, 6) for p in mesh['submeshes'] for s in p['streams'])) and not (bind_pose or skinning):
            raise ValueError('Skinned meshes require --bind-pose; animation and skin reconstruction are incomplete')
        primitives = []
        for submesh in mesh['submeshes']:
            position, index, points = geometry(submesh)
            low = [min(p[a] for p in points) for a in range(3)]
            high = [max(p[a] for p in points) for a in range(3)]
            attributes = {'POSITION': accessor(position['data'], 'VEC3', 5126, len(points), 34962, low, high)}
            if vertex_colors:
                colors = [s for s in submesh['streams'] if s['kind'] == 3]
                if len(colors) > 1:
                    raise ValueError('Duplicate vertex color stream')
                if colors:
                    color = colors[0]
                    # MDL v4 primitive word -> SubMesh+148 -> renderer flags.
                    # Bit 0 takes priority (BGRA); otherwise bit 1 selects RGBA.
                    flags = submesh['unknown']
                    if color['stride'] != 4 or not flags & 3:
                        raise ValueError('Unsupported vertex color encoding')
                    if color['count'] != len(points) or len(color['data']) != len(points) * 4:
                        raise ValueError('Invalid vertex color payload size')
                    payload = bytes(color['data'])
                    if flags & 1:
                        payload = bytes(v for b, g, r, a in struct.iter_unpack('<4B', payload) for v in (r, g, b, a))
                    attributes['COLOR_0'] = accessor(payload, 'VEC4', 5121, len(points), 34962, normalized=True)
            if skinning and (mesh['skin_bindings'] or any(s['kind'] in (5, 6) for s in submesh['streams'])):
                joints, weights = skin_streams(submesh, len(mesh['skin_bindings']), len(points))
                attributes['JOINTS_0'] = accessor(joints, 'VEC4', 5123, len(points), 34962)
                attributes['WEIGHTS_0'] = accessor(weights, 'VEC4', 5126, len(points), 34962)
            texcoord = 0
            for stream in submesh['streams']:
                if stream['kind'] == 1:
                    if 'NORMAL' in attributes:
                        raise ValueError('Duplicate normal stream')
                    if stream['stride'] == 12:
                        normals = bytes(stream['data'])
                    elif stream['stride'] == 4:
                        normals = b''.join(struct.pack('<3f', *(max(-1.0, n / 127) for n in row[:3]))
                                           for row in struct.iter_unpack('<4b', stream['data']))
                    else:
                        raise ValueError('Unsupported normal encoding')
                    normalized = bytearray()
                    for row in struct.iter_unpack('<3f', normals):
                        length = math.hypot(*row)
                        if not math.isfinite(length) or length <= 1e-12:
                            raise ValueError('Nonfinite or zero normal')
                        normalized.extend(struct.pack('<3f', *(v / length for v in row)))
                    normals = normalized
                    attributes['NORMAL'] = accessor(normals, 'VEC3', 5126, len(points), 34962)
                elif stream['kind'] == 4:
                    if stream['stride'] != 8 or texcoord >= 8:
                        raise ValueError('Unsupported UV stream')
                    if not all(math.isfinite(v[0]) for v in struct.iter_unpack('<f', stream['data'])):
                        raise ValueError('Nonfinite texture coordinate')
                    attributes[f'TEXCOORD_{texcoord}'] = accessor(stream['data'], 'VEC2', 5126, len(points), 34962)
                    texcoord += 1
            primitives.append({'attributes': attributes, 'indices': accessor(index['data'], 'SCALAR', 5125, index['count'], 34963),
                               'mode': 4, 'extras': {'fdk_material_index': submesh['material']}})
            if model.get('materials'):
                primitives[-1]['material'] = submesh['material']
                binding = output['materials'][submesh['material']]['pbrMetallicRoughness'].get('baseColorTexture')
                if binding and f'TEXCOORD_{binding["texCoord"]}' not in attributes:
                    raise ValueError('Diffuse material references a missing UV stream')
        if not primitives:
            continue
        mesh_index = len(output['meshes'])
        mesh_indices[source_index] = mesh_index
        output['meshes'].append({'name': mesh['name'], 'primitives': primitives})
        output['scenes'][0]['nodes'].append(len(output['nodes']))
        output['nodes'].append({'name': mesh['name'], 'mesh': mesh_index})
    if not output['meshes']:
        raise ValueError('No exportable geometry')
    if preserve_hierarchy:
        tree = model.get('hierarchy')
        if not tree:
            raise ValueError('Model has no hierarchy section')
        nodes = []
        for source in tree['nodes']:
            if not skinning and (source['kind'] == 1 or source['skin']):
                raise ValueError('Hierarchy export requires static nodes without skin or animation nodes')
            translation, quaternion = static_transform(source)
            node = {'name': source['name'], 'translation': translation,
                    'scale': list(source['scale'])}
            if quaternion != [0, 0, 0, 1]:
                node['rotation'] = quaternion
            if source['children']:
                node['children'] = source['children'].copy()
            if source['mesh'] != 0xffffffff:
                if source['mesh'] in mesh_indices:
                    node['mesh'] = mesh_indices[source['mesh']]
                else:
                    # Collision-only/empty source meshes have no glTF primitive,
                    # but their hierarchy nodes still carry transforms and children.
                    node['extras'] = {'fdk_empty_mesh_index': source['mesh']}
            nodes.append(node)
        if not set(mesh_indices) <= {node['mesh'] for node in tree['nodes'] if node['mesh'] != 0xffffffff}:
            raise ValueError('Hierarchy does not reference every exported mesh')
        output['nodes'] = nodes
        output['scenes'][0]['nodes'] = tree['roots'].copy()
        output['extras']['scope'] = 'Static hierarchy with translation, rotation, scale and pivot composition; animations and shader materials are not reconstructed.'
        if skinning:
            names = {n['name']: i for i, n in enumerate(tree['nodes'])}
            if len(names) != len(tree['nodes']):
                raise ValueError('Skin hierarchy requires unique node names')
            world = hierarchy_world(tree)
            for source_index, mesh in enumerate(model['meshes']):
                if not mesh['skin_bindings']:
                    continue
                users = [i for i, n in enumerate(tree['nodes']) if n['mesh'] == source_index]
                if len(users) != 1 or source_index not in mesh_indices:
                    raise ValueError('Skinned mesh requires exactly one exported hierarchy node')
                user = users[0]
                identity = [float(r == c) for c in range(4) for r in range(4)]
                if tree['nodes'][user]['children'] or any(abs(a-b)>1e-5 for a,b in zip(world[user],identity)):
                    raise ValueError('Skinned mesh requires an identity bind transform without children')
                active = set()
                primitives = output['meshes'][mesh_indices[source_index]]['primitives']
                for primitive in primitives:
                    ja = output['accessors'][primitive['attributes']['JOINTS_0']]
                    wa = output['accessors'][primitive['attributes']['WEIGHTS_0']]
                    jv = output['bufferViews'][ja['bufferView']]; wv = output['bufferViews'][wa['bufferView']]
                    for j, w in zip(struct.iter_unpack('<4H', binary[jv['byteOffset']:jv['byteOffset']+jv['byteLength']]),
                                    struct.iter_unpack('<4f', binary[wv['byteOffset']:wv['byteOffset']+wv['byteLength']])):
                        active.update(index for index, weight in zip(j, w) if weight > 0)
                joint_indices, inverses, remap, omitted = [], bytearray(), {}, []
                binding_names = [b['name'] for b in mesh['skin_bindings']]
                if len(set(binding_names)) != len(binding_names):
                    raise ValueError('Duplicate skin binding name')
                for original_index, binding in enumerate(mesh['skin_bindings']):
                    joint = names.get(binding['name'])
                    if joint is None and original_index not in active:
                        omitted.append(binding['name'])
                        continue
                    if joint is None or tree['nodes'][joint]['kind'] not in (0, 1) or joint in joint_indices:
                        raise ValueError('Skin binding requires a unique joint node')
                    matrix = binding['matrix']
                    inverse = inverse_bind(matrix)
                    tolerance = 2e-5 * max(1, *(abs(v) for v in matrix))
                    if any(abs(a-b) > tolerance for a, b in zip(world[joint], matrix)):
                        raise ValueError('Skin binding does not match the hierarchy bind transform')
                    remap[original_index] = len(joint_indices)
                    joint_indices.append(joint)
                    inverses.extend(struct.pack('<16f', *inverse))
                skin = {'name': mesh['name'], 'joints': joint_indices,
                        'inverseBindMatrices': accessor(inverses, 'MAT4', 5126, len(joint_indices), None)}
                if omitted:
                    skin['extras'] = {'fdk_unused_missing_bindings': omitted}
                for primitive in primitives:
                    ja = output['accessors'][primitive['attributes']['JOINTS_0']]
                    wa = output['accessors'][primitive['attributes']['WEIGHTS_0']]
                    jv = output['bufferViews'][ja['bufferView']]; wv = output['bufferViews'][wa['bufferView']]
                    rewritten = bytearray()
                    for j, w in zip(struct.iter_unpack('<4H', binary[jv['byteOffset']:jv['byteOffset']+jv['byteLength']]),
                                    struct.iter_unpack('<4f', binary[wv['byteOffset']:wv['byteOffset']+wv['byteLength']])):
                        rewritten.extend(struct.pack('<4H', *(remap[index] if weight > 0 else 0 for index, weight in zip(j, w))))
                    binary[jv['byteOffset']:jv['byteOffset']+jv['byteLength']] = rewritten
                output.setdefault('skins', []).append(skin)
                nodes[user]['skin'] = len(output['skins'])-1
                # glTF skin matrices already place vertices through the joint
                # hierarchy; skinned mesh nodes must not inherit that hierarchy.
                for parent in nodes:
                    if user in parent.get('children', []):
                        parent['children'].remove(user)
                        if not parent['children']:
                            del parent['children']
                if user not in output['scenes'][0]['nodes']:
                    output['scenes'][0]['nodes'].append(user)
                for key in ('translation', 'rotation', 'scale'):
                    nodes[user].pop(key, None)
            output['extras']['scope'] = 'Skinned bind hierarchy with joint weights and inverse bind matrices; animation clips and shader materials are not reconstructed.'
    output['buffers'].append({'byteLength': len(binary), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(binary).decode()})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('model', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inspect', action='store_true', help='Write raw model/hierarchy metadata JSON without geometry conversion')
    parser.add_argument('--bind-pose', action='store_true')
    parser.add_argument('--skin', action='store_true', help='Export skinned bind hierarchy, joint weights and inverse bind matrices')
    parser.add_argument('--hierarchy', action='store_true', help='Preserve static hierarchy with rotation, scale and pivot composition')
    parser.add_argument('--base-color-dir', type=Path, help='Embed converted PNG slot-0 diffuse textures for map shaders')
    parser.add_argument('--vertex-colors', action='store_true', help='Preserve verified packed vertex colors as normalized RGBA; shader use is approximate')
    args = parser.parse_args()
    if args.inspect and (args.bind_pose or args.skin or args.hierarchy or args.base_color_dir is not None or args.vertex_colors):
        parser.error('--inspect cannot be combined with geometry conversion options')
    if args.output.resolve() == args.model.resolve() or args.output.exists() and args.output.samefile(args.model):
        parser.error('glTF output cannot replace the input model')
    model = parse(args.model.read_bytes())
    result = inspection(model) if args.inspect else gltf(model, args.bind_pose, args.hierarchy, args.base_color_dir, args.skin, args.vertex_colors)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, separators=(',', ':'), allow_nan=False), encoding='utf-8')
    print(f"Exported {'metadata for ' if args.inspect else ''}{len(result['meshes'])} meshes to {args.output}")


if __name__ == '__main__':
    main()
