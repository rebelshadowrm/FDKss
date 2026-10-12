import copy
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from compile_script import compile_source
from script_sdk import inspect


def source(code, inputs=0, defaults=None):
    fn = dict(name='Test', inputs=inputs, code=code)
    if defaults is not None:
        fn['defaults'] = defaults
    return dict(format='fdk-asm-1', functions=[fn])


class CompilerTests(unittest.TestCase):
    def test_independent_constant_bytecode(self):
        data = compile_source(source([['push', 42], ['save', 0], ['return']]))
        metadata = inspect(data)
        at = metadata['functions'][0]['code_offset']
        self.assertEqual(data[at:], bytes((0, 4, 42, 0, 0, 64, 10, 0, 13)))
        self.assertEqual(metadata['functions'][0]['name'], 'Test')

    def test_branches_defaults_and_call_packet(self):
        doc = source([['call', 'Child', []], ['return']])
        doc['functions'].append(dict(name='Child', inputs=1, defaults=[-7],
            code=[['save', 0], ['return']]))
        data = compile_source(doc)
        funcs = inspect(data)['functions']
        at = funcs[0]['code_offset']
        self.assertEqual(data[at:at+12], struct.pack('<BBIBBI', 0, 4, 0, 0, 4, at+21))
        self.assertEqual(data[at+12:at+21], bytes((0,4,249,255,255,127,12,1,0)))
        self.assertEqual(funcs[1]['raw_default_metadata'], [0x7ffffff9])
        doc = source([['label','loop'], ['push',1], ['jump_if','loop'], ['return']])
        data = compile_source(doc); at = inspect(data)['functions'][0]['code_offset']
        self.assertEqual(struct.unpack_from('<I',data,at+7)[0], at)

    def test_strings_and_float_precision(self):
        data = compile_source(source([['push','hello'],['save',0],['return']]))
        at = inspect(data)['functions'][0]['code_offset']
        raw = struct.unpack_from('<I',data,at+2)[0]
        self.assertEqual(data[raw & 0x3fffffff:(raw & 0x3fffffff)+6], b'hello\0')
        for v in (-7.5, 0.0, 1.5):
            data = compile_source(source([['push',v],['save',0],['return']]))
            at = inspect(data)['functions'][0]['code_offset']; raw = struct.unpack_from('<I',data,at+2)[0]
            self.assertEqual(struct.unpack('<f',struct.pack('<I',(raw << 2) & 0xffffffff))[0], v)

    def test_rejections(self):
        bad = [
            source([['native',0,30,1],['return']]),
            source([['push',True],['save',0],['return']]),
            source([['push',536870912],['save',0],['return']]),
            source([['push',float('nan')],['save',0],['return']]),
            source([['push',1e-40],['save',0],['return']]),
            source([['push',1e40],['save',0],['return']]),
            source([['push',1],['return']]),
            source([['save',0],['return']]),
            source([['copy',-4],['return']]),
            source([['jump','missing'],['return']]),
            source([['jump','end'],['return'],['label','end']]),
            source([['label','x'],['label','x'],['return']]),
            source([['push',1],['jump_if','end'],['push',2],['label','end'],['return']]),
            source([['call','Missing',[]],['return']]),
            source([['load',16],['return']]),
            source([['wait',-1],['return']]),
            source([['wait',31],['return']]),
            source([['wait',True],['return']]),
        ]
        doc = source([['return']]); doc['functions'].append(copy.deepcopy(doc['functions'][0])); bad.append(doc)
        for doc in bad:
            with self.subTest(doc=doc), self.assertRaises(ValueError):
                compile_source(doc)

    def test_read_timer_lowering(self):
        data = compile_source(source([['read_timer'], ['return']]))
        at = inspect(data)['functions'][0]['code_offset']
        self.assertEqual(data[at:], bytes((36, 0, 31, 0, 13)))
        with self.assertRaises(ValueError):
            compile_source(source([['read_timer', 1], ['return']]))

    def test_native_abs_consumes_one_argument_and_sets_register_zero(self):
        data = compile_source(source([['push', -19], ['native_abs'], ['return']]))
        at = inspect(data)['functions'][0]['code_offset']
        self.assertEqual(data[at + 6:], bytes((36, 0, 39, 1, 1, 4, 13)))
        for code in ([['native_abs'], ['return']], [['push', -19], ['native_abs', 1], ['return']]):
            with self.assertRaises(ValueError):
                compile_source(source(code))

    def test_native_selection_consumes_two_arguments_and_sets_register_zero(self):
        for name, command in [('native_min', 25), ('native_max', 26)]:
            data = compile_source(source([['push', 7], ['push', -19], [name], ['return']]))
            at = inspect(data)['functions'][0]['code_offset']
            self.assertEqual(data[at + 12:], bytes((36, 0, command, 2, 1, 8, 13)))
            for code in ([[name], ['return']], [['push', 7], [name], ['return']],
                         [['push', 7], ['push', -19], [name, 1], ['return']]):
                with self.assertRaises(ValueError): compile_source(source(code))

    def test_native_sqrt_lowering_and_stack_refusal(self):
        data = compile_source(source([['push', 4], ['native_sqrt'], ['return']]))
        at = inspect(data)['functions'][0]['code_offset']
        self.assertEqual(data[at + 6:], bytes((36, 0, 23, 1, 1, 4, 13)))
        for code in ([['native_sqrt'], ['return']], [['push', 4], ['native_sqrt', 1], ['return']]):
            with self.assertRaises(ValueError): compile_source(source(code))

    def test_wait_lowering_and_precision(self):
        for duration in (0, 1, .1, .25, 1e-50, 30.0):
            data = compile_source(source([['wait',duration],['return']]))
            at = inspect(data)['functions'][0]['code_offset']
            self.assertEqual(data[at:at+6], bytes((36,0,28,0,0,4)))
            raw = struct.unpack_from('<I', data, at+6)[0]
            decoded = (raw & 0x3fffffff) if raw>>30 == 1 else struct.unpack('<f',struct.pack('<I',(raw << 2)&0xffffffff))[0]
            self.assertGreaterEqual(decoded, duration)
            self.assertEqual(data[at+10:at+20], bytes((36,0,30,1,1,4,36,0,29,0)))


if __name__ == '__main__':
    unittest.main()
