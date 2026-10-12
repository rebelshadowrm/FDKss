from publication_fixtures import private_evidence
import json
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from trace_engine_references import reference, trace_references


class Family:
    def contiguous(self, at):
        return 0, 32


class ReferenceTests(unittest.TestCase):
    def test_locked_boolean_rmw_and_implicit_atomic_exchange(self):
        for prefix,opcode,immediate,width,exchange in [
            (b'\xf0',b'\x83\x0d',b'\x02',4,False),
            (b'\xf0\x48',b'\x83\x25',b'\xfd',8,False),
            (b'\xf0\x66',b'\x81\x0d',b'\x02\x00',2,False),
            (b'',b'\x86\x05',b'',1,True),
            (b'\x48',b'\x87\x05',b'',8,True)]:
            size=len(prefix+opcode)+4+len(immediate)
            code=prefix+opcode+struct.pack('<i',-size)+immediate
            effect=reference(code,0,len(code))
            self.assertEqual((effect['operation'],effect['target_rva'],effect['operand_bytes']),('read_write',0,width))
            self.assertEqual(effect['locked'],prefix.startswith(b'\xf0'))
            self.assertEqual(effect['atomic_memory_exchange'],exchange)
            self.assertIsNone(reference(code,0,len(code)-1))
        for code in (b'\xf0\x89\x05'+bytes(4),b'\xf0\x83\x3d'+bytes(5),b'\xf0\x66\x0f\x7f\x05'+bytes(4)):
            self.assertIsNone(reference(code,0,len(code)))

    @private_evidence
    def test_original_ys_framework_publish_and_main_thread_store(self):
        fixtures = json.loads((Path(__file__).parent / 'fixtures/engine_call_originals.json').read_text())['fixtures']
        for begin, targets, expected in [
            (0x6c0b10, [0xa86988, 0xd0bb90], {(0x6c15a5, 'address', 8), (0x6c1669, 'write', 8)}),
            (0x6a8290, [0xd0b658], {(0x6a82ce, 'write', 4)})]:
            row = next(row for row in fixtures if row['game'] == 'ysx' and row['begin'] == begin)
            a, b = row['begin'], row['end']; image = bytearray(0xd0bc00)
            image[a:b] = bytes.fromhex(row['code_hex'])
            instructions = {i['rva']: i for i in row['instructions']}
            class OriginalFamily:
                def contiguous(self, at):
                    return a, b
            result = trace_references(bytes(image), [(a, b)], OriginalFamily(), targets, lambda at, end: instructions.get(at))
            self.assertEqual(len(result['functions']), 1)
            observed = {(int(s['rva'], 16), s['operation'], s['operand_bytes']) for s in result['functions'][0]['sites']}
            self.assertEqual(observed, expected)

    def test_scalar_widths_and_rip_base_uses_complete_instruction_size(self):
        for prefix, opcode, width, access, immediate in [
            (b'\x48', b'\x8b\x05', 8, 'read', b''),
            (b'', b'\x89\x05', 4, 'write', b''),
            (b'\x66', b'\x8b\x05', 2, 'read', b''),
            (b'\x4c', b'\x8d\x0d', 8, 'address', b''),
            (b'', b'\xc6\x05', 1, 'write', b'\x01'),
            (b'\x48', b'\xc7\x05', 8, 'write', b'\x01\0\0\0'),
            (b'\x66', b'\x81\x3d', 2, 'read', b'\x01\0'),
            (b'\x48', b'\x83\x3d', 8, 'read', b'\x01')]:
            size = len(prefix + opcode) + 4 + len(immediate)
            code = prefix + opcode + struct.pack('<i', -size) + immediate
            row = reference(code, 0, len(code))
            self.assertEqual((row['target_rva'], row['size'], row['operand_bytes'], row['operation']), (0, size, width, access))
            self.assertIsNone(reference(code, 0, len(code) - 1))
        for code in (b'\x48\x8b\x00' + bytes(4), b'\x83\x05' + bytes(5), b'\xc7\x0d' + bytes(8)):
            self.assertIsNone(reference(code, 0, len(code)))

    def test_instruction_interiors_skipped_bytes_and_incomplete_walk_refused(self):
        image = bytearray(64); image[:7] = b'\x48\x8d\x05' + struct.pack('<i', 48 - 7)
        for rows, accepted in [({0: dict(size=7, kind='ordinary'), 7: dict(size=1, kind='return')}, True),
                               ({0: dict(size=8, kind='ordinary'), 8: dict(size=1, kind='return')}, False),
                               ({0: dict(size=1, kind='return')}, False),
                               ({0: dict(size=7, kind='ordinary'), 7: dict(size=1, kind='jump', target=None)}, False)]:
            result = trace_references(bytes(image), [(0, 32)], Family(), [48], lambda at, end: rows.get(at))
            self.assertEqual(len(result['functions']), int(accepted))
            self.assertFalse(result['runtime_profile_accepted'])
        for targets in ([], [48, 48], [64], [True], list(range(65))):
            with self.assertRaises(ValueError):
                trace_references(bytes(image), [(0, 32)], Family(), targets, lambda at, end: None)

    @private_evidence
    def test_independent_original_framework_reference(self):
        fixtures = json.loads((Path(__file__).parent / 'fixtures/engine_call_originals.json').read_text())['fixtures']
        row = next(row for row in fixtures if row['game'] == 'ysxpn' and row['begin'] == 0x761520)
        a, b = row['begin'], row['end']; image = bytearray(0xf06000)
        image[a:b] = bytes.fromhex(row['code_hex'])
        instructions = {i['rva']: i for i in row['instructions']}
        class OriginalFamily:
            def contiguous(self, at):
                return a, b
        # Complete independently frozen Frame reads the Application global.
        result = trace_references(bytes(image), [(a, b)], OriginalFamily(), [0xd2a280], lambda at, end: instructions.get(at))
        self.assertEqual(len(result['functions']), 1)
        sites = result['functions'][0]['sites']
        self.assertTrue(sites)
        self.assertTrue(all(site['operation'] == 'read' and site['operand_bytes'] == 8 for site in sites))
        self.assertEqual(result['functions'][0]['evidence']['code_hex'], row['code_hex'])


if __name__ == '__main__':
    unittest.main()
