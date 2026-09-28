#!/usr/bin/env python3
"""Build a FAT16 disk image from a directory of files.

The previous format was a flat manifest of name/LBA/length triples written by
this script, which was enough to get a WAD in front of the Doom code but is not
a filesystem: no directories, no allocation, and nothing another tool can read.
This makes a real FAT16 image instead, so the host can populate it with
mtools, the guest mounts it with an actual FAT driver, and the same file can be
pulled out with mdir on any Linux box.

mkfs.vfat and mtools do the formatting, so the image is a genuine FAT16 volume
rather than something that merely resembles one.
"""

import os
import shutil
import subprocess
import sys
import tempfile


def run(args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        sys.stderr.write('%s failed:\n%s\n' % (' '.join(args),
                                               result.stderr.strip()))
        raise SystemExit(1)
    return result.stdout


def collect(root):
    """(relative path, absolute path) for every file, directories included."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames):
            absolute = os.path.join(dirpath, name)
            found.append((os.path.relpath(absolute, root).replace(os.sep, '/'),
                          absolute, True))
        for name in sorted(filenames):
            absolute = os.path.join(dirpath, name)
            found.append((os.path.relpath(absolute, root).replace(os.sep, '/'),
                          absolute, False))
    found.sort()
    return found


def main():
    if len(sys.argv) not in (3, 4):
        sys.stderr.write('usage: mkfatdisk.py <source-dir> <output.img> '
                         '[size-mib]\n')
        return 2
    source, output = sys.argv[1], sys.argv[2]
    size_mib = int(sys.argv[3]) if len(sys.argv) > 3 else 64

    for tool in ('mkfs.vfat', 'mcopy', 'mmd'):
        if shutil.which(tool) is None:
            sys.stderr.write('mkfatdisk: %s is not installed\n' % tool)
            return 1

    entries = collect(source)
    if os.path.exists(output):
        os.unlink(output)
    # mkfs.vfat wants a file of the right size; sparse is fine
    with open(output, 'wb') as image:
        image.truncate(size_mib * 1024 * 1024)

    # -F 16 forces FAT16.  A 32 MiB volume would only reach FAT12, which has a
    # different cluster encoding, and 64 MiB is not much more to carry around.
    run(['mkfs.vfat', '-F', '16', '-n', 'KLYE', output])

    # mtools keeps its own state; point it at a scratch config so it cannot
    # pick up a drive letter mapping from the environment
    config = tempfile.NamedTemporaryFile('w', suffix='.mtoolsrc', delete=False)
    config.write('mtools_skip_check=1\ndrive c: file="%s" offset=0\n' % output)
    config.close()
    environment = dict(os.environ, MTOOLSRC=config.name)

    made = {''}
    for relative, absolute, is_dir in entries:
        parent = os.path.dirname(relative)
        if parent and parent not in made:
            # create every missing level, nearest first
            parts = parent.split('/')
            for depth in range(1, len(parts) + 1):
                path = '/'.join(parts[:depth])
                if path in made:
                    continue
                run(['mmd', '-i', output, '::/' + path], env=environment)
                made.add(path)
        if is_dir:
            continue
        name = os.path.basename(relative)
        target = '::/%s/%s' % (parent, name) if parent else '::/%s' % name
        run(['mcopy', '-i', output, '-o', absolute, target], env=environment)

    os.unlink(config.name)

    total = 0
    for relative, absolute, is_dir in entries:
        if not is_dir:
            total += os.path.getsize(absolute)
    print('mkfatdisk: FAT16, %d file(s), %d bytes of files -> %s'
          % (len(entries) - len(made) + 1, total, output))
    listing = run(['mdir', '-i', output, '-/', '::'], env=environment)
    for line in listing.splitlines():
        if line.strip():
            print('  ' + line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
