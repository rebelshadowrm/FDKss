import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from export_battle import capture,validate,SUPPORTED


class BattleExportTests(unittest.TestCase):
    def snapshot(self):
        return dict(build_sha256=sorted(SUPPORTED)[0],available=True,atomic=False,count=1,
            records=[dict(index=0,status_word=0xffffffff,level=45,hp=1813,max_hp=12186,ep=1000,max_ep=1000,cp=199,max_cp=200)])

    def test_populated_and_absent(self):
        for digest in SUPPORTED:
            row=self.snapshot();row['build_sha256']=digest
            self.assertEqual(validate(row)['records'][0]['status_word'],0xffffffff)
        for available in (True,False):
            row=self.snapshot();row.update(available=available,count=0,records=[])
            self.assertEqual(validate(row)['records'],[])

    def test_bad_shape_build_or_unavailable_rows(self):
        for field,value in [('build_sha256','unknown'),('atomic',True),('count',True),('count',71),('count',0),('records',{}),('available',False)]:
            row=self.snapshot();row[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate(row)

    def test_bad_order_fields_and_numeric_resources(self):
        for field,value in [('index',1),('status_word',-1),('level',True),('hp',12187),('max_ep',0x80000000),('cp',201)]:
            row=self.snapshot();row['records'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate(row)
        row=self.snapshot();row['records'][0]['address']='0x1234'
        with self.assertRaises(ValueError):validate(row)

    def test_one_query_and_independent_copies(self):
        source=self.snapshot();calls=[]
        def query(lua):calls.append(lua);return json.dumps(source)
        held=capture(query);source['records'][0]['hp']=900;current=capture(query)
        self.assertEqual(len(calls),2)
        self.assertEqual(held['records'][0]['hp'],1813)
        self.assertEqual(current['records'][0]['hp'],900)
        self.assertIn('observed_utc',current)


if __name__=='__main__':unittest.main()
