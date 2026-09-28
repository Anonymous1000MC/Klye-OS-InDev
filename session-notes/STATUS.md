# Status

Last updated: ring 3 scheduler work, HEAD 9349ed6 + uncommitted scheduler fix.

## Verified working
- Boot to desktop, shell, compositor, all host tests (libc, math, wad, vfs, doom3d).
- Ring 3 privilege switch: iretq from ring 0 with correct cs/ss/rsp.
- Ring 3 code executes from a user-only page range (own PML4 slot, PTE_USER at
  every level).
- SYSCALL works end to end: entry, handler, return. write and exit handled.
- A ring 3 program runs as a real scheduler task and survives timer ticks
  (this is the fix that removed the reboot).
- Program and stack allocated from vm_user_alloc_pages.

## Known broken
- Only the FIRST of the test program's two writes runs. The second syscall is
  never reached. Verified: handler reports ret-rip = base+30 for a syscall at
  base+28, so the return address is right, and the sysretq R11 packing
  (CS<<48 | SS<<32 | flags) is verified correct by hand. Cause not established.
- exit is therefore never reached either.
- GDB against the live guest is unreliable: `gdb()` in tools/qemu_harness.py
  prepends `print ` to every -ex, so `b *addr` and `x/4i` run as print
  statements and silently do nothing. Use a hand-built gdb argv. Even then the
  remote connection times out, so do not trust a negative from it.
- Ring 3 has only write and exit. A real Linux binary needs arch_prctl,
  set_tid_address, brk, rt_sigaction, futex, getrandom, mmap, openat and more
  before libc finishes starting.
- No ELF loader yet.

## Next
1. Find why the second syscall is missed (instrument, do not use the harness gdb).
2. Linux syscall set that libc needs at startup.
3. ELF loader: ELF64 headers, PT_LOAD mapping, relocations, TLS via fs base.
4. Static musl or dietlibc binary, then PT_INTERP.
