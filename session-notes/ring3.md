# Ring 3

## What works
- iretq ring 0 -> ring 3, user-only page range, SYSCALL entry/handler/return.
- write and exit handled. Program runs as a real scheduler task, survives ticks.
- usertest does not reboot any more (5f2bf60).

## The open bug: second syscall never runs

The test program makes two writes then exit. Only the first write appears.
Not a reboot, not a panic, not a fault: the task is alive and the log just
stops. `exit` is never reached either.

### Ruled out, with the value that ruled it out
- Sysretq input is CORRECT. Traced immediately before the instruction:
    rcx = 1099511627806 = image base + 30   (syscall is at base + 28)  ok
    r11 = 150325625346   = 0x23001B00000002 = CS 0x23 | SS 0x1B | FLAGS 2  ok
    rsp = 1099511640040  = inside the user stack                          ok
  So the return address, the selector packing and the stack are all right.
- The image copy is right: user_test_entry is 0x1C into the blob, the
  messages are at +0x55 and +0x6F, and the handler reported rsi = base+0x55
  with rdx = 26 for the first message, the exact length.
- The handler is reached exactly once. user_syscall_count is the only evidence
  and it is static, so read it through a function, not by symbol.

### NOT ruled out
- What the program actually executes after the first return. Never observed.
  This is the thing to look at next.

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
