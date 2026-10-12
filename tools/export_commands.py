"""Export the pinned runtime's read-only native command catalog in bounded owner-thread pages."""
import argparse
import json
from pathlib import Path
from console import execute


def export(pid, mod, query=None):
    query = query or (lambda source: execute(pid, mod, source))
    digest = query('return fdk.call("game.build_sha256")')
    groups = json.loads(query('return fdk.invoke("engine.script_command_groups")'))
    if len(groups) != 28 or [group['group'] for group in groups] != list(range(28)):
        raise ValueError('Unexpected native command group catalog')
    commands = []
    for group in groups:
        capacity = group['slot_count']
        if not isinstance(capacity, int) or not 1 <= capacity <= 256:
            raise ValueError('Invalid group slot capacity')
        cursor, seen = 0, set()
        while True:
            page = json.loads(query(f'return fdk.invoke("engine.script_command_page",{group["group"]},{cursor},16)'))
            rows, next_cursor = page['entries'], page['next']
            end = min(cursor + 16, capacity)
            if len(rows) > end - cursor or next_cursor != (end if end < capacity else -1):
                raise ValueError('Invalid native command pagination')
            for row in rows:
                command = row['command']
                if row['group'] != group['group'] or not cursor <= command < end or command in seen:
                    raise ValueError('Duplicate or out-of-page native command')
                seen.add(command)
                if not isinstance(row['handler_rva'], int) or row['handler_rva'] <= 0:
                    raise ValueError('Invalid native handler RVA')
                if not isinstance(row['raw_mask'], int) or not 0 <= row['raw_mask'] <= 0xffffffff:
                    raise ValueError('Invalid raw native mask')
                if len(bytes.fromhex(row['raw_entry_hex'])) != 24:
                    raise ValueError('Invalid raw native entry size')
                commands.append(row)
            if next_cursor == -1:
                break
            cursor = next_cursor
        if len(seen) != group['implemented_count']:
            raise ValueError('Native command catalog changed during export')
    return {'format_version': 1, 'build_sha256': digest, 'groups': groups, 'commands': commands,
            'limitations': ['Group pages are separate owner-thread snapshots.',
                            'Raw masks and entry words do not establish names, argument types, context or callable signatures.',
                            'Raw entry bytes include process addresses; handler_rva is relative to the pinned executable.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--mod', default='HelloFDK')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = export(args.pid, args.mod)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Exported {len(result["groups"])} groups and {len(result["commands"])} native command entries to {args.output}')


if __name__ == '__main__':
    main()
