from io import BytesIO
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import texture
try:
    from PIL import Image
except ImportError:
    Image = None
try:
    import lz4.frame
except ImportError:
    lz4 = None


def dds():
    header = bytearray(128)
    struct.pack_into('<4s7I', header, 0, b'DDS ', 124, 0x100f, 1, 2, 8, 0, 1)
    struct.pack_into('<8I', header, 76, 32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000)
    struct.pack_into('<I', header, 108, 0x1000)
    return bytes(header) + b'\0\0\xff\xff\0\xff\0\xff'


class TextureTests(unittest.TestCase):
    def test_raw_dds_limits_and_shape_validation(self):
        data = dds()
        self.assertEqual(texture.unpack(data), data)
        for end in range(128):
            with self.subTest(end=end), self.assertRaises(ValueError):
                texture.unpack(data[:end])
        for offset, value in ((12, 0), (16, 8193), (24, 2), (112, 0x200), (76, 31)):
            bad = bytearray(data); struct.pack_into('<I', bad, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                texture.unpack(bad)
        with self.assertRaisesRegex(ValueError, 'byte limit'):
            texture.unpack(data, len(data)-1)

    @unittest.skipUnless(Image, 'Optional Pillow dependency absent')
    def test_png_preserves_rgba_pixels(self):
        data, size = texture.png(dds())
        self.assertEqual(size, (2, 1))
        with Image.open(BytesIO(data)) as image:
            self.assertEqual(image.getpixel((0, 0)), (255, 0, 0, 255))
            self.assertEqual(image.getpixel((1, 0)), (0, 255, 0, 255))

    @unittest.skipUnless(lz4, 'Optional LZ4 dependency absent')
    def test_lz4_limits_truncation_and_single_frame(self):
        compressed = lz4.frame.compress(dds())
        self.assertEqual(texture.unpack(compressed), dds())
        with self.assertRaisesRegex(ValueError, 'Decompressed'):
            texture.unpack(lz4.frame.compress(dds()+bytes(4096)), 1024)
        for end in range(1, len(compressed)):
            with self.subTest(end=end), self.assertRaises((ValueError, RuntimeError)):
                texture.unpack(compressed[:end])
        with self.assertRaisesRegex(ValueError, 'trailing'):
            texture.unpack(compressed+compressed)


if __name__ == '__main__':
    unittest.main()
