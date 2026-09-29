# Status

Last updated: musl reaches main and returns 42. HEAD `0d6cb95`, working tree
clean apart from two untracked binaries.

## The headline

**Static musl runs end to end.** A true static PIE (ET_DYN, no `PT_INTERP`,
self-relocating) gets through self-relocation, auxv, TLS via `arch_prctl`, the
syscall ABI, calls `main`, writes to stdout, returns 42, and calls
`exit_group`. No panic.

    TRACE rax=9e rdi=1002 rsi=10000004138 ... ->0      arch_prctl(FS)
    musl main
    TRACE rax=e7 rdi=2a ...                             exit_group(42)
    TRACE end of program

This was the wall the whole ELF/TLS effort was pointed at. It took four
separate bugs, **all in my own syscall return path**, none of which were in the
loader, the page tables, the relocations or the program itself.

## Verified working
- Boot to desktop, shell, compositor, all host tests (7/7).
- Ring 3: iretq entry, SYSCALL, sysretq return, scheduler task, timer ticks.
- Register canary (`canary` command) passes for rbx rbp r12 r13 r14 r15 r10 r9.
- ELF loader: freestanding `hello.elf` runs; musl static PIE runs to completion.
- Initial stack: argc, argv, envp, NULL terminators, auxv ending `AT_NULL`,
  `AT_RANDOM` bytes, 16-byte alignment, `AT_PHDR = bias + e_phoff`.
- Syscalls: `arch_prctl(ARCH_SET_FS)`, `brk`, anonymous `mmap`, `write`,
  `exit_group`, `exit`. Others return ENOSYS silently unless strace is on.
- musl and glibc both self-relocate (`_start_c` passes `&_DYNAMIC`;
  glibc calls `_dl_relocate_static_pie`), so the kernel applies **no**
  relocations. `elf_apply_internal_relocations` is empty on purpose.
- `musl-gcc` 16.2.1 installed; QEMU 11.0.2.

## Known broken / open
- **`rdi rsi rdx r8` come back clobbered from a syscall.** Canary reads
  `......GHIJ..`. These four are caller-saved but a syscall must preserve them.
  Not yet explained. Ruled out: the canary itself, C writes to the frame
  (`user.c`/`kernel.c` only read those fields), a print in the path, and the
  push/pop lists (exact mirrors, 15 each). Claude's theory was argument
  marshalling -- matches the failing set exactly, but **the stub contains no
  such code**, so the theory is unsupported. The *values* are still unknown;
  reading them out of the guest failed twice on byte offsets.
- Static glibc still stops at VEX/AVX in `_dl_aux_init` (`vpxor`). QEMU TCG
  reports XSAVE/AVX on `-cpu max` but leaves `XCR0=1`. Not a kernel bug; a QEMU
  limitation. The AVX state setup in `fpu.S` does not help.
- Fault dump cannot be fully trusted (a `rax == cr2` reading). Worth re-checking
  against the frame layout once the register question is settled.
- No file I/O at all: no `openat`, `read`, `close`, `fstat`, `lseek`, `ioctl`.
  No fd table, no FAT/Linux directory layout. This is now the **main blocker**
  for a real binary.
- `mmap` is anonymous only: no file-backed, no `munmap`/`mremap`/`madvise`/
  `mprotect`. Needed for `ld.so`.
- No `set_tid_address`, `rt_sigaction`, `futex`, `clone`, threads, signals.
- No `PT_INTERP` support, so no dynamic linking.
- ET_EXEC binaries linked at `0x400000` collide with the loader's window.
- `mc.png` is 1,215,182 bytes; embedded VFS caps a file at 1 MiB. Needs FAT
  disk or resizing.
- GDB via `gdb()` in `tools/qemu_harness.py` is unreliable (prepends `print `,
  connection times out). Do not trust a negative from it.

## Next
1. Find out **what** `rdi/rsi/rdx/r8` become. Print them in-program as ASCII
   hex (binary through a NUL-terminated `write()` is what defeated me twice).
   Then either fix the return path or prove the canary is wrong again.
2. `openat`/`read`/`close` plus an fd table -- this is what "a real Linux
   binary" now means.
3. File-backed `mmap` and `munmap`, then `PT_INTERP` and `ld.so`.
4. `set_tid_address` before anything with threads gets serious.

## How to build and run musl
Build (true static PIE; plain `-static` gives ET_EXEC at 0x400000 and collides):

    musl-gcc -nostartfiles -static -Wl,-pie -Wl,--no-dynamic-linker \
        -o rootfs/bin/musl.elf /usr/lib/musl/lib/rcrt1.o /usr/lib/musl/lib/crti.o \
        m.c /usr/lib/musl/lib/crtn.o
    make -B klye.iso

Then from the Klye shell: `strace`, then `elf /bin/musl.elf`.

Test programs live in `/tmp/opencode/`. `m7.elf` and `musl.elf` are static PIEs
that get past TLS; the current `rootfs/bin/musl.elf` is the `main`+`return 42`
one. **Boot the ISO by hand and type the commands** -- `tr.py` has been sending
keystrokes that never reach the shell, and a blank log means the harness failed,
not that the kernel failed.
