"""Compile bounded JSON assembly source into FDK #scp bytecode."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

OPS = dict(zip(('add', 'subtract', 'multiply', 'divide', 'remainder', 'equal',
                'not_equal', 'greater', 'greater_equal', 'less', 'less_equal',
                'bit_and', 'bit_or', 'and', 'or', 'negate', 'not', 'bit_not'), range(16, 34)))
BRANCHES = {'jump': 11, 'jump_if': 14, 'jump_unless': 15}


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'Expected integer in {low}..{high}')
    return value


def name(value):
    if not isinstance(value, str) or not value or '\0' in value or len(value.encode('utf-8')) > 255:
        raise ValueError('Names must contain 1..255 UTF-8 bytes without NUL')
    return value


def scalar(value, strings):
    if value is None:
        return 0
    if type(value) is int:
        return (integer(value, -536870912, 536870911) & 0x3fffffff) | 0x40000000
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError('Nonfinite float')
        try:
            bits = struct.unpack('<I', struct.pack('<f', value))[0]
        except OverflowError as exc:
            raise ValueError('Float exceeds VM range') from exc
        packed = (bits >> 2) | 0x80000000
        restored = struct.unpack('<f', struct.pack('<I', (packed << 2) & 0xffffffff))[0]
        if not math.isfinite(restored) or (value != 0 and abs(restored) < 2**-126):
            raise ValueError('Float underflow or unsupported subnormal')
        return packed
    if isinstance(value, str) and '\0' not in value and len(value.encode('utf-8')) <= 16383:
        return 0xc0000000 | strings(value)
    raise ValueError('Expected nil, signed 30-bit integer, finite float or bounded string')


def wait_duration(value):
    if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 30:
        raise ValueError('Wait duration must be finite and within 0..30')
    if type(value) is int:
        return scalar(value, lambda _: 0)
    bits = struct.unpack('<I', struct.pack('<f', value))[0]
    if value>0 and bits<0x00800000:
        bits=0x00800000 # Keep a positive normal target when the engine flushes denormals.
    if struct.unpack('<f', struct.pack('<I', bits))[0] < value:
        bits += 1
    return ((bits + 3) >> 2) | 0x80000000


def compile_source(source):
    if not isinstance(source, dict) or set(source) != {'format', 'functions'} or source['format'] != 'fdk-asm-1':
        raise ValueError('Expected fdk-asm-1 source with format and functions fields')
    functions = source['functions']
    if not isinstance(functions, list) or not 1 <= len(functions) <= 256:
        raise ValueError('Expected 1..256 functions')
    names = {}
    for index, fn in enumerate(functions):
        if not isinstance(fn, dict) or not {'name', 'code'} <= fn.keys() or fn.keys() - {'name', 'code', 'inputs', 'defaults'}:
            raise ValueError('Invalid function fields')
        fn_name = name(fn['name'])
        if fn_name in names:
            raise ValueError('Duplicate function name')
        names[fn_name] = index
        integer(fn.get('inputs', 0), 0, 32)
        defaults = fn.get('defaults', [])
        if not isinstance(defaults, list) or len(defaults) > fn.get('inputs', 0):
            raise ValueError('Defaults must cover a trailing subset of inputs')
        if not isinstance(fn['code'], list) or not 1 <= len(fn['code']) <= 4096:
            raise ValueError('Expected 1..4096 source instructions per function')
    data = bytearray(24 + 32 * len(functions))
    variables = len(data)
    pool = {}

    def strings(value):
        if value not in pool:
            encoded = value.encode('utf-8') + b'\0'
            if len(data) + len(encoded) > 1024 * 1024:
                raise ValueError('Compiled string pool exceeds 1 MiB')
            pool[value] = len(data)
            data.extend(encoded)
        return pool[value]

    records = []
    # Put all metadata/string data before executable code so instruction ranges
    # are contiguous and every code address refers to an instruction boundary.
    for fn in functions:
        inputs = fn.get('inputs', 0)
        input_table = len(data)
        data.extend(bytes(inputs * 4))
        defaults = fn.get('defaults', [])
        # Intern strings before reserving contiguous default DWORDs.
        raw_defaults = [scalar(v, strings) for v in defaults]
        default_table = len(data)
        data.extend(struct.pack(f'<{len(defaults)}I', *raw_defaults))
        records.append((inputs, input_table, defaults, default_table, strings(fn['name'])))
    for fn in functions:
        for instruction in fn['code']:
            if not isinstance(instruction, list) or not instruction or not isinstance(instruction[0], str):
                raise ValueError('Instructions must be nonempty arrays starting with an operation name')
            if instruction[0] == 'push' and len(instruction) == 2:
                scalar(instruction[1], strings)
            elif instruction[0] == 'call' and len(instruction) == 3 and isinstance(instruction[2], list):
                for arg in instruction[2]:
                    if not isinstance(arg, dict):
                        scalar(arg, strings)

    instruction_total = 0
    for index, fn in enumerate(functions):
        start = len(data)
        labels, patches, instructions = {}, [], []
        for ins in fn['code']:
            op, args = ins[0], ins[1:]
            at = len(data)
            effect, need, targets = 0, 0, []
            if op == 'label' and len(args) == 1:
                label = name(args[0])
                if label in labels:
                    raise ValueError('Duplicate label')
                labels[label] = at
                continue
            if op == 'push' and len(args) == 1:
                data.extend(struct.pack('<BBI', 0, 4, scalar(args[0], strings))); effect = 1
            elif op in ('load', 'save') and len(args) == 1:
                data.extend((9 if op == 'load' else 10, integer(args[0], 0, 15)))
                effect, need = (1, 0) if op == 'load' else (-1, 1)
            elif op == 'pop' and len(args) == 1:
                count = integer(args[0], 0, 63)
                data.extend((1, count * 4)); effect, need = -count, count
            elif op == 'copy' and len(args) == 1:
                relative = integer(args[0], -256, -4)
                if relative % 4:
                    raise ValueError('Copy offset must be a negative multiple of four')
                data.extend(struct.pack('<Bi', 2, relative)); effect, need = 1, -relative // 4
            elif op in OPS and not args:
                data.append(OPS[op]); need = 1 if OPS[op] >= 31 else 2; effect = 1 - need
            elif op in BRANCHES and len(args) == 1:
                target = name(args[0]); targets = [target]
                patches.append((at + 1, target)); data.extend(struct.pack('<BI', BRANCHES[op], 0))
                if op != 'jump': effect, need = -1, 1
            elif op == 'return' and not args:
                data.append(13)
            elif op == 'read_timer' and not args:
                data.extend(bytes((36, 0, 31, 0)))
            elif op == 'native_abs' and not args:
                data.extend(bytes((36, 0, 39, 1, 1, 4)));effect,need=-1,1
            elif op == 'native_sqrt' and not args:
                data.extend(bytes((36, 0, 23, 1, 1, 4)));effect,need=-1,1
            elif op in ('native_min', 'native_max') and not args:
                data.extend(bytes((36, 0, 25 if op == 'native_min' else 26, 2, 1, 8)));effect,need=-2,2
            elif op == 'wait' and len(args) == 1:
                raw = wait_duration(args[0])
                data.extend(bytes((36, 0, 28, 0)))
                data.extend(struct.pack('<BBI', 0, 4, raw))
                data.extend(bytes((36, 0, 30, 1, 1, 4, 36, 0, 29, 0)))
            elif op == 'call' and len(args) == 2 and isinstance(args[1], list):
                callee = names.get(name(args[0]))
                if callee is None:
                    raise ValueError('Unknown callee')
                values = list(args[1]); called = functions[callee]; count = called.get('inputs', 0)
                defaults = called.get('defaults', [])
                if not count - len(defaults) <= len(values) <= count:
                    raise ValueError('Call arity mismatch')
                supplied = len(values)
                values.extend(defaults[supplied - (count - len(defaults)):])
                data.extend(struct.pack('<BBIBBI', 0, 4, index, 0, 4, 0))
                for value in reversed(values):
                    if isinstance(value, dict) and set(value) == {'register'}:
                        data.extend((9, integer(value['register'], 0, 15)))
                    else:
                        data.extend(struct.pack('<BBI', 0, 4, scalar(value, strings)))
                data.extend(struct.pack('<BH', 12, callee))
                struct.pack_into('<I', data, at + 8, len(data))
            else:
                raise ValueError(f'Unknown operation or invalid operands: {ins!r}')
            instructions.append((at, len(data), op, effect, need, targets))
        if not instructions or instructions[-1][2] != 'return':
            raise ValueError('Function must end with return')
        if len(data) - start > 1024:
            raise ValueError('Function exceeds 1024-byte execution window')
        for at, target in patches:
            if target not in labels or labels[target] == len(data):
                raise ValueError('Unknown label or branch to function end')
            struct.pack_into('<I', data, at, labels[target])
        by_address = {ins[0]: ins for ins in instructions}
        depths, pending = {}, [(start, fn.get('inputs', 0))]
        while pending:
            at, depth = pending.pop()
            if at in depths:
                if depths[at] != depth:
                    raise ValueError('Inconsistent stack depth at branch merge')
                continue
            depths[at] = depth
            if at not in by_address:
                raise ValueError('Execution falls outside function')
            _, following, op, effect, need, targets = by_address[at]
            if depth < need or not 0 <= depth + effect <= 64:
                raise ValueError('Stack underflow or stack bound exceeded')
            depth += effect
            if op == 'return':
                if depth:
                    raise ValueError('Return must consume all arguments and temporaries; result belongs in register 0')
                continue
            pending.extend((labels[label], depth) for label in targets)
            if op != 'jump':
                pending.append((following, depth))
        inputs, input_table, defaults, default_table, string_at = records[index]
        struct.pack_into('<IBBBBIIIIII', data, 24 + index * 32, start, inputs, 0, 0, len(defaults),
                         default_table, input_table, 0, 0, zlib.crc32(fn['name'].encode('utf-8')) ^ 0xffffffff, 0xc0000000 | string_at)
        instruction_total += len(instructions)
        if instruction_total > 65536 or len(data) > 1024 * 1024:
            raise ValueError('Compiled program limit exceeded')
    struct.pack_into('<4sIIIII', data, 0, b'#scp', 24, len(functions), variables, 0, 0)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve() or (args.output.exists() and args.output.samefile(args.source)):
        parser.error('Source and output must be different files')
    try:
        if args.source.stat().st_size > 1024 * 1024:
            raise ValueError('Source exceeds 1 MiB')
        result = compile_source(json.loads(args.source.read_text(encoding='utf-8')))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print(f'Compiled {len(result)} bytes to {args.output}')


if __name__ == '__main__':
    main()
