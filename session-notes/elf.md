# ELF loader

## Where it stands

`elf /bin/hello.elf` loads the file, maps all four PT_LOAD segments, sets up a
64 KiB user stack, spawns a task, and the task starts in ring 3.  It then
faults on the very first instruction fetch, and that is the whole of the
remaining problem.

## The fault

    !! page fault vector E error 5 [user read protection]
       rip 10000001000  cr2 0  rsp 10000010000  cs 23  ss 1B
       faulting task : 3

`cr2 = 0` on an instruction fetch is the thing to explain.  A page-table
refusal sets cr2 to the address walked; a cr2 of zero means the fault happened
before paging, i.e. at the segment.

## Ruled out, with the evidence

The walk for the entry address is correct, all four levels present and user:

    walk0[2] = 0x158a027 user=1 present=1
    walk1[0] = 0x158b027 user=1 present=1
    walk2[0] = 0x158c027 user=1 present=1
    walk3[1] = 0x1591007 user=1 present=1

- Mapping: leaf is `0x1591007` = present|write|user, physical 0x1591000, a real
  heap frame.  Not NX (bit 63 clear).
- GDT: gdt[4].limit_low = 0xffff, granularity = 0xaf, so the limit is
  0xFFFFF * 4096 + 4095 = 4 GiB.  The entry at 0x10000001000 is inside it.
- Task frame: rip 0x10000001000, cs 0x23, rsp 0x10000010000, ss 0x1B -- all
  correct, read out of the running guest over gdb.
- CR0/CR4: PG set, PAE set (cr4 = 0x2a0), eflags bit 1 set.
- The bias is inside the user window, and the window is 0x10000000000 based,
  which the guest confirmed by reading vm_user_base.

## NOT ruled out, and where to look next

1. **PAE.** cr4 has PAE set, so paging is three levels with an 8-byte PDPTE and
   no PML4, but mmu.c's vm_leaf_table_flags walks four levels.  CR3 is set from
   `page_table_pml4` and the boot code writes large-page PDEs into it, which is
   right for PAE.  The identity map therefore works and the dynamic windows
   may not -- the walk *prints* plausibly while indexing one level off, because
   a walk of the wrong depth still finds present entries.  Check this first:
   it explains why everything else works and only this does not.
2. The `cr2 = 0` shape still says segment, and the segment looks right.  If
   PAE turns out to be fine, dump CS/SS/descriptor at the fault itself rather
   than after the fact.

## Gotchas hit while building this

- `vm_user_map_at` was the missing piece: vm_user_alloc_pages bump-allocates,
  and a loader must map where the headers say.  It also needed its own
  vm_user_init call -- without it vm_user_limit is 0 and every range is
  rejected as "not in the user window".
- A PIE's header entry is 0x1000, an offset from the load base.  Testing it
  against a minimum address rejects every PIE ever built; test entry + bias.
- vfs had no vfs_read_at, so segment loading re-read the whole file from the
  start.  Added.
- gcc here can build `-static-pie -nostdlib`, which is enough to prove the
  loader.  No musl, no dietlibc, no libc.a, so a real libc binary cannot be
  produced here yet -- that is the next blocker after this fault.

## What the loaded program is

tests/hello.c, freestanding: write(1,...) twice then exit(0), via raw syscall
inline asm.  Deliberately not linked against libc, because there is no signal
handling, futex, brk or arch_prctl yet, so a libc program would stop inside
libc's own startup and say nothing about why.
