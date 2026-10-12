"""Capture supported scene actors and their transforms at one FDK update boundary."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import uuid

from console import execute


TRANSFORM_FIELDS = [f'transform_m{row}{column}' for row in range(4) for column in range(4)]
POSITION_FIELDS = ['position_x', 'position_y', 'position_z']


def capture(pid, mod='HelloFDK', character_snapshots=False, model_metadata=False, local_transforms=False):
    key = '__fdkss_export_' + uuid.uuid4().hex
    try:
        # All reads occur in this single console execution on the owning update thread.
        metadata = json.loads(execute(pid, mod, f'''
            local data = {{}}
            for _, actor in ipairs(fdk.find_objects('')) do
                local entry = {{class=actor.class, address=actor.address}}
                local named, name = pcall(function() return actor.name end)
                if named then entry.name = name end
                local identified, entity = pcall(function() return actor.entity_id end)
                if identified then entry.entity_id = entity end
                local ok, failure = pcall(function()
                    entry.matrix = {{}}
                    for row = 0, 3 do for column = 0, 3 do
                        local value = actor['transform_m' .. row .. column]
                        assert(type(value) == 'number', 'transform unavailable')
                        entry.matrix[#entry.matrix+1] = value
                    end end
                    entry.position = {{actor.position_x, actor.position_y, actor.position_z}}
                    assert(#entry.position == 3, 'position unavailable')
                end)
                if not ok then entry.matrix=nil; entry.position=nil; entry.error=tostring(failure) end
                if {'true' if local_transforms else 'false'} then
                    local copied, local_transform = pcall(function()
                        local t = {{position={{actor.position_x, actor.position_y, actor.position_z}},
                            quaternion={{actor.rotation_x, actor.rotation_y, actor.rotation_z, actor.rotation_w}},
                            scale={{actor.scale_x, actor.scale_y, actor.scale_z}}}}
                        assert(#t.position == 3 and #t.quaternion == 4 and #t.scale == 3, 'local transform unavailable')
                        return t
                    end)
                    if copied then entry.local_transform=local_transform
                    else entry.local_transform_error=tostring(local_transform) end
                end
                if {'true' if character_snapshots else 'false'} and entry.class == '.?AVActor@chara@sora@@' then
                    entry.character_snapshot = {{}}
                    local copied, resources = pcall(function() return actor:resources() end)
                    if copied then entry.character_snapshot.resources=resources
                    else entry.character_snapshot.resource_error=tostring(resources) end
                    local copied_conditions, records = pcall(function() return actor:conditions() end)
                    if copied_conditions and records then
                        entry.character_snapshot.keys={{}}
                        for _,record in ipairs(records) do
                            entry.character_snapshot.keys[#entry.character_snapshot.keys+1]=record.raw_key
                        end
                    elseif not copied_conditions then entry.character_snapshot.condition_error=tostring(records) end
                end
                if {'true' if model_metadata else 'false'} and
                   (entry.class == '.?AVActor@chara@sora@@' or entry.class == '.?AVObjectActor@fieldmap@sora@@') then
                    entry.model_snapshot = {{}}
                    local copied, model = pcall(function() return actor:model_info() end)
                    if copied then entry.model_snapshot.metadata=model
                    else entry.model_snapshot.error=tostring(model) end
                end
                data[#data+1] = entry
            end
            _G.{key} = data
            return string.format('{{"map_id":%q,"build_sha256":%q,"actor_count":%d}}',
                fdk.engine.scene.map_id or '', fdk.call('game.build_sha256'), #data)
        '''))
        actors = []
        page_size = 2 if character_snapshots or model_metadata else 8
        for first in range(1, metadata['actor_count'] + 1, page_size):
            page = json.loads(execute(pid, mod, f'''
                local data, result = assert(_G.{key}), {{}}
                local function numbers(values)
                    local result = {{}}
                    for i, value in ipairs(values) do result[i] = string.format('%.17g', value) end
                    return '[' .. table.concat(result, ',') .. ']'
                end
                local function quote(value)
                    local result = {{'"'}}
                    for index = 1, #value do
                        local byte = string.byte(value, index)
                        if byte == 34 or byte == 92 then result[#result+1] = string.char(92, byte)
                        elseif byte < 32 then result[#result+1] = string.format('\\\\u%04x', byte)
                        else result[#result+1] = string.char(byte) end
                    end
                    result[#result+1] = '"'
                    return table.concat(result)
                end
                for index = {first}, math.min({first + page_size - 1}, #data) do
                    local entry = data[index]
                    local row = string.format('{{"class":%q,"address":%q', entry.class, entry.address)
                    if entry.name then row = row .. ',"name":' .. quote(entry.name) end
                    if entry.entity_id then row = row .. string.format(',"entity_id":%d', entry.entity_id) end
                    if entry.matrix then
                        row = row .. ',"transform":' .. numbers(entry.matrix) .. ',"position":' .. numbers(entry.position)
                    else
                        -- Fixed ASCII marker; the detailed Lua error remains in the captured entry.
                        row = row .. ',"error":"unsupported or unavailable actor transform"'
                    end
                    if {'true' if local_transforms else 'false'} then
                        local t=entry.local_transform
                        row=row..',"local_transform":'
                        if t then
                            row=row..'{{"position":'..numbers(t.position)..',"quaternion":'..numbers(t.quaternion)..',"scale":'..numbers(t.scale)..'}}'
                        else
                            row=row..'null,"local_transform_error":'..quote(entry.local_transform_error)
                        end
                    end
                    if entry.character_snapshot then
                        local snapshot=entry.character_snapshot
                        row=row..',"character_snapshot":{{"resources":'
                        if snapshot.resources then
                            local r=snapshot.resources
                            row=row..string.format('{{"hp":%d,"max_hp":%d,"ep":%d,"max_ep":%d,"cp":%d,"max_cp":%d}}',
                                r.hp,r.max_hp,r.ep,r.max_ep,r.cp,r.max_cp)
                        else row=row..'null' end
                        row=row..',"conditions":'
                        if snapshot.keys then row=row..'{{"raw_keys":'..numbers(snapshot.keys)..',"count":'..#snapshot.keys..'}}'
                        else row=row..'null' end
                        if snapshot.resource_error then row=row..',"resource_error":'..quote(snapshot.resource_error) end
                        if snapshot.condition_error then row=row..',"condition_error":'..quote(snapshot.condition_error) end
                        row=row..'}}'
                    end
                    if entry.model_snapshot then
                        local model=entry.model_snapshot.metadata
                        row=row..',"model_metadata":'
                        if model then
                            row=row..'{{"name":'..quote(model.name)..string.format(
                                ',"format_version":%d,"mesh_count":%d,"node_count":%d}}',
                                model.format_version,model.mesh_count,model.node_count)
                        else row=row..'null' end
                        if entry.model_snapshot.error then row=row..',"model_error":'..quote(entry.model_snapshot.error) end
                    end
                    result[#result+1] = row .. '}}'
                end
                return '[' .. table.concat(result, ',') .. ']'
            '''))
            actors.extend(page)
        if len(actors) != metadata['actor_count']:
            raise RuntimeError('Scene snapshot page count mismatch')
        return {
            'format': 'fdkss.scene', 'format_version': 1,
            'captured_at': datetime.now(timezone.utc).isoformat(),
            'pid': pid, **metadata,
            'coordinate_system': {
                'units': 'unconverted engine units',
                'matrix_order': 'row-major engine memory order; translation at indices 12, 13, 14',
                'vector_convention': 'row vectors; transpose for column-vector consumers',
                **({'local_quaternion_order': 'x,y,z,w; copied without normalization',
                    'local_scale': 'local diagonal scale, not decomposed world scale or collision dimensions'} if local_transforms else {}),
            },
            'scope': ('Current scene actors with copied names, IDs and transforms.'
                      + (' Includes copied exact-Chara resources/condition keys; raw keys have no active-condition or action attribution meaning.' if character_snapshots else '')
                      + (' Includes copied exact-Chara/ObjectActor Model metadata; no retained Model handles or mesh geometry.' if model_metadata else '')
                      + (' Includes local position, raw quaternion and diagonal scale; these components are not a decomposition of the cached matrix.' if local_transforms else '')),
            'character_snapshots': bool(character_snapshots),
            'model_metadata': bool(model_metadata),
            **({'local_transforms': True} if local_transforms else {}),
            'actors': actors,
        }
    finally:
        try:
            execute(pid, mod, f'_G.{key} = nil')
        except (OSError, RuntimeError):
            pass  # State is also discarded on mod reload or runtime stop.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--mod', default='HelloFDK')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--character-snapshots', action='store_true', help='Include copied Chara HP/EP/CP and raw condition keys at the same update boundary')
    parser.add_argument('--model-metadata', action='store_true', help='Include copied current Chara/ObjectActor Model name, version and mesh/node counts')
    parser.add_argument('--local-transforms', action='store_true', help='Include local position, raw XYZW quaternion and diagonal scale copied at the same update boundary')
    args = parser.parse_args()
    result = capture(args.pid, args.mod, args.character_snapshots, args.model_metadata, args.local_transforms)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    failures = sum('error' in actor for actor in result['actors'])
    print(f"Exported {result['actor_count']} actors in {result['map_id']} ({failures} unavailable transforms) to {args.output}")


if __name__ == '__main__':
    main()
