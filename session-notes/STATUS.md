# Status

Last updated: 2026-09-30, end of the compositor and desktop session.
HEAD `cd75cd9`, pushed, tree clean.

## The headline

Two things landed today, and they are independent of each other.

**The compositor now beats its target.** 144 fps was asked for; the incremental
path measures 228 to 230.  It was at 33.  The whole gap was damage tracking that
was computed and then ignored -- see "Performance" below, which is where the
interesting part is.

**musl reached main and returned 42**, and then a day of chasing the remaining
syscall fault ended with a real answer: the `sysretq` selector pair was being
built in `rax`, overwriting the syscall's return value.  Four separate bugs in
the return path, all in about twenty instructions, all of which presented as a
fault somewhere else entirely.

## Performance

    ibench 300      228 fps     incremental repaint, what a running desktop does
    bench 40        25 fps      full-screen recompose, the worst case

Both are correct.  `bench` deliberately damages every pixel, so 25 fps is what a
full recompose costs; a desktop that only repaints what changed is `ibench`.
The boot screen reports the measured rate rather than claiming 60, which it used
to do while the compositor was managing 33.

The 33 to 228 came from two things.  Clipping: a rectangle in `gfx.c` that
every primitive intersects against, so a window that does not intersect the
damage costs three comparisons instead of a full repaint.  And then, after the
clipping was in and nothing much had changed, per-stage counters that said
desktop icons were 1.23M cycles a frame against 540k for everything else -- an
icon with a shadowed label, drawn at bilinear glyph scaling, repainted for
damage anywhere on screen.

`STAGE` in the bench output attributes a frame.  Use it before optimising
anything here; the first guess about where the time went was wrong twice.

## The desktop

Rewritten away from a Mac-like arrangement.  The top menubar, the bottom dock
and the Apple logo are deleted, not restyled.  What is there now:

- **A taskbar.**  Opaque, along the bottom.  Launcher at the left, a labelled
  button per running window in the middle, the clock at the right.  A button
  raises its window, or lowers it if it was already in front.  A minimized
  window keeps its button: removing it would move every other button.
- **A start menu** opening upward from the launcher, so the button does not
  move.  Applications, then Settings, then the windows that are open.
- **Windows resize** in eight zones with a grab area wider than the drawn
  border, snap to the edges on the drop rather than during the drag, maximize
  from the title bar button or a double click, and restore to where they were.
- **Settings is a real program.**  A store of keys with ranges, loaded at
  startup and saved on change, in a plain `key=value` file.  It was a picture
  of a settings application with fixed numbers on it.

Verified against screendumps, not by reading the code: resize 946x523 to
1030x614, maximize to full width, the start menu with five legible items, the
clock reading 03:56.

## Still broken

**Buffered stdio hangs.**  `printf` writes its first buffer and the second
`writev` is handed a length of `-24` that gets more negative each call, with the
base walking forward 27 bytes at a time.  musl is computing
`iov_len - bytes_written` and going negative, so its `FILE` has a bad length in
it.  The kernel's own `writev` is proven correct: `e1.S` and `e2.S` in
`tests/musl/` do two calls with a caller-owned vector and succeed.

The one untested interaction left is the window-size `ioctl`.  It answers 80x24
and musl may be doing something with that which is what puts `f->nbytes` into
this state.  Try returning ENOTTY again now that other things are fixed -- it
used to make musl abort in its own startup, but that was before the return value
bug was found, so the conclusion it was based on may not hold.

## 16 of ~450 syscalls

    files     open openat read readv close lseek
    output    write writev
    memory    brk mmap            (anonymous only)
    threads   set_tid_address gettid
    other     arch_prctl ioctl rt_sigprocmask

Missing and needed next, roughly in order: `fstat`, `fcntl`, `munmap`,
`mprotect`, a writable filesystem with a Linux directory layout, `fork`,
`execve`, `wait4`, file-backed `mmap`, `PT_INTERP`.

**Static busybox** is the right next milestone -- about 40 syscalls, and it
exercises fork, exec, pipes, dup, wait, getdents and stat in one go.  Not
`pacman`: it is dynamically linked against glibc and would install binaries
that this kernel cannot run.

## Not done

- App icons are unchanged and the system apps still have Mac-flavoured chrome
  internally.  This is the last item of the desktop rewrite and is untouched.
- A taskbar button does not have a context menu, and the start menu has no
  search.
- Window snapping is edges only: no half-screen snap, no corner quadrants.

## Where the work was

    20 commits   compositor damage clipping and the 3x
    6 commits    taskbar, start menu, resize, snap, maximize
    3 commits    settings
    4 commits    the syscall return path: rbx, rax, FS base, brk/mmap overlap
    2 commits    8x16 font and bilinear glyph scaling
    1 commit     launcher icon out of range, which was a kernel panic

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

## 2026-09-30 later: file I/O landed, stdio is not fixed

**Working and verified.** A musl program with no stdio opens a file in the
image, reads it, closes it and exits 42. `open` returns a descriptor, `read`
returns real content, `close` succeeds. The register canary is 12/12 and the
host suite is 7/7.

`write(2)` also returns correctly at the kernel boundary -- strace shows
`->abcde5` for a five-byte write -- and `printf("XY\n")` does produce `XY`, and
the writev returns the right count.

**Broken: the guest reads garbage return values from a working syscall.** A
program that does `dec(write(1, "abcde", 5))` prints `6840998410471589`
instead of `5`, and the *same* wrong value for every call. The kernel side is
provably correct for the same call, so the value is being lost or replaced on
the way back into the guest between the `sysretq` and the program's read of the
return register.

The register canary passes all twelve, which is the confusing part: it issues a
bare syscall and reads the registers immediately after, with no function call in
between. A program that receives the value as a call argument and then calls
another function is exercising something the canary does not.

**One real bug found and fixed while chasing this.** The heap was mapped
starting at the *top* of the stack region and growing up, which is inside the
stack, because a stack grows down. Both regions are mapped and writable, so
nothing faulted and the register canary passed: the corruption was of program
data, not registers, which is exactly why it looked like a return-value bug.
The heap now sits below the stack, in the gap between the image and the stack
bottom.  This did not fix the return values, so the overlap was real and the
cause was not -- both were true.

**Narrowed much further, and the lead is now specific.** A program using a bare
inline `syscall` instruction -- no libc, no wrapper -- still reads a wrong
return value, so it is not musl's write(). A plain store to a fixed address
survives a syscall, and the same fixed address still holds its value after one,
so the stack and the stores are both fine: only the returned value is wrong.

The register canary now checks rax and passes, but the canary is a different
program from the failing ones.  Re-running the canary's own test *inside a
spawned ELF* fails: rax after syscall 999 is neither -38 nor -1.

The frame dump from that run is the clue worth carrying:

    TRACE rax=3e7 rdi=1 rsi=10000015d18 rdx=10000015d28 r10=10000000040

r10 is 0x10000000040, which is a *kernel* address -- and it is the address of
user_fs_base_slot itself.  The program never put that in r10, and it is wrong on
the first syscall, not just the failing one.  The frame the C dispatcher is
reading has a kernel value where the program's r10 should be, while rdi, rsi
and rdx are the program's own.  So the damage is to specific slots of the
saved frame, not to the return path as a whole, and the wrmsr that reads
user_fs_base_slot on every syscall is the thing to suspect next: it uses rax
and rdx as scratch immediately before the frame is popped.

**Still not diagnosed, and the lead worth having:** the register canary never
checks `rax`.  It is the one register a syscall is allowed to change and the
only one that carries data, and every test so far has relied on some other
register to prove the return arrived.  A test that asks for a return value and
compares it, rather than inferring it from a side effect, is the missing piece.
Suspect the frame's rax slot specifically: a syscall returning a literal
(arch_prctl -> 0) works, and one returning a loop counter (write) does not,
which is a compiler-code-shape question and not a logic one.

**Not diagnosed.** Next step is not to guess: print the return value in the
canary after a real call-and-return sequence, or find where the frame's rax
slot is read back differently from the canary's path. Everything needed to
bisect is in the tree -- `tests/` has the pattern for a host-side check, and
`usertest.S` is where a call-after-syscall canary belongs.
