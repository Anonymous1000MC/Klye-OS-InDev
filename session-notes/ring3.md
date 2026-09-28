# Ring 3

## What works
- iretq ring 0 -> ring 3, user-only page range, SYSCALL entry/handler/return.
- write and exit handled. Program runs as a real scheduler task, survives ticks.
- usertest does not reboot any more (5f2bf60).
- BOTH writes and exit now run. The ring 3 path is complete for what it covers.

## The bug that was actually there, and why it hid so well
Two mistakes in the same six instructions, one after the other:

1. The code selector was shifted into r11 with `shl $16` twice, which puts it
   at bit 32 instead of bit 48. Bits 63:48 stayed zero, so sysretq was handed a
   NULL code selector. It rejects that without raising anything: no fault, no
   panic, the task just stops. And 0x00000023001B0002 still looks like a
   plausible register read from the wrong end, so a hand check passed it.
   Fixed: `shl $48`, giving 0x0023001B00000002.

2. Fixing that exposed the next one immediately. The scratch register used to
   build the stack selector was rcx -- which is where the return address lives.
   The program therefore jumped to 0x1B00000000, the stack selector executed as
   an instruction pointer. It faulted cleanly this time (error 4, protection,
   cs 0x23), which is what made it findable. Fixed: scratch in rbx.

The lesson worth keeping: the first version failed *silently*, so nothing in the
kernel could report it. Every silent failure here had the same shape -- a value
that looked right because it was checked from the wrong end, or checked before
it was set. A fault is worth a lot more than a hang.

## Tooling traps in this repo (cost real time, all avoidable)
1. tools/qemu_harness.py `gdb()` prepends `print ` to every -ex. So `b *addr`,
   `x/4i`, `c` and `info registers` all run as `print b *addr` and do nothing.
   They report a clean negative. Build a gdb argv by hand instead. Even then
   the remote connection timed out, so treat its output as unreliable.
2. Any scratch script that greps the serial log hides trailing lines. The
   sysret trace was present and correct the whole time and was filtered out by
   the very script used to look for it. Dump the raw log tail, unfiltered.
3. Serial output can interleave with itself; a line fragment appearing inline
   in another line is a lost newline, not a missing message.
4. A .bss symbol that is `static` may be optimised to a copy; gdb by symbol
   name then reads the wrong storage. Read through a non-static accessor.

## Bugs found on the way, all fixed
- tss.rsp[0] was 0: kernel.c declared a stack label as uint64_t, so it read
  the bytes AT the label rather than its address. Silent triple fault.
- ISR_ERROR discarded the page fault error code, so protection violations were
  indistinguishable from missing pages.
- PTE_FLAGS had no user bit. Not fixable on the leaf: the CPU checks U/S at all
  four levels and the kernel's tables are shared, so user memory needs its own
  PML4 slot.
- EFER.SCE never set: boot.asm did `or eax, 1 << 8` (LME) where it meant bit 0.
- Ring 3 had no task, so the tick orphaned it. That was the reboot.
- The user's stack pointer was parked in rbp, which is part of the handler's
  register frame, so save and restore were the program's own rbp.
- A trace `call` before the register save clobbered rax, so write arrived as
  "unknown syscall 16".
- addq $16 after pushing 3 words: the third was never discarded.
