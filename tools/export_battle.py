"""Export a copied battle roster through the supported owner-thread API."""
import argparse
from datetime import datetime,timezone
import json
from pathlib import Path

FIELDS=('index','status_word','level','hp','max_hp','ep','max_ep','cp','max_cp')
SNAPSHOT_LUA='''
local snapshot=fdk.battle_snapshot()
local digest=fdk.call('game.build_sha256')
if snapshot==nil then
    return '{"build_sha256":"'..digest..'","available":false,"atomic":false,"count":0,"records":[]}'
end
local rows={}
for i,r in ipairs(snapshot.records) do
    rows[i]=string.format('{"index":%d,"status_word":%d,"level":%d,"hp":%d,"max_hp":%d,"ep":%d,"max_ep":%d,"cp":%d,"max_cp":%d}',
        r.index,r.status_word,r.level,r.hp,r.max_hp,r.ep,r.max_ep,r.cp,r.max_cp)
end
return '{"build_sha256":"'..digest..'","available":true,"atomic":false,"count":'..snapshot.count..',"records":['..table.concat(rows,',')..']}'
'''
SUPPORTED={'4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf',
           'd8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf'}


def validate(value):
    if not isinstance(value,dict) or value.get('build_sha256') not in SUPPORTED:
        raise ValueError('Unsupported battle snapshot build')
    count=value.get('count');rows=value.get('records')
    if type(value.get('available')) is not bool or value.get('atomic') is not False or type(count) is not int or not 0<=count<=70 or not isinstance(rows,list) or len(rows)!=count:
        raise ValueError('Invalid battle snapshot shape')
    if not value['available'] and count:raise ValueError('Absent collection contains records')
    for index,row in enumerate(rows):
        if not isinstance(row,dict) or set(row)!=set(FIELDS) or row['index']!=index:
            raise ValueError('Invalid battle record fields or order')
        if any(type(row[f]) is not int or not 0<=row[f]<=0xffffffff for f in FIELDS):
            raise ValueError('Invalid copied battle DWORD')
        for current,maximum in [('hp','max_hp'),('ep','max_ep'),('cp','max_cp')]:
            if not row[current]<=row[maximum]<=0x7fffffff:raise ValueError('Invalid battle resources')
    return value


def capture(query):
    value=validate(json.loads(query(SNAPSHOT_LUA)))
    value['observed_utc']=datetime.now(timezone.utc).isoformat()
    return value


def main():
    from console import execute
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid',type=int);parser.add_argument('--mod',default='HelloFDK')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    value=capture(lambda source:execute(args.pid,args.mod,source))
    args.output.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    print(f'Copied {value["count"]} battle records to {args.output}')


if __name__=='__main__':main()
