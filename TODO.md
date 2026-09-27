# Klye OS roadmap

Tracked here rather than only in-session so it survives a session restart.
Ordered so each blocker sits above whatever depends on it.

## Done

- [x] Task stacks and the graphics arena off the static BSS region; boots in 32 MiB
- [x] `docs/lua.md` and `docs/kby.md`
- [x] VFS mount no longer aborts on a file over 4 KiB; `/bin/dino` launcher

## Next

- [ ] Restore a bounded per-frame KBY budget (~20k-50k)
- [ ] Dirty-rectangle tracking and a row blitter for a stable 60 FPS
- [ ] ATA driver and disk-backed VFS, so installed apps survive a reboot
- [ ] Larger VFS capacity: more nodes, more blocks, and much bigger files
- [ ] Full PMM: dynamic page mapping, guard pages, demand paging
- [ ] Userspace and ring 3, so Lua apps stop running in ring 0

## Later

- [ ] Boot a real DOOM WAD, to drive the VFS, PMM, and MMU work
- [ ] PCI enumeration, BAR mapping, DMA-capable physical mappings
- [ ] VIRTIO-GPU with virgl for accelerated 3D

## Known limits and gaps

- No persistent storage. RAM VFS only, so `kpm install` disappears on reboot.
- Lua apps run in ring 0. Errors are caught, but there is no memory isolation.
- VFS holds ~128 nodes, 320 x 512-byte blocks, ~12 KiB max file size.
  A 4 MB DOOM WAD does not fit.
- The frame heap is PMM-lite: contiguous frames plus a free-list malloc.
  No dynamic page mapping, no DMA-capable physical ranges.
- No PCI, DMA, or VIRTIO stack, which is what blocks accelerated 3D.
- `KBY_BUDGET` is 2,000,000, far above the intended per-frame cap. No program
  tested so far has come close enough to trip it, so the cap is unverified.
- Compositor is demand-driven when idle, but `bench` forces a full recompose at
  ~28-34 ms/frame. A live Lua window steps at roughly 65-69 frames/second.
  A stable interactive 60 FPS is not yet established.
- PS/2 mouse has no wheel support; `ps2_config_byte` reads `0x00`.
- Deleting `/` has no shell policy bypass. It still fails structurally, because
  node 0 is the root sentinel and `vfs_delete()` rejects `index <= 0`.
