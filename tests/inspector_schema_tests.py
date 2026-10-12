"""Command object inspection must retain read-only fields and existing actor schemas."""
import json
from pathlib import Path
import re
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from inspector import Client, schema


class InspectorSchemaTests(unittest.TestCase):
    def test_resource_edit_integer_transport_and_read_only_maxima(self):
        client=Client.__new__(Client);client.query=lambda source:source
        self.assertIn(', 2147483647)',client.set_property('player','hp',2147483647))
        self.assertIn(', -1)',client.set_property('player','cp',-1))
        for value in (1.5,2147483648,-2147483649):
            with self.assertRaises(ValueError):client.set_property('player','ep',value)
        for game in ('sky1','sky2'):
            fields=dict(schema(game)['player'])
            self.assertTrue(all(fields[name] for name in ('hp','ep','cp')))
            self.assertTrue(all(not fields[name] for name in ('max_hp','max_ep','max_cp')))

    def test_yaw_and_state_edit_validation_and_transport(self):
        client=Client.__new__(Client);client.query=lambda source:source
        self.assertIn('yaw_radians',client.set_property('player','yaw_radians',.5))
        self.assertIn('native_state_byte',client.set_property('player','native_state_byte',255))
        self.assertIn(', 255)',client.set_property('player','native_state_byte',255))
        for name,value in [('yaw_radians',7),('native_state_byte',-.5),('native_state_byte',256),('native_state_byte',.5)]:
            with self.subTest(name=name,value=value),self.assertRaises(ValueError):client.set_property('player',name,value)

    def client(self, game):
        client=Client.__new__(Client)
        client.fields=schema(game)
        client.roots=tuple(root for root in client.fields if root!='actor')
        client.errors={}
        def query(source):
            if "engine.list" in source:
                return json.dumps({'next':-1,'objects':[{'id':'@fixture','class':'.?AVActor@chara@sora@@','address':'0x123'}]})
            root=re.search(r'fdk.object\("([^"]+)"\)',source).group(1)
            if 'local rows, errors' not in source:
                return json.dumps({'class':'.?AVCommandSys@script@sora@@' if root.startswith('script_group:') else 'Root','address':'0x123'})
            return json.dumps({'values':{'class':'.?AVCommandSys@script@sora@@','address':'0x123','group':0,
                                        'slot_count':40 if game=='sky1' else 43,'implemented_count':40 if game=='sky1' else 43},
                               'errors':client.errors})
        client.query=query
        return client

    def test_roots_and_readonly_group_properties(self):
        for game in ('sky1','sky2'):
            with self.subTest(game=game):
                client=self.client(game)
                self.assertEqual(len(client.roots),36)
                rows=client.objects()
                self.assertEqual(len(rows),37)
                self.assertEqual([row['id'] for row in rows[8:36]],[f'script_group:{group}' for group in range(28)])
                properties=client.properties('script_group:0','.?AVCommandSys@script@sora@@')
                self.assertEqual([p[0] for p in properties],['class','address','group','slot_count','implemented_count'])
                self.assertFalse(any(p[2] for p in properties))
                self.assertIn(('position_x',True),client.fields['player'])
                client.errors={'implemented_count':'group unavailable'}
                partial=client.properties('script_group:0')
                self.assertEqual(partial[4],('implemented_count','Unavailable: group unavailable',False))
                self.assertIsInstance(partial[3][1],int)


if __name__=='__main__':
    unittest.main()
