"""Decode bounded D9BA Zstandard asset envelopes; preserve original serialized identities.

The layout is referenced from pinned kuro_mdl_tool externally, not copied source.
Encrypted F9BA/C9BA inputs are explicitly refused by this decoder.
"""
import hashlib
import struct

MAX_SIZE = 128 * 1024 * 1024


def decode(data):
    if len(data) > MAX_SIZE:
        raise ValueError('Serialized asset exceeds budget')
    wrappers = []
    for _ in range(5):
        if data[:4] in (b'F9BA',b'C9BA'):
            raise ValueError('Encrypted CLE assets require a separately verified external decrypt operation')
        if data[:4] != b'D9BA':
            return data, wrappers
        if len(wrappers)>=4:
            raise ValueError('Asset envelope nesting budget exceeded')
        if len(data)<8 or struct.unpack_from('<I',data,4)[0]!=len(data)-8:
            raise ValueError('Invalid D9BA payload size')
        import zstandard
        payload=data[8:]
        try:
            parameters=zstandard.get_frame_parameters(payload)
            size=zstandard.frame_content_size(payload)
        except zstandard.ZstdError as error:
            raise ValueError('Invalid D9BA Zstandard frame') from error
        if size in (zstandard.CONTENTSIZE_UNKNOWN,zstandard.CONTENTSIZE_ERROR) or size>MAX_SIZE or parameters.window_size>MAX_SIZE:
            raise ValueError('D9BA expanded-size/window budget is unknown or exceeded')
        decoder=zstandard.ZstdDecompressor().decompressobj()
        try:
            unpacked=decoder.decompress(payload)
        except zstandard.ZstdError as error:
            raise ValueError('Invalid D9BA Zstandard content/checksum') from error
        if not decoder.eof or len(unpacked)!=size or len(decoder.unused_data)>7 or any(decoder.unused_data):
            raise ValueError('Invalid D9BA frame or padding')
        wrappers.append({'magic':'D9BA','input_sha256':hashlib.sha256(data).hexdigest(),
                         'output_sha256':hashlib.sha256(unpacked).hexdigest(),'expanded_bytes':len(unpacked)})
        data=unpacked
    raise ValueError('Asset envelope nesting budget exceeded')
