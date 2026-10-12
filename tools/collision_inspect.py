"""Bounded offline MDL v4 collision-tree comparison; no native query acceptance."""
import hashlib
import math
import struct

from mdl import Reader


def payloads(data):
    reader = Reader(data)
    if reader.unpack('<4sII')[:2] != (b'MDL ', 4):
        raise ValueError('Expected uncompressed MDL v4')
    sections = {}
    while True:
        kind = reader.u32()
        if kind == 0xffffffff:
            reader.done()
            break
        if kind in sections:
            raise ValueError('Duplicate MDL section')
        sections[kind] = reader.take(reader.u32())
    metadata = Reader(sections[1])
    count = metadata.u32()
    if count > 10000:
        raise ValueError('Collision mesh count budget exceeded')
    output = []
    for _ in range(count):
        name = metadata.name()
        metadata.take(metadata.u32())
        output.append((name, bytes(metadata.take(metadata.u32()))))
    metadata.done()
    return output


def inspect(data):
    r = Reader(data)
    minimum = r.unpack('<3f'); unknown0 = r.u32()
    maximum = r.unpack('<3f'); unknown1 = r.u32()
    triangles = r.u32()
    if triangles > 1000000:
        raise ValueError('Collision triangle budget exceeded')
    raw_triangles = bytes(r.take(triangles * 64))
    triangle_rows = list(struct.iter_unpack('<16f', raw_triangles))
    if any(not math.isfinite(v) for row in triangle_rows for v in row):
        raise ValueError('Nonfinite collision triangle')
    count = r.u32()
    if count > 1000000:
        raise ValueError('Collision node budget exceeded')
    nodes = []
    references = 0
    for _ in range(count):
        bounds = r.unpack('<6f')
        start, end, n = r.unpack('<iiI')
        references += n
        if references > 1000000 or n > triangles:
            raise ValueError('Collision reference budget exceeded')
        ids = r.unpack('<' + 'I' * n)
        if any(i >= triangles for i in ids) or any(not math.isfinite(v) for v in bounds):
            raise ValueError('Invalid collision node data')
        nodes.append(dict(bounds=bounds, start=start, end=end, triangles=ids))
    flags = r.u32()
    r.done()
    visited, active, hashes, leaves = set(), set(), {}, []
    child_references = 0
    pending = [(0, False)] if nodes else []
    while pending:
        index, finish = pending.pop()
        node = nodes[index]
        if finish:
            active.remove(index)
            children = [] if node['triangles'] else list(range(node['start'], node['end'] + 1))
            value = repr((node['bounds'], node['triangles'], tuple(hashes[i] for i in children)))
            hashes[index] = hashlib.sha256(value.encode()).hexdigest()
            continue
        if index in active or index in visited:
            raise ValueError('Collision tree cycle or shared child')
        visited.add(index); active.add(index)
        if any(a > b for a,b in zip(node['bounds'][:3], node['bounds'][3:])):
            raise ValueError('Invalid reachable collision bounds')
        pending.append((index, True))
        if node['triangles']:
            if (node['start'], node['end']) != (-1, -1):
                raise ValueError('Unsupported collision leaf encoding')
            leaves.append((node['bounds'], node['triangles']))
        else:
            start, end = node['start'], node['end']
            if not 0 <= start <= end < count or end - start > 1024:
                raise ValueError('Collision child range out of bounds')
            child_references += end - start + 1
            if child_references >= count:
                raise ValueError('Collision tree cycle or excessive child references')
            for child in range(end, start - 1, -1):
                pending.append((child, False))
    referenced = sorted(i for _, ids in leaves for i in ids)
    unused = [n for i,n in enumerate(nodes) if i not in visited]
    derived = {label: hashlib.sha256(b''.join(struct.pack('<'+'f'*(end-start), *row[start:end]) for row in triangle_rows)).hexdigest()
               for label,start,end in [('normals',9,12),('midpoints',12,15),('radii',15,16)]}
    return dict(bounds=[minimum, maximum], unknown_words=[unknown0,unknown1], flags=flags,
                triangle_count=triangles, triangle_bytes_sha256=hashlib.sha256(raw_triangles).hexdigest(),
                triangle_positions_sha256=hashlib.sha256(b''.join(struct.pack('<9f', *row[:9]) for row in triangle_rows)).hexdigest(),
                derived_triangle_hashes=derived,
                node_count=count, reachable_nodes=len(visited), unreachable_nodes=len(unused),
                unreachable_empty_nodes=sum(not n['triangles'] and n['start']==n['end']==0 and
                    all(a>b for a,b in zip(n['bounds'][:3],n['bounds'][3:])) for n in unused),
                referenced_triangle_ids=referenced, all_triangles_referenced=referenced==list(range(triangles)),
                canonical_tree_sha256=hashes.get(0),
                leaf_bounds_and_membership_sha256=hashlib.sha256(repr(sorted(leaves)).encode()).hexdigest())


def compare(original, rebuilt):
    a, b = payloads(original), payloads(rebuilt)
    if [name for name,_ in a] != [name for name,_ in b]:
        return {'status': 'mesh_names_differ', 'native_queries_verified': False}
    output = []
    for (name, x), (_, y) in zip(a,b):
        row = dict(mesh=name, raw_equal=x==y)
        try:
            left, right = inspect(x), inspect(y)
            row.update(status='decoded', original=left, rebuilt=right,
                       equivalent_rooted_tree=left['canonical_tree_sha256']==right['canonical_tree_sha256'],
                       equivalent_triangle_positions=left['triangle_positions_sha256']==right['triangle_positions_sha256'],
                       equivalent_leaf_bounds_and_membership=left['leaf_bounds_and_membership_sha256']==right['leaf_bounds_and_membership_sha256'])
        except ValueError as error:
            row.update(status='opaque_refused', reason=str(error))
        output.append(row)
    return dict(meshes=output, native_queries_verified=False,
                interpretation='Rooted tree and triangle checks use the observed peer layout; engine traversal and collision responses require live validation.')
