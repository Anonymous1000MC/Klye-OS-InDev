# Klye OS

A hobby x86-64 operating system: a real multitasking kernel with a graphical
desktop, built from scratch in C and x86 assembly, and working far enough into
the Linux userspace contract to run static musl binaries to completion.

**Status: in development.** A desktop environment boots and is usable. Static
musl programs run. General Linux binary support is a work in progress — see
[What works](#what-works) and [What doesn't](#what-doesnt-work-yet) below.

---

## What works

- **Boots to a graphical desktop** with a window manager, a compositor
  (damage-rectangle based, 60 fps), and a text shell.
- **Ring 3 with real privilege separation** — user page tables, a DPL-3 syscall
  gate, `SYSCALL`/`SYSRETQ`, and user programs running as scheduler tasks that
  survive timer ticks.
- **An ELF64 loader** that runs static PIE binaries: `PT_LOAD` mapping, a
  correct initial stack (`argc`/`argv`/`envp`/auxv, 16-byte aligned), and
  thread-local storage via `arch_prctl`.
- **Static musl binaries run to completion.** A true static PIE gets through
  self-relocation, auxv parsing, TLS setup, and the syscall ABI, calls `main`,
  writes to stdout, and exits:

  ```
  TRACE rax=9e rdi=1002 rsi=10000004138 rdx=e7 ... -> 0    arch_prctl(ARCH_SET_FS)
  musl main
  TRACE rax=e7 rdi=2a ...                                exit_group(42)
  TRACE end of program
  ```

- A register canary that runs a program in ring 3 and verifies the syscall ABI
  preserves every register the kernel is required to leave alone.
- FAT16 and VFS filesystems, a WAD loader, a PNG decoder, a Lua interpreter with
  a bytecode VM, PS/2 input, PCI and virtio transport, and a Doom renderer in
  progress.

## What doesn't work yet

Listed plainly, because a list of what a kernel *cannot* do yet is more useful
than a feature count.

- **No file I/O.** No `openat`, `read`, `close`, `fstat`, `lseek` or `ioctl`, and
  no file-descriptor table. This is the main thing between this and running a
  real program.
- **No dynamic linking.** No `PT_INTERP` support, so `ld.so` cannot be loaded.
  `mmap` is anonymous only — no file-backed mapping, no `munmap`.
- **No threads or signals.** No `clone`, `futex`, `rt_sigaction`,
  `set_tid_address`.
- **`rdi`, `rsi`, `rdx` and `r8` come back clobbered from a syscall.** They are
  caller-saved but a syscall must preserve them. Everything else survives; the
  cause is still unidentified. A register canary is in the tree that proves
  this, so it cannot regress silently.
- Static glibc reaches libc startup and then stops on VEX/AVX, which QEMU's TCG
  does not honour. That is a limitation of the emulator, not of the kernel.
- ET_EXEC binaries linked at `0x400000` collide with the loader's chosen window.

## Building

Requires `gcc`, `binutils`, `qemu-system-x86_64`, and a Python 3 for the image
tools.

```sh
make klye.iso
```

Then either let the build print a QEMU invocation, or run it yourself:

```sh
qemu-system-x86_64 -cdrom klye.iso -serial stdio -display gtk -no-reboot
```

`make -B klye.iso` forces a full rebuild. Host tests, which cover the userspace
libraries, PNG/DEFLATE, the WAD parser and the math library:

```sh
./tests/run.sh
```

> The floating-point tests have a known baseline of 63 disagreements out of 1440
> cases against the host libc. That is a known quantity, not a regression.

## Trying a musl program

```sh
cat > hello.c <<'EOF'
#include <unistd.h>
int main(void) { write(1, "it works\n", 9); return 42; }
EOF

# A true static PIE.  Plain `-static` produces an ET_EXEC at 0x400000, which
# collides with the loader's mapping window.
musl-gcc -nostartfiles -static -Wl,-pie -Wl,--no-dynamic-linker \
    -o rootfs/bin/hello-musl.elf /usr/lib/musl/lib/rcrt1.o \
    /usr/lib/musl/lib/crti.o hello.c /usr/lib/musl/lib/crtn.o

make -B klye.iso
```

Boot the image and, at the Klye shell, run `strace` and then
`elf /bin/hello-musl.elf`. The `strace` output shows every syscall as it
happens, which is the fastest way to see how far a program got.

## Layout

| Path | What it is |
|---|---|
| `boot.asm` | bootloader entry, long mode, initial GDT and IDT |
| `kernel.c` | kernel main, physical memory, init, panic and fault reporting |
| `interrupts.S` | the interrupt stub and its register save |
| `ring3.S` | ring 3 entry, the syscall path, `sysretq` return |
| `scheduler.c` | tasks, context switching, the tick |
| `mmu.c` | paging, including separate user page tables |
| `elf.c` | the ELF64 loader and the initial user stack |
| `user.c` | the Linux syscall ABI surface |
| `gfx.c`, `wm.c`, `shell.c` | graphics, window manager, shell |
| `fat.c`, `vfs.c` | filesystems |
| `png.c`, `inflate.c` | PNG and DEFLATE |
| `lua_host.c`, `kby.c`, `kas.c` | Lua, its bytecode VM, and an assembler |
| `usertest.S` | the ring 3 test and the syscall register canary |
| `tests/` | host-side tests |
| `session-notes/` | working notes, including what is known broken and why |
| `third_party/` | vendored dependencies |

## Notes

`session-notes/` is deliberately committed. It records what is broken and what
has already been ruled out, which is the part that is expensive to rediscover.
`STATUS.md` is the current state; `lessons.md` is a list of mistakes worth not
repeating.

`dino.lua` under `rootfs/bin` is a user file and is kept in the tree as-is.

## License

Not yet chosen. Add one before treating this as a project others may reuse.
