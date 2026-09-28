# Klye OS roadmap

Tracked here rather than only in-session so it survives a session restart.
Ordered so each blocker sits above whatever depends on it.

## Done

- [x] Task stacks and the graphics arena off the static BSS region; boots in 32 MiB
- [x] `docs/lua.md` and `docs/kby.md`
- [x] Working host test runner, after finding the documented one did not link
- [x] VFS mount no longer aborts on a file over 4 KiB; `/bin/dino` launcher

## Next

- [ ] Restore a bounded per-frame KBY budget (~20k-50k)
- [x] Bound the per-frame KBY budget and report a starved program
- [ ] Dirty-rectangle tracking and a row blitter for a stable 60 FPS
- [x] ATA PIO driver on the legacy IDE ports
- [x] Disk image format and whole-file loads for large files
- [x] Dynamic page mapping, so large buffers need not be contiguous
- [x] `make disk` and a disk image the harness attaches, so a WAD is reachable
- [x] WAD directory parsing and lump access
- [ ] A real filesystem on the block layer, so installed apps survive a reboot
- [ ] Larger VFS capacity: more nodes, more blocks, and much bigger files
- [ ] Full PMM: dynamic page mapping, guard pages, demand paging
- [ ] Userspace and ring 3, so Lua apps stop running in ring 0

## Later

- [ ] Boot a real DOOM WAD, to drive the VFS, PMM, and MMU work
- [ ] PCI enumeration, BAR mapping, DMA-capable physical mappings
- [ ] VIRTIO-GPU with virgl for accelerated 3D

## Known limits and gaps

- Persistent storage is a work in progress. There is now an ATA driver, a
  flat disk image format (tools/mkdisk.py), and whole-file loads that work for
  multi-megabyte files. What is still missing is a real filesystem, so the RAM
  VFS is still where the shell and editor keep their files and `kpm install`
  still does not survive a reboot.
- Lua apps run in ring 0. Errors are caught, but there is no memory isolation.
- VFS holds ~128 nodes, 320 x 512-byte blocks, ~12 KiB max file size.
  A 4 MB DOOM WAD does not fit.
- The frame heap is a bitmap plus a free-list malloc, and now supports
  dynamically mapped pages (mmu.c) so a large buffer can be virtually
  contiguous while physically scattered. Still missing: DMA-capable physical
  ranges, guard pages, demand paging, and page eviction.
- No PCI, DMA, or VIRTIO stack, which is what blocks accelerated 3D.
- Float formatting matches glibc for ordinary magnitudes, but not always at
  the edges. `tests/fmt_diff.c` compares 1440 cases against glibc; 63 disagree
  for two reasons, and both need exact big-integer decimal conversion to fix:
  past ~17 significant digits (our digits come from repeated double scaling, so
  the last digit can be one off, and an exact tie is indistinguishable from a
  value just above one), and an integer part past 2^64, where `%f` falls back
  to exponent form to avoid overflowing the cast. The test fails only if the
  count rises above that baseline, so a regression is still caught.
- No command-line flags to run the host tests are wired into the Makefile;
  use `./tests/run.sh`.
- Compositor is demand-driven when idle, but `bench` forces a full recompose at
  ~28-34 ms/frame. A live Lua window steps at roughly 65-69 frames/second.
  A stable interactive 60 FPS is not yet established.
- PS/2 mouse has no wheel support; `ps2_config_byte` reads `0x00`.
- Deleting `/` has no shell policy bypass. It still fails structurally, because
  node 0 is the root sentinel and `vfs_delete()` rejects `index <= 0`.
