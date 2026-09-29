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

## The syscall return path, 2026-09-30

Four bugs, all here, all of which looked like a fault somewhere else. This is
the section that mattered most and it was the last one to break.

1. **rbx used as scratch to build the sysretq selector pair.** One instruction
   pair before handing control back, so it overwrote whatever the pops had just
   restored. musl keeps its thread control block in rbx across the arch_prctl
   that sets the thread pointer, so its next store went to a dead pointer. This
   is why the fault looked like a bad load bias, a missing relocation and a
   missing mmap, in that order. It was none of them. (`2452571`)

2. **FS base applied only on task entry.** The single `wrmsr` sat on the iretq
   path, which runs once when a task enters ring 3 and never again, so
   `arch_prctl` took effect one syscall too late and the program's next
   instruction read `%fs:0` at address 0. The comment above the slot already
   claimed the base was applied "on the way in and on the way out"; the way-out
   half was never written. (`3828b81`)

3. **wrmsr ignoring the high half of the base.** The base is 64 bits and wrmsr
   takes `edx:eax`, 32 bits each, so `edx` has to be bits 32:63 of it. It held
   whatever `interrupt_dispatch` last returned, giving `0x3F800004138` for a
   pointer that was `0x10000004138`. Zeroing edx is equally wrong -- it drops
   the high half entirely, which is what my first attempt did. (`3545673`)

4. **No scratch register at all, finally.** CS and SS are constants, so the
   whole selector pair is one constant: `0x0023001B00000000` ORed with the
   flags. That needs only rax and r11, which sysretq already takes. r10 had
   been the scratch for (1) and was silently broken by it -- nothing had ever
   checked r10, because the canary did not test it. (`0d6cb95`)

### Still open: rdi, rsi, rdx, r8

Canary reads `......GHIJ..` after a silent argument-less ENOSYS. Those four are
caller-saved and a syscall must preserve them. Not yet explained.

Ruled out: the canary (byte-accurate, setup and expected values verified to
match); C writing the frame (`user.c` and `kernel.c` only *read* those four
fields); a print in the path (gated behind strace, failures reproduce silently);
push/pop symmetry (exact 15-register mirrors); the selector build (r10 was the
one it touched, and r10 now passes).

The failing set is exactly the SysV argument registers minus r9, minus r10 and
minus rcx (which SYSCALL has already destroyed). Claude's theory -- the stub
marshals syscall args into the C convention before saving -- matches the set
perfectly. **The stub contains no such code**: the 15 pushes are the first
thing it does and `rdi`/`rsi` are set only once the frame is complete. The
theory fits the symptom and contradicts the source, so it is unsupported. Worth
keeping as a shape to look for, not a conclusion.

What is missing is the *values*. They were never successfully read out: two
attempts to dump them through the guest failed, once by misreading the byte
stream's position relative to the report line and once by picking up the marker
text. Both produced numbers that disagreed with the pass/fail table, which is
how the errors were caught. Next time: print them in-program as ASCII hex, not
as binary through a `write()` that stops at NUL.
