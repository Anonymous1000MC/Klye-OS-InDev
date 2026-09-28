# Disk image contents

Files here are packed into `build/klye.img` by `make disk`, and the kernel
reads that image as an IDE disk at boot. This is separate from the ISO: the
built-in filesystem holds about 160 KB in total, so anything large has to live
on the disk.

The directory layout becomes the path inside the image, so
`disk/doom/DOOM1.WAD` is the file `doom/DOOM1.WAD` to a guest.

## DOOM

`doom/DOOM1.WAD` is what the `title` app reads. It is not in the repository,
since it is a game data file rather than source: put your own copy there and
rebuild.

    cp /path/to/DOOM1.wad disk/doom/DOOM1.WAD
    make disk

Then in the guest:

    open title

`files` lists what is actually on the image, and `wadinfo` parses the WAD and
reports its lumps and levels.
