#!/usr/bin/env python3
"""The pipeline list the port imports at start (vk_pipeline_list.cpp) from the map tour's export.

usage: make_pipeline_list.py <tour pipelines folder (user/tour/pipelines)> <out file>

The tour exports the shader descriptions (.meta) and pipeline keys (.key) the game made; the
compiled SPIR-V is left out (it depends on the GPU) and so is the shaders' own code (it is the
game's, read from the player's copy at import).

File: "BBPL", format (u32), entry count (u32), payload size (u64), then the payload compressed
with zlib. Payload: per entry its blob type (u8: 0 shader description, 2 pipeline key), name
length (u8), name (ASCII, the cache file name without extension), data size (u32), data.
"""
import os
import struct
import sys
import zlib

FORMAT = 1
TYPES = {'.meta': 0, '.key': 2}
# Shader descriptions whose compile without the game (the port's replay of the stored inputs)
# did not give the game's own SPIR-V: left for the game to compile when it meets them; the
# pipelines that use them are then left out by the port too.
EXCLUDE = {
    '0x0000042f8e4a112a',  # compute shader 0x42f2a521, permutation 0
}


def main():
    folder, out = sys.argv[1], sys.argv[2]
    payload, count = bytearray(), 0
    for name in sorted(os.listdir(folder)):
        stem, ext = os.path.splitext(name)
        if ext not in TYPES or stem in EXCLUDE:
            continue
        with open(os.path.join(folder, name), 'rb') as f:
            data = f.read()
        payload += struct.pack('<BB', TYPES[ext], len(stem)) + stem.encode('ascii')
        payload += struct.pack('<I', len(data)) + data
        count += 1
    packed = zlib.compress(bytes(payload), 9)
    with open(out + '.tmp', 'wb') as f:
        f.write(b'BBPL' + struct.pack('<IIQ', FORMAT, count, len(payload)) + packed)
    os.replace(out + '.tmp', out)
    print('%d entries, %d bytes (%d unpacked) in %s' % (count, 24 + len(packed), len(payload), out))


if __name__ == '__main__':
    main()
