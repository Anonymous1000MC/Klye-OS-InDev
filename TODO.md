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

## Next

- [ ] Dirty-rectangle tracking and a row blitter for a stable 60 FPS
- [ ] A real filesystem on the block layer, so installed apps survive a reboot
- [ ] Larger VFS capacity: more nodes, more blocks, and much bigger files
- [ ] Full PMM: guard pages, demand paging, DMA-capable physical mappings
- [ ] Userspace and ring 3, so Lua apps stop running in ring 0
- [ ] Doom sprites are not pixel exact: a post length byte does not match the
      pixels that follow it, and a k+2 fudge renders a stretched but
      recognisable Doomguy. Pictures are exact, which is what the title screen
      needs.

## Later

- [ ] Boot a real DOOM WAD, to drive the VFS, PMM, and MMU work
- [ ] PCI enumeration, BAR mapping, DMA-capable physical mappings
- [ ] VIRTIO-GPU with virgl for accelerated 3D

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
