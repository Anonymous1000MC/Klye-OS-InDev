# Klye OS roadmap

Tracked here rather than only in-session so it survives a session restart.
Ordered so each blocker sits above whatever depends on it.

## Done

- [x] Task stacks and the graphics arena off the static BSS region; boots in 32 MiB
- [x] `docs/lua.md` and `docs/kby.md`
- [x] Working host test runner, after finding the documented one did not link
- [x] VFS mount no longer aborts on a file over 4 KiB; `/bin/dino` launcher
- [x] Bound the per-frame KBY budget and report a starved program
- [x] ATA PIO driver on the legacy IDE ports
- [x] Disk image format and whole-file loads for large files
- [x] Dynamic page mapping, so large buffers need not be contiguous
- [x] `make disk` and a disk image the harness attaches, so a WAD is reachable
- [x] WAD directory parsing and lump access
- [x] Doom picture and sprite decoding, and a title screen
- [x] Five float formatting bugs, found by a differential test against glibc
- [x] The qemu harness reading terminal text back correctly

## Doom format notes, so the archaeology is not repeated

Three layouts have been identified in DOOM1.WAD, and all three differ from what
a vanilla-format reading would give. Checked against the real file, not assumed.

- **Full screen pictures** are flat, not post encoded. Every column is the same
  length with the pixels at a fixed offset of 2: TITLEPIC is 320x200 with a
  stride of 209, and 320 * 209 accounts for the lump exactly. Reading one as
  posts overruns the column and paints 289 rows.
- **Sprites** are post encoded with varying column stride, and the length byte
  does not match the number of pixels that follow. A k+2 fudge renders a
  recognisable but stretched Doomguy. Unresolved, and it gates sprites, the
  status bar face, and the title animation.
- **TEXTURE1 is offset indexed**: a 125 entry table of uint32 follows the count,
  where vanilla walks the textures inline. Each entry lands on a sane
  definition, AASTINKY at 24x72 with 2 patches, BIGDOOR1 at 128x96 with 5.
- **The texture definition header is 18 bytes** in this file, not the 24 of
  vanilla: name[8], masked[4], width[2], height[2], npatches[2], with no
  columndirectory and no trailing pad. That is confirmed by the offsets: the
  next texture begins exactly 18 + 12 * npatches bytes later. But the column
  directory would then start at the same place the next texture's name sits,
  so where it actually lives is still unresolved. This is the blocker for
  texture lookup and therefore for the 3D view.

## Next

- [x] Skip repainting a script that drew the same thing again: a static window
      went from 133 composites per 3 s to 0, and a full compose is 14.4 ms
      (69 fps) with the present already damage limited to 28 rows
- [x] FAT16 on the ATA disk: real directories, long filenames, read and write
- [x] A boot order fix, without which a real filesystem on the disk stopped the
      kernel booting at all
- [ ] Damage only the bounding box of what a script changed, rather than the
      whole window, and a wide-copy present path
- [x] FAT16 mounted on the ATA disk: directories, long filenames, read and
      write, so files have real paths
- [x] Larger VFS capacity: 4 MiB across 8192 blocks, 512 nodes and 256 KiB per
      file, up from 160 KiB, 128 nodes and 12 KiB. The three tables were
      static arrays in BSS, so growing them meant growing the kernel image by
      megabytes before a single file existed; they now come from the MMU, which
      maps the block store a frame at a time and does not need a physically
      contiguous run. A guest too small for the full size gets a smaller
      filesystem rather than a failed mount, and the boot log reports what was
      actually allocated instead of the ceiling, which is how the original
      160 KiB went unnoticed for so long.
- [x] `kpm build` can install a script again. The stem was taken by stripping
      the extension but keeping the directory, so `kpm build lua/clock.lua`
      installed to `/bin/lua/clock.lua`, whose parent does not exist, and every
      script with a directory in its name failed. All of them have one, because
      the sources live in `/home/klye/lua`, so the command was unusable and
      nothing had noticed: the apps in `/bin` are baked into the ISO by the
      build system, so the guest's own build was never on a path that mattered.
- [x] VFS host test. It had none, which is the other half of why the broken
      write path survived: a filesystem with no tests, exercised only by an
      image that is regenerated at build time.
- [ ] Full PMM: guard pages, demand paging, DMA-capable physical mappings
- [ ] Persist user data to the FAT16 data partition: config, and game state.
      Not `kpm install`: a live system's root is the RAM VFS, so an installed
      app has nowhere to live, and the FAT volume is a data volume rather than a
      system one. What needs it is anything that should outlive a session, which
      is the settings app, a Doom high score table, and Lua apps saving state.
      The write path is implemented already and only needs a caller.
- [ ] Userspace and ring 3, so Lua apps stop running in ring 0
- [ ] Doom sprites are not pixel exact: a post length byte does not match the
      pixels that follow it, and a k+2 fudge renders a stretched but
      recognisable Doomguy. Pictures are exact, which is what the title screen
      needs.

## Later

- [x] Boot the real DOOM WAD: the title screen renders from it, read out of
      FAT16
- [ ] PCI enumeration, BAR mapping, DMA-capable physical mappings
- [ ] VIRTIO-GPU with virgl for accelerated 3D

## Next phase, once the list above is done

To be spun up as a fresh list rather than appended here, so the ordering can be
rethought on its own. Recorded now so the intent is not lost.

### Full ext4

Read *and* write, including the journal, not the read-only or ext2 subset.

- [ ] A common block and filesystem interface, so filesystems are pluggable and
      the installer does not know which one it is writing
- [ ] ext4 read-only first: superblock, group descriptors, inodes, extents
      instead of indirect blocks, htree directories, 64-bit fields
- [ ] ext4 read-write: bitmaps, block and inode allocation, extent insertion
      and splitting, directory entry creation
- [ ] jbd2: transactions, the commit block, replay on mount, write barriers.
      This is the part that makes crash consistency real rather than claimed
- [ ] ext2 read-write as a fallback, since it is the same on-disk family
      without the journal and is a useful stepping stone
- [ ] Checksums, or a documented decision to ignore them

### Installation

- [ ] A boot menu with Live and Install entries. A small selector in the
      existing boot code rather than a GRUB dependency, unless that changes
- [ ] Live mode: keep nothing across a reboot, and never write to the disk.
      The RAM VFS already discards everything, so this is close to free
- [ ] A setup screen with real options, not a single Install button:
      device and partition selection, layout (whole disk, existing partition,
      free space), filesystem choice, hostname, first user account, whether to
      keep a live image, bootloader target, and a confirmation summary
- [ ] A partitioner: read and write a partition table, and create partitions
- [ ] The install flow end to end: lay down the filesystem, copy the kernel and
      the base tree, install a bootloader, write config
- [ ] Persistent root filesystem so installed apps survive a reboot

### Linux binary support

- [ ] An ELF loader: headers, program headers, PT_LOAD mapping, relocations for
      a position independent binary, and TLS set up through the fs base
- [ ] The Linux x86-64 syscall ABI, and the syscalls themselves: mmap, mprotect,
      brk, openat, fstat, readlink, arch_prctl, set_tid_address, rt_sigaction,
      rt_sigreturn, futex, getrandom, getdents64, ioctl, madvise, and the rest
      that turn out to be load-bearing
- [ ] Signals, and a futex implementation, or libc hangs on startup
- [ ] Userspace and ring 3, which everything above depends on
- [ ] A filesystem in Linux's layout: /lib, /usr/lib, /etc, /proc/self/maps,
      /dev/null, /dev/urandom
- [ ] A static musl or dietlibc program first, then dynamic linking via PT_INTERP
- [ ] Goal: run an existing DOOM port rather than hand-writing a renderer

## Known limits and gaps

- A bug that turned out not to be one, recorded so it is not re-chased: while
  the Doom post decoder was overrunning its columns, artwork appeared at the
  bottom left of the screen and looked like a compositor region that was never
  repainted. Flooding the back buffer and forcing a full recompose showed every
  pixel is repainted, and the artwork was the decoder painting past the
  picture. Fixing the decoder removed it. There is no compositor bug.
- Persistent storage is a work in progress. There is now an ATA driver, a
  flat disk image format (tools/mkdisk.py), and whole-file loads that work for
  multi-megabyte files. What is still missing is a real filesystem, so the RAM
  VFS is still where the shell and editor keep their files and `kpm install`
  still does not survive a reboot.
