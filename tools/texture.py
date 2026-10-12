"""Convert bounded 2D DDS textures, optionally wrapped in one LZ4 frame, to PNG."""
import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import struct

MAX_BYTES = 128 * 1024 * 1024
MAX_PIXELS = 16 * 1024 * 1024


def unpack(data, limit=MAX_BYTES):
    if not data or len(data) > limit:
        raise ValueError('Texture input exceeds the byte limit')
    if data[:4] == b'\x04\x22\x4d\x18':
        import lz4.frame
        decoder = lz4.frame.LZ4FrameDecompressor()
        output = bytearray(decoder.decompress(data, max_length=limit + 1))
        while not decoder.eof and not decoder.needs_input and len(output) <= limit:
            output.extend(decoder.decompress(b'', max_length=limit + 1 - len(output)))
        if len(output) > limit:
            raise ValueError('Decompressed texture exceeds the byte limit')
        if not decoder.eof or decoder.unused_data:
            raise ValueError('Truncated or trailing LZ4 texture data')
        data = bytes(output)
    if len(data) < 128 or data[:4] != b'DDS ' or struct.unpack_from('<I', data, 4)[0] != 124:
        raise ValueError('Expected a complete DDS header')
    height, width = struct.unpack_from('<II', data, 12)
    depth = struct.unpack_from('<I', data, 24)[0]
    caps2 = struct.unpack_from('<I', data, 112)[0]
    if not width or not height or width > 8192 or height > 8192 or width * height > MAX_PIXELS:
        raise ValueError('Texture dimensions exceed the pixel limit')
    if depth > 1 or caps2 & (0xfe00 | 0x200000):
        raise ValueError('Only single 2D textures are supported')
    if struct.unpack_from('<I', data, 76)[0] != 32:
        raise ValueError('Invalid DDS pixel format header')
    if data[84:88] == b'DX10':
        if len(data) < 148:
            raise ValueError('Truncated DDS DX10 header')
        _, dimension, misc, array_count, _ = struct.unpack_from('<5I', data, 128)
        if dimension != 3 or misc & 4 or array_count != 1:
            raise ValueError('Only single 2D textures are supported')
    return data


def png(data):
    from PIL import Image
    dds = unpack(data)
    with Image.open(BytesIO(dds)) as image:
        if image.format != 'DDS':
            raise ValueError('Expected a DDS image')
        image.load()
        output = BytesIO()
        image.convert('RGBA').save(output, format='PNG')
        return output.getvalue(), image.size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('texture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() == args.texture.resolve() or args.output.exists() and args.output.samefile(args.texture):
        parser.error('PNG output cannot replace the source texture')
    if args.texture.stat().st_size > MAX_BYTES:
        parser.error('Texture input exceeds the byte limit')
    source = args.texture.read_bytes()
    output, size = png(source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f'Exported {size[0]}x{size[1]} PNG to {args.output}; source SHA-256 {hashlib.sha256(source).hexdigest()}')


if __name__ == '__main__':
    main()
