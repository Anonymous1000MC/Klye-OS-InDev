#!/usr/bin/env python3
"""Build a Klye disk image from a directory of files.

The on-disk format is deliberately minimal, because the point of this stage is
getting a multi-megabyte file such as a WAD in front of the game code, not
general-purpose storage. It is a superblock, a flat manifest of name/LBA/length
triples, then the file data. There are no directories, no allocation, and no
free space tracking: the host lays every file out contiguously and the guest
just reads.

Once a real filesystem lands it can be built on top of the same block layer,
since this driver already proved out reads and writes against the hardware.

Layout, all in 512-byte sectors:

    sector 0        superblock
    sector 1..N     manifest entries, 16 per sector
    then            file data, each file starting on a sector boundary

Superblock:
    0   magic "KLYEDISK"
    8   format version
    12  manifest entry count
    16  manifest start sector

Manifest entry, 48 bytes:
    0   name, 32 bytes, NUL padded
    32  starting LBA
    36  length in bytes
    40  flags, reserved
    44  checksum of the name, so a corrupt entry is detectable
"""

import os
import struct
import sys

SECTOR = 512
MAGIC = b'KLYEDISK'
VERSION = 1
ENTRY_SIZE = 48
ENTRIES_PER_SECTOR = SECTOR // ENTRY_SIZE
NAME_MAX = 32
SUPER_SECTOR = 0
MANIFEST_START = 1


def name_checksum(name):
    total = 0
    for byte in name.encode('utf-8'):
        total = (total * 31 + byte) & 0xFFFFFFFF
    return total


def collect(root):
    """Every regular file under root, as (name, absolute path) sorted by name."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for filename in sorted(filenames):
            absolute = os.path.join(dirpath, filename)
            relative = os.path.relpath(absolute, root).replace(os.sep, '/')
            found.append((relative, absolute))
    found.sort(key=lambda item: item[0])
    return found


def main():
    if len(sys.argv) != 4:
        sys.stderr.write('usage: mkdisk.py <source-dir> <output.img> '
                         '<size-mib>\n')
        return 2
    source, output, size_mib = sys.argv[1], sys.argv[2], int(sys.argv[3])

    entries = collect(source)
    manifest_sectors = (len(entries) + ENTRIES_PER_SECTOR - 1) // \
        ENTRIES_PER_SECTOR
    data_start = MANIFEST_START + max(1, manifest_sectors)

    # assign an LBA to each file, then check the image is big enough
    placed = []
    cursor = data_start
    for name, absolute in entries:
        size = os.path.getsize(absolute)
        placed.append((name, absolute, cursor, size))
        cursor += (size + SECTOR - 1) // SECTOR
    capacity = size_mib * 1024 * 1024 // SECTOR
    if cursor > capacity:
        needed = (cursor * SECTOR + 1024 * 1024 - 1) // (1024 * 1024)
        sys.stderr.write('image too small: needs %d MiB, asked for %d\n' %
                         (needed, size_mib))
        return 1

    with open(output, 'wb') as image:
        # superblock
        block = bytearray(SECTOR)
        block[0:8] = MAGIC
        struct.pack_into('<III', block, 8, VERSION, len(entries),
                         MANIFEST_START)
        image.write(block)

        # manifest
        manifest = bytearray()
        for name, _absolute, lba, size in placed:
            raw = name.encode('utf-8')
            if len(raw) >= NAME_MAX:
                sys.stderr.write('name too long for the manifest: %s\n' % name)
                return 1
            entry = bytearray(ENTRY_SIZE)
            entry[0:len(raw)] = raw
            struct.pack_into('<III', entry, 32, lba, size, 0)
            struct.pack_into('<I', entry, 44, name_checksum(name))
            manifest += entry
        # pad the final manifest sector so the data starts where we promised
        while len(manifest) % SECTOR != 0:
            manifest += b'\0'
        image.write(bytes(manifest))

        # file data
        for _name, absolute, _lba, _size in placed:
            with open(absolute, 'rb') as source_file:
                remaining = _size
                while remaining > 0:
                    chunk = source_file.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    image.write(chunk)
                    remaining -= len(chunk)
                if remaining > 0:
                    pad = (-remaining) % SECTOR
                    image.write(b'\0' * pad)

    total = os.path.getsize(output)
    print('mkdisk: %d file(s), %d bytes -> %s' % (len(entries), total, output))
    for name, _absolute, lba, size in placed:
        print('  %-40s lba %-8d %d bytes' % (name, lba, size))
    return 0


if __name__ == '__main__':
    sys.exit(main())
