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

## PAE: looked like the cause, and is not

cr3 = 0x22c000 and cr4 = 0x2a0 (PAE set), while mmu.c writes and walks four
levels.  That is a real inconsistency and it is why vm_dump_walk printed a
perfect walk: it prints mmu.c's own view, which is correct, and the processor
indexes the same tables differently.  It is a genuine latent bug.

But it is NOT why the ELF program cannot be fetched.  Tested directly: clearing
PAE in CR4 and leaving the original six page table entries completely unchanged
still produced zero serial output, and so did the correct 4-level identity map
with the framebuffer mapped.  Every variant with PAE clear fails to boot at all,
including the one that differs from the working kernel by a single bit.

So the boot code genuinely needs PAE, for reasons not yet understood -- plausibly
it relies on PAE's 64-bit CR3/entry layout combined with how the firmware left
memory, or the tables were written to suit a 3-level read and changing one bit
breaks an assumption elsewhere in the same file.

Correct the earlier claim: this is an open inconsistency worth fixing
carefully, not the ELF fault.  Do not spend another attempt on it without first
working out why the boot needs it.

## What is still unexplained

The instruction fetch at the entry is refused with cr2 = 0 while every table
the kernel builds is present and user, the GDT limit is 4 GiB, and the task
frame read back from the running guest is correct.  cr2 = 0 says the fault
happened before paging, which points at the segment -- but the segment measures
out correct.  That contradiction is the whole remaining problem.

Next step: dump CS, SS and the loaded descriptor at the moment of the fault,
from inside the fault handler, rather than after the fact.  Everything checked
so far was checked after the machine had already moved on.

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

## Solved: the ELF entry fault

`elf /bin/hello.elf` runs. It writes "hello from a loaded ELF program" and
exits 0, with no panic. Three separate faults were stacked on top of each other,
and the first two were disguised as the third.

### 1. Segments were never written to the frames

`elf_load_segment` copied the file through the *virtual* address
(`where = virtual_address + done`). The kernel runs with paging on and the user
window is not identity mapped, so those stores did not land in the program's
pages. The page came up present and user readable and full of zeros, so the
entry point faulted on an instruction fetch of zeroes.

The copy now goes through `vm_to_physical`, a page at a time, because a
translation is only valid for the page it came from and a segment can start part
way into one.

### 2. The stack was mapped over the program's own text

`vm_user_map_at`, which maps the segments, deliberately does not move the
window's bump pointer. So `vm_user_alloc_pages` for the stack returned the
*bottom* of the window -- the same pages the entry point had just been loaded
into -- and zeroed every page it covered. The text was wiped moments after
being written, which is why the fix for (1) appeared not to work.

The stack is now mapped at an address above the highest segment, computed from
the program headers before anything is mapped.

### 3. exit returned instead of terminating the program

`SYS_EXIT` recorded the code and returned. The syscall stub returns to user
mode with `sysretq`, which goes to the instruction *after* the syscall, so a
program whose last instruction is `exit` ran off the end of its text. It showed
up as a `#GP` at an address one past `.text`, which reads as a privilege or
paging fault and is neither.

`task_kill_current()` in scheduler.c, marked noreturn, called from the handler.

## Ruled out, with reasons

- **PAE vs four-level paging.** Not a bug. PAE is mandatory in long mode; the
  four-level hierarchy is built on top of PAE mechanics. Clearing CR4.PAE is
  why an earlier experiment produced a silent boot failure, and it was never a
  valid thing to try.
- **Missing static-PIE relocations.** Real gap, but not this fault.
  `readelf -d rootfs/bin/hello.elf` has no `DT_RELA` and `readelf -r` reports no
  relocations, because it is `-nostdlib` with no absolute pointers in data.
  A musl static binary will need the `R_X86_64_RELATIVE` loop before it runs.
- **CR2 = 0 from a nested fault.** No nested fault occurred; the count stayed
  at zero. The real reason CR2 read as zero is below.

## Why CR2 read as zero

CR2 was being read late: `capture_fault` read it after `last_error_code`, some
twenty struct writes and fourteen register copies, so a fault inside the handler
would have destroyed it. It never happened here, but the window was real.

`isr_common` could not close it either -- this assembler rejects
`movq %cr2, sym(%rip)` as an operand type mismatch, while the same move into a
register assembles. So vector 14 has its own entry macro that reads CR2 into
`%rax`, where the error code has already claimed it as scratch, and parks it in
`fault_cr2_latest` before the handler runs.

It read as zero because the fault was a `#GP`, which does not set CR2 at all.
Once the three faults above were fixed the `#PF` was gone and the `#GP` was
what remained, and CR2 was zero simply because nothing had put anything there.

## Diagnostics added, and why each one earned its place

- `nested_cr2` / `nested_rip` / `nested_error`: a handler that faults on its own
  is indistinguishable from a program that faults, from outside.
- `rip pte`, `rsp pte`: separates "page absent" from "page present but refused",
  which are the same fault from the program's side. This is what showed the
  stack was unmapped.
- `rip physical` + 16 code bytes: RIP alone says where the fault was reported,
  not what the program was doing. The bytes settled it -- a page of zeros at the
  entry meant the loader had not written, and a `#GP` at entry+0x30 said the
  program had been running and fell off the end.

The harness `gdb()` wrapper still prepends `print` to every command, so
breakpoints and memory reads through it remain unreliable.
