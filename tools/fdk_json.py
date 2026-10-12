"""Decode and encode the verified FDK binary JSON node types used by Sky scenes."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

MAX_SIZE = 128 * 1024 * 1024
MAX_NODES = 1_000_000
MAX_DEPTH = 128


def decode(data):
    if len(data) < 21 or len(data) > MAX_SIZE:
        raise ValueError('Invalid binary JSON size')
    magic, reserved, root, flags = struct.unpack_from('<4sIII', data)
    if magic != b'JSON' or reserved or flags or not 16 <= root < len(data):
        raise ValueError('Unsupported binary JSON header')
    names, active = {}, set()
    count = 0

    def unpack(format_, offset):
        size = struct.calcsize(format_)
        if offset < 0 or size > len(data) - offset:
            raise ValueError('Binary JSON value outside file')
        return struct.unpack_from(format_, data, offset)

    def string(offset, limit):
        if offset < 0 or offset >= limit:
            raise ValueError('Binary JSON string outside its section')
        end = data.find(b'\0', offset, limit)
        if end < 0:
            raise ValueError('Unterminated binary JSON string')
        return data[offset:end].decode('utf-8')

    def property_name(offset):
        if offset in names:
            return names[offset]
        if not 16 <= offset <= root - 5:
            raise ValueError('Binary JSON dictionary pointer outside dictionary')
        expected, = unpack('<I', offset)
        name = string(offset + 4, root)
        if (zlib.crc32(name.encode('utf-8')) ^ 0xFFFFFFFF) != expected:
            raise ValueError('Binary JSON property checksum mismatch')
        names[offset] = name
        return name

    def node(offset, depth):
        nonlocal count
        count += 1
        if count > MAX_NODES or depth > MAX_DEPTH or offset in active:
            raise ValueError('Binary JSON node budget, depth or cycle limit exceeded')
        if not root <= offset < len(data):
            raise ValueError('Binary JSON node pointer outside data section')
        active.add(offset)
        tag = data[offset]
        if tag not in (2, 3, 4, 5, 6, 0x12, 0x13, 0x14, 0x15, 0x16):
            raise ValueError(f'Unsupported binary JSON node type 0x{tag:x}')
        cursor, name = offset + 1, None
        if tag < 0x10:
            dictionary, = unpack('<I', cursor)
            name = property_name(dictionary)
            cursor += 4
        kind = tag & 0x0F
        if kind == 2:
            result = string(cursor, len(data))
        elif kind == 3:
            result, = unpack('<d', cursor)
            if not math.isfinite(result):
                raise ValueError('Nonfinite binary JSON number')
        elif kind == 6:
            boolean, = unpack('<B', cursor)
            if boolean not in (0, 1):
                raise ValueError('Invalid binary JSON boolean')
            result = bool(boolean)
        else:
            size, = unpack('<I', cursor)
            if size > MAX_NODES or size > (len(data) - cursor - 4) // 4:
                raise ValueError('Invalid binary JSON child array bounds')
            children = unpack('<' + str(size) + 'I', cursor + 4)
            result = {} if kind == 4 else []
            for child in children:
                key, value = node(child, depth + 1)
                if kind == 4:
                    if key is None or key in result:
                        raise ValueError('Object child is unnamed or duplicates a key')
                    result[key] = value
                else:
                    if key is not None:
                        raise ValueError('Array child unexpectedly has a property name')
                    result.append(value)
        active.remove(offset)
        return name, result

    _, result = node(root, 0)
    return result


def encode(value):
    names = {'': None}
    count = 0

    def validate(item, depth):
        nonlocal count
        count += 1
        if count > MAX_NODES or depth > MAX_DEPTH:
            raise ValueError('Binary JSON node or depth limit exceeded')
        if isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str) or '\0' in key:
                    raise ValueError('Object keys must be strings without NUL bytes')
                names.setdefault(key, None)
                validate(child, depth + 1)
        elif isinstance(item, list):
            for child in item:
                validate(child, depth + 1)
        elif isinstance(item, str):
            if '\0' in item:
                raise ValueError('FDK binary JSON cannot represent a NUL inside a string')
        elif isinstance(item, bool):
            pass
        elif isinstance(item, (int, float)):
            try:
                number = float(item)
            except OverflowError as exception:
                raise ValueError('Number exceeds FDK double representation') from exception
            if not math.isfinite(number) or (isinstance(item, int) and int(number) != item):
                raise ValueError('Number cannot be represented exactly as a finite FDK double')
        else:
            raise ValueError('Unsupported JSON value; null nodes are not yet verified')

    validate(value, 0)
    output = bytearray(struct.pack('<4sIII', b'JSON', 0, 0, 0))
    for name in names:
        raw = name.encode('utf-8')
        names[name] = len(output)
        output.extend(struct.pack('<I', zlib.crc32(raw) ^ 0xFFFFFFFF) + raw + b'\0')
        if len(output) > MAX_SIZE:
            raise ValueError('Binary JSON size limit exceeded')
    root = len(output)

    def write(item, name):
        offset = len(output)
        kind = 4 if isinstance(item, dict) else 5 if isinstance(item, list) else 2 if isinstance(item, str) else 6 if isinstance(item, bool) else 3
        output.append(kind if name is not None else kind + 0x10)
        if name is not None:
            output.extend(struct.pack('<I', names[name]))
        if kind in (4, 5):
            output.extend(struct.pack('<I', len(item)))
            slots = len(output)
            output.extend(b'\0' * (4 * len(item)))
            children = item.items() if kind == 4 else ((None, child) for child in item)
            for index, (key, child) in enumerate(children):
                child_offset = write(child, key)
                struct.pack_into('<I', output, slots + index * 4, child_offset)
        elif kind == 2:
            output.extend(item.encode('utf-8') + b'\0')
        elif kind == 6:
            output.append(int(item))
        else:
            output.extend(struct.pack('<d', float(item)))
        if len(output) > MAX_SIZE:
            raise ValueError('Binary JSON size limit exceeded')
        return offset

    write(value, '')
    struct.pack_into('<I', output, 8, root)
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('decode', 'encode'))
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve() or (args.output.exists() and args.output.samefile(args.source)):
        parser.error('Source and output must be different files')
    if args.operation == 'decode':
        value = decode(args.source.read_bytes())
        data = (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')
    else:
        data = encode(json.loads(args.source.read_text(encoding='utf-8')))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f'{args.operation}: wrote {len(data)} bytes to {args.output}')


if __name__ == '__main__':
    main()
