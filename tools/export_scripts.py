"""Capture loaded program/function metadata on the game's owning update thread."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
from console import execute


def capture(pid, mod='HelloFDK'):
    names = execute(pid, mod, "local r={} for _,p in ipairs(fdk.find_scripts()) do local n=p:info().name; assert(not n:find('[^ -~]')); r[#r+1]=n end return table.concat(r, string.char(10))").splitlines()
    programs = []
    for name in names:
        literal = json.dumps(name, ensure_ascii=True)
        header = execute(pid, mod, f"local i=assert(fdk.script({literal}):info()); return i.size_bytes, i.function_count, i.input_variables, i.output_variables")
        size, count, inputs, outputs = map(int, header.split('\t'))
        functions = []
        for first in range(0, count, 16):
            page = execute(pid, mod, f'''local p=fdk.script({literal}); local rows={{}}
                for index={first}, {min(first + 15, count - 1)} do
                    local fn=assert(p:function_at(index))
                    assert(not fn.name:find('[^ -~]'), 'export requires printable ASCII function names')
                    rows[#rows+1]=string.format('{{"name":%q,"index":%d,"input_count":%d,"default_count":%d,"flags0":%d,"flags1":%d,"checksum":%d,"code_offset":%d}}',
                        fn.name, fn.index, fn.input_count, fn.default_count, fn.flags0, fn.flags1, fn.checksum, fn.code_offset)
                end
                return '[' .. table.concat(rows, ',') .. ']' ''')
            functions.extend(json.loads(page))
        after = execute(pid, mod, f"local i=assert(fdk.script({literal}):info()); return i.size_bytes, i.function_count, i.input_variables, i.output_variables")
        if after != header:
            raise RuntimeError(f'Program changed while exporting: {name}')
        programs.append(dict(name=name, size_bytes=size, function_count=count,
                             input_variables=inputs, output_variables=outputs, functions=functions))
    return dict(format='fdkss.loaded_scripts', version=1, pid=pid,
                captured_at=datetime.now(timezone.utc).isoformat(), program_count=len(programs),
                build_sha256=execute(pid, mod, "return fdk.call('game.build_sha256')"), programs=programs,
                limitations='Reads span update boundaries; metadata only, no verified argument types or VM calling convention.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--mod', default='HelloFDK')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = capture(args.pid, args.mod)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Exported {result["program_count"]} loaded programs to {args.output}')


if __name__ == '__main__':
    main()
