#!/usr/bin/env python3
"""Generate a WAD with real structure, for testing the parser without a copy
of the real thing.

This is not a substitute for DOOM1.WAD and will not run the game. It produces
the same *shape*: a header, a directory of 16-byte entries, and lump data,
including the lumps Doom needs before it will show anything (PLAYPAL, COLORMAP,
PNAMES, TEXTURE1, a font) and one level laid out as the marker plus THINGS,
LINEDEFS and SIDEDEFS. That is enough to prove the parser walks the directory,
resolves offsets, and finds a level, and enough to render something textured
off the palette later.

Everything it writes is deterministic, so a test can assert exact bytes.
"""

import struct
import sys

# Doom's fixed-width string fields: NUL padded, truncated to 8.
def name8(text):
    raw = text.upper().encode('ascii')[:8]
    return raw + b'\0' * (8 - len(raw))


def build_thing(x, y, angle, type_, flags):
    return struct.pack('<hhhhHH', x, y, angle, type_, flags, 0)


def build_linedef(v1, v2, flags, special, tag, right, left):
    return struct.pack('<HHHHHHH', v1, v2, flags, special, tag, right, left)


def build_sidedef(xoff, yoff, upper, lower, middle, sector):
    return struct.pack('<hhhhhHH', xoff, yoff, upper, lower, middle, sector, 0)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'test.wad'

    lumps = []

    # 14 palettes of 256 rgb triples. Only the first is used, but the count has
    # to be right for anything that walks palettes.
    playpal = bytearray()
    for palette in range(14):
        for entry in range(256):
            r = (entry * 7 + palette * 3) & 0xFF
            g = (entry * 5 + palette * 11) & 0xFF
            b = (entry * 3 + palette * 19) & 0xFF
            playpal += bytes((r, g, b))
    lumps.append(('PLAYPAL', bytes(playpal)))

    # 34 colormaps of 256 bytes.
    colormap = bytearray()
    for cmap in range(34):
        for entry in range(256):
            colormap.append((entry + cmap) & 0xFF)
    lumps.append(('COLORMAP', bytes(colormap)))

    # One patch name, so PNAMES is non-trivial.
    pnames = struct.pack('<I', 1) + name8('WALL01')
    lumps.append(('PNAMES', pnames))

    # TEXTURE1: one texture, one patch, 64x64 of a simple pattern.
    width = 64
    height = 64
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            # a brick-ish pattern with a mortar line, so a texture-mapped
            # render is obviously correct when it shows up
            if y % 16 == 0 or (x + (8 if (y // 16) % 2 else 0)) % 32 == 0:
                pixels.append(96)     # mortar
            else:
                pixels.append(32 + ((x // 8 + y // 8) % 3) * 24)
    # patch header: width, height, leftoffset, topoffset, then column offsets
    patch = struct.pack('<hhhh', width, height, 0, 0)
    patch += struct.pack('<I', 0)  # column directory placeholder
    column_dir_at = len(patch) - 4
    # one column per 8 pixels, each a single post of 64 bytes
    columns = b''
    data = b''
    for column in range(width // 8):
        columns += struct.pack('<I', 4 + column * (2 + height))
        data += bytes((0, height))          # topdelta, length
        data += bytes(pixels[column * 8:(column + 1) * 8] * 8)  # 8x64 block
    patch = patch[:column_dir_at] + columns + data

    # texture definition: name, masked size, then one patch reference
    texture = name8('WALL01') + struct.pack('<IIhh', 0, width, height, 0)
    texture += struct.pack('<H', 1)              # one patch
    texture += struct.pack('<HHhh', 0, 0, width, height)  # origin, patch 0
    texture1 = struct.pack('<I', 1) + texture
    lumps.append(('TEXTURE1', texture1))

    # A 8x8 font called WAD1, which is what the status bar wants.
    font = bytearray()
    for y in range(8):
        for x in range(8):
            font.append(0xFF if (x + y) % 2 == 0 else 0x00)
    font_patch = struct.pack('<hhhh', 8, 8, 0, 0)
    font_patch += struct.pack('<I', 0)
    font_patch += struct.pack('<I', 4)          # single column at offset 4
    font_patch += bytes((0, 8)) + bytes(font)
    lumps.append(('WAD1', font_patch))

    lumps.append(('P_START', b''))
    lumps.append(('P_END', b''))

    # A level: marker, then the three lumps, in Doom's order.
    things = b''.join([
        build_thing(1056, -3616, 90, 1, 7),
        build_thing(1280, -3520, 270, 2, 7),
        build_thing(1024, -2432, 0, 3004, 7),
    ])
    linedefs = b''.join([
        build_linedef(0, 1, 1, 0, 0, 0, 0),
        build_linedef(1, 2, 1, 0, 0, 0, 1),
        build_linedef(2, 3, 1, 0, 0, 1, 0),
        build_linedef(3, 0, 1, 0, 0, 1, 1),
    ])
    sidedefs = b''.join([
        build_sidedef(0, 0, 0, 0, 0, 0),
        build_sidedef(16, 0, 0, 0, 0, 0),
        build_sidedef(0, 16, 0, 0, 0, 0),
        build_sidedef(-16, 0, 0, 0, 0, 0),
    ])
    lumps.append(('E1M1', b''))
    lumps.append(('THINGS', things))
    lumps.append(('LINEDEFS', linedefs))
    lumps.append(('SIDEDEFS', sidedefs))

    # second level, so level iteration can be checked
    lumps.append(('E1M2', b''))
    lumps.append(('THINGS', things))
    lumps.append(('LINEDEFS', linedefs))
    lumps.append(('SIDEDEFS', sidedefs))
    lumps.append(('E1M3', b''))

    lumps.append(('P_START', b''))
    lumps.append(('P_END', b''))

    directory_size = len(lumps) * 16
    data_offset = 12 + directory_size

    directory = b''
    body = b''
    for lump_name, payload in lumps:
        # an empty lump is a marker, and Doom stores offset -1 for those
        if len(payload) == 0:
            directory += struct.pack('<ii', -1, 0) + name8(lump_name)
        else:
            # offsets are absolute file offsets, not offsets into the body
            directory += struct.pack('<ii', data_offset + len(body),
                                     len(payload)) + name8(lump_name)
            body += payload

    with open(out_path, 'wb') as out:
        out.write(b'PWAD' + struct.pack('<ii', len(lumps), 12))
        out.write(directory)
        out.write(body)

    print('mkwad: %d lumps, %d bytes -> %s' %
          (len(lumps), data_offset + len(body), out_path))
    for index, (lump_name, payload) in enumerate(lumps):
        print('  %2d  %-8s %6d' % (index, lump_name, len(payload)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
