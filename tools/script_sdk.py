"""Export verified #scp program and function metadata; does not decompile bytecode."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def inspect(data):
    if not 24 <= len(data) <= 16 * 1024 * 1024:
        raise ValueError('Invalid compiled program size')
    magic, table, count, variables, inputs, outputs = struct.unpack_from('<4sIIIII', data)
    if magic != b'#scp' or table < 24 or count > 65536:
        raise ValueError('Unsupported compiled program header')

    def region(offset, size):
        if offset > len(data) or size > len(data) - offset:
            raise ValueError('Program metadata points outside the file')
        return data[offset:offset + size]

    region(table, count * 32)
    # Each variable record contains a tagged name plus a second raw DWORD.
    # The interpreter's live value array is separate from this metadata table.
    variable_data = region(variables, (inputs + outputs) * 8)
    variable_records = []
    for index in range(inputs + outputs):
        raw_name, raw_value = struct.unpack_from('<II', variable_data, index * 8)
        if raw_name >> 30 != 3:
            raise ValueError('Invalid variable-name tag')
        start = raw_name & 0x3fffffff
        region(start, 1)
        end = data.find(b'\0', start, min(start + 256, len(data)))
        if end < 0:
            raise ValueError('Unterminated variable name')
        name = data[start:end].decode('utf-8')
        if not name:
            raise ValueError('Empty variable name')
        variable_records.append(dict(index=index, name=name, raw_name=raw_name, raw_value=raw_value))
    functions = []
    names = set()
    for index in range(count):
        code, argc_in, flags0, flags1, argc_out, out_table, in_table, structs, struct_table, checksum, tagged_name = struct.unpack_from('<IBBBBIIIIII', data, table + index * 32)
        if code >= len(data) or tagged_name >> 30 != 3:
            raise ValueError('Invalid code or function-name offset')
        start = tagged_name & 0x3fffffff
        region(start, 1)
        end = data.find(b'\0', start, min(start + 256, len(data)))
        if end < 0:
            raise ValueError('Unterminated function name')
        raw_name = data[start:end]
        name = raw_name.decode('utf-8')
        if not name or name in names or (zlib.crc32(raw_name) ^ 0xffffffff) != checksum:
            raise ValueError('Invalid, duplicate or corrupt function name')
        names.add(name)
        raw_inputs = region(in_table, argc_in * 4)
        raw_outputs = region(out_table, argc_out * 4)
        region(struct_table, structs * 12)
        functions.append(dict(name=name, index=index, code_offset=code, input_count=argc_in,
                              default_count=argc_out, flags0=flags0, flags1=flags1, checksum=checksum,
                              raw_input_metadata=list(struct.unpack(f'<{argc_in}I', raw_inputs)),
                              raw_default_metadata=list(struct.unpack(f'<{argc_out}I', raw_outputs)),
                              struct_count=structs, struct_table_offset=struct_table))
    return dict(format='fdkss.script', version=1, sha256=hashlib.sha256(data).hexdigest(),
                size_bytes=len(data), function_count=count, input_variables=inputs, output_variables=outputs,
                raw_variable_metadata=list(struct.unpack(f'<{(inputs + outputs) * 2}I', variable_data)),
                variable_records=variable_records,
                functions=functions,
                limitations='Argument types, flags, instruction semantics and calling convention are unverified.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve() or (args.output.exists() and args.output.samefile(args.source)):
        parser.error('Source and output must be different files')
    result = inspect(args.source.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'Exported {result["function_count"]} functions to {args.output}')


if __name__ == '__main__':
    main()
