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

## THE CAUSE, FOUND: PAE makes the kernel and the processor disagree

Read out of the running guest:

    cr3 = 0x22c000   (page_table_pml4)
    cr4 = 0x2a0      = PAE(0x20) | PGE(0x80) | OSFXSR(0x200)

`mmu.c` writes and walks FOUR levels: PML4 -> PDPT -> PD -> PT.  With PAE set the
processor uses THREE, and CR3 names a PDPT whose entries are 8 bytes, with no
fourth level at all.  So a table index means one thing to the kernel and
something else to the processor.

That is why the diagnostic walk looked perfect and the fetch still failed: it
printed `mmu.c`'s view of the tables, which really is correct, while the
processor was indexing them differently.  Every dynamic mapping -- the mapping
window, the user window, and every page an ELF program is given -- is wrong in
the only sense that matters.

The identity map survives by luck.  Under PAE, `pml4[0]` is read as a PDPT
entry, so `pdpt_framebuffer`'s entries are read as PD entries, and `0x83` has
the page-size bit, so they become 2 MiB pages.  That maps 0..10 MiB by accident.
The kernel image is 1.4 MiB, so boot works and everything in it works.

## Two attempts at the fix, both of which failed to boot

Changing CR4 to drop PAE, and rebuilding a proper 4-level identity map
(PML4 -> PDPT -> PD with 512 x 2 MiB entries covering the low gigabyte).  The
assembled code was verified correct by disassembly, and the tables are at
0x22c000/0x22d000/0x22e000, all written before paging is enabled.  It still
produced zero serial output, so something in that path faults before the first
print and has not been isolated yet.

So: the diagnosis is solid and evidenced, the fix is not.  Do not assume PAE is
the last word -- it is the best-supported explanation, not a confirmed one.

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
