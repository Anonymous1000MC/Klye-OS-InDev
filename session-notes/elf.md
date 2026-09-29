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

## Scope: how far from real Linux programs

An outside assessment put this nearer than my own 55% guess, and the evidence
backs it up. The 55% conflated "runs ordinary static programs" with "runs
everything"; those are different questions.

- **Static, no threads/signals: ~75-80% done.** Remaining: .rela.dyn
  relocations, the initial stack layout with a populated auxv, arch_prctl for
  the FS base, and a brk bump allocator. After that a static binary is a
  native program making raw syscalls.
- **Threads, signals, dynamic linking: ~20-25% done.** clone, futex atomics,
  signal frame delivery, PT_INTERP and shared-object dependencies. A separate
  universe.

### Relocations, measured on a real target

Built a glibc `-static-pie` binary to develop against, since there is no musl
on this host:

    1125 relocations in .rela.dyn
    1104 R_X86_64_RELATIVE
      21 R_X86_64_IRELATIVE   (readelf prints the type as "IRELATIV")

Both passes are needed. The ordering the outside advice gave is confirmed
empirically rather than assumed:

- all 1104 RELATIVE entries come first (rows 0..1103), then all 21 IRELATIVE
  (rows 1104..1124). The compiler already groups them, so a single in-order
  pass would happen to work -- but two explicit passes is the thing to rely on.
- all 21 IRELATIVE target addresses fall **inside** the RELATIVE span: they
  patch the same .data.rel.ro/.got region. An IFUNC resolver that reads a
  global through one of those slots sees an unrelocated pointer if RELATIVE has
  not already run, so the dependency is real and not a formality.

### Correction: the kernel must NOT apply .rela.dyn

An outside assessment recommended implementing R_X86_64_RELATIVE and
R_X86_64_IRELATIVE in the kernel as the first build step. That would be wrong
for a static PIE, and checking before implementing saved a large amount of work.

The glibc `-static-pie` target relocates **itself**:

    0000000000024b20 <_dl_relocate_static_pie>
    _dl_relocate_static_pie_ifunc
    __libc_start_main:
      c5a4: call 24b20 <_dl_relocate_static_pie>

`_dl_relocate_static_pie` is called from `__libc_start_main`, so the C runtime
walks its own .rela.dyn after the kernel has mapped the segments and before it
calls main. Linux's own kernel does not process relocations for ET_DYN static
PIEs for exactly this reason.

So applying them in elf.c would mean every one of the 1125 entries is applied
once by the kernel and once again by glibc, the second application being wrong
by the load bias. `elf_apply_internal_relocations()` stays empty, and now the
comment says why with evidence rather than assertion.

This makes the remaining work to a static libc binary smaller than it looks:
auxv, arch_prctl for FS, and brk. No relocation pass at all.

## Progress: a real glibc static-pie binary now starts

A glibc `-static-pie` binary (944 KiB, 1125 relocations) now loads, gets a
proper initial stack, and gets as far as executing glibc's own startup code.
It has not reached main().

Reached so far: mapped, entered, running at image offset 0xC584, with a mapped
stack. It then faults on a wild pointer (0xFFF4B64F50) inside what is almost
certainly `_dl_relocate_static_pie`, before issuing a single syscall. The value
looks like a load bias computed against a link-time address, so the next thing
to check is how the bias is derived at runtime -- not the relocation loop, which
the binary does itself.

### Fixed along the way

- **Initial stack with a real auxv.** argc/argv/envp, the vector terminated by
  AT_NULL, AT_RANDOM's sixteen bytes, and PATH. A library walks all of it with
  no bounds, so every terminator has to be exactly where it is expected.
- **The heap was mapped over the stack.** `heap_low` was computed from the
  *entry pointer*, which points into the middle of the stack region, rather than
  the top of it. A program that grew into it would have overwritten its own
  arguments.
- **AT_PHDR pointed at a page, not the table.** It must be bias + e_phoff. A
  library handed the page reads the ELF header as fourteen program headers, so
  e_phnum comes back as the first bytes of the magic. This did not change the
  outcome here, but it was wrong independently and would have broken the first
  libc that got past relocation.
- **arch_prctl(ARCH_SET_FS) and brk**, so the thread pointer and heap exist.
  MSR_FS_BASE is applied at both ring transitions rather than written once:
  it is per-thread hardware state the CPU does not save across a switch by
  itself, so a value set at startup would drift as soon as a second program ran
  with a different stack. The wrmsr on the return path is placed *before* rcx
  is loaded, because rcx is where sysretq takes its instruction pointer from.

### The file size ceiling was 64 KiB, and it was in two places

This is the bug behind "why is my wallpaper too big", and it was never one bug:

1. `tools/mkvfs.c` wrote each file's size in **two bytes**. A 944,416-byte
   binary was recorded as 26,912 -- its low sixteen bits -- and the kernel then
   read that many bytes and treated the rest of the real content as the next
   record's header. Nothing reported an error, because from the kernel's side
   the file it built *was* the size the header said. Now four bytes on both
   sides.
2. `VFS_MAX_BLOCKS_PER_FILE` capped any one file at 256 KiB, so a file over
   that was dropped from the mount entirely. Now 1 MiB.
3. `tools/mkvfs.c` read file bodies into `unsigned char buffer[MAX_BODY]` on the
   stack. Raising the cap turned that into a 2 MiB local and the tool died with
   a segfault and no message. Now allocated to the file's actual size.
4. `MAX_BODY` itself was 12 KiB, so anything larger was silently truncated on
   the way in -- the original wallpaper failure.

The recurring lesson, now four times: a limit that quietly truncates looks
exactly like a limit that works, and every test asset happened to be under it.

## The AVX wall, and what it is not

A glibc `-static-pie` binary now runs its own startup: it walks envp, runs
_dl_relocate_static_pie's 1125 self-relocations, and runs __libc_setup_tls. It
then faults in _dl_aux_init on

    262e5:  c5 f9 ef c0    vpxor %xmm0,%xmm0,%xmm0

a VEX-encoded instruction. Three separate bugs in my own attempt to enable AVX
state, each found by reading the disassembly of what I had actually built:

1. **`xsetbv` was being assembled as `xgetbv`.** They share the opcode
   `0F 01 D0` and differ only in a REX.W prefix: `48 0F 01 D0` writes, `0F 01
   D0` reads. Emitted bare, it read XCR0 and discarded the result, enabling
   nothing at all -- and looking like a correct enable in the source.
2. **The feature bit was tested in the wrong register.** CPUID puts the
   feature flags in ECX and EDX; EAX holds the highest leaf. Testing EAX
   tested the number 1 against bit 26, which is always false, so the guard
   skipped the AVX enable every single time.
3. **`cpuid` clobbers RBX** and it was not saved, in a function where RBX is
   callee-saved.

With all three fixed the enable genuinely runs: CR4 goes from 0x2A0 to 0x402A0,
OSXSAVE is set, and reading XCR0 back gives **1**.

XCR0 = 1 is x87 alone. So the CPU accepted the instruction and kept only bit 0.
QEMU's TCG does not honour the request on this host, whatever CPU model is
selected: qemu64 reports XSAVE clear (so xsetbv is itself an invalid opcode
and the machine dies before the first serial byte, with no message), and
`-cpu max` reports XSAVE and AVX present and still leaves XCR0 at 1.

The three fixes are correct and stay -- they would work on hardware that
honours the request. The wall is the emulator, not the kernel.

### This is not a test-method problem, as first assumed

The first guess was that the binary was at fault: that a plain `gcc
-static-pie` inherits the host's AVX baseline, and that rebuilding with
`-march=x86-64 -mno-avx` would remove the VEX instructions. It does not.
The prebuilt glibc objects in libc.a carry them, not the code being compiled:

    $ objdump -d glibc.elf | grep -coE '\bv[a-z0-9]+ '
    7657

A stock static glibc binary contains thousands of VEX instructions and will
need the AVX state enabled to run at all. So there is no way to avoid this
short of building glibc from source for a baseline target.

The decision is therefore to stop on glibc and go to musl, which is small
enough to reason about and whose startup does not lean on the vector
initialisation paths. musl 1.2.6 is in the package repository; it needs to be
installed with sudo, which is not available here.

## musl, and why it is the right target

musl 1.2.6 is installed (`musl-gcc` at /usr/bin/musl-gcc). It is the system
musl, not a special build.

### Getting a binary the loader will take

Three attempts, and only the third works:

- `musl-gcc -static` gives a clean ET_EXEC with no PT_INTERP, but linked at
  0x400000, which collides with the kernel's own low mapping. The loader
  refused it with "a large page covers this address" -- correct, and the reason
  a non-PIE binary needs its link address to be free.
- `musl-gcc -static-pie` still emits PT_INTERP for /lib/ld-musl-x86_64.so.1,
  so the loader rejects it as dynamically linked.
- `musl-gcc -nostartfiles -static -Wl,-pie -Wl,--no-dynamic-linker` with
  musl's own `rcrt1.o` (which exists precisely for static PIE) gives a real
  ET_DYN, entry 0x108f, no PT_INTERP, zero VEX instructions, and exactly one
  R_X86_64_RELATIVE relocation that musl applies to itself.

The full command, for next time:

    musl-gcc -nostartfiles -static -Wl,-pie -Wl,--no-dynamic-linker \
        -o out.elf /usr/lib/musl/lib/rcrt1.o /usr/lib/musl/lib/crti.o \
        prog.c /usr/lib/musl/lib/crtn.o

### Where it gets to

Further than glibc did. musl runs its own startup, relocates itself, and
reaches __init_tls, where it calls __set_thread_area -- the arch_prctl
ARCH_SET_FS wrapper -- successfully, and then:

    1ab7:  call  __set_thread_area
    1abc:  test  %eax,%eax
    1abe:  js    1b7c          # negative: error
    1ac4:  je    1b70          # zero: a different path
    1aca:  movl  $0x2,0x38(%rbx)   <- faults

  exception : page fault, write, not present
  rip       : image offset 0x1ACA
  address   : 0x1B00000038

rbx is 0x1B00000000, which is not a link-time address and not anything the
loader produced. Two live suspects, in order:

1. musl sizes its TLS block from the program headers it finds through AT_PHDR
   and then has to get memory for it. mmap currently returns ENOSYS, and if
   musl fell back to brk for that block the result would be a break value used
   as a pointer.
2. AT_PHDR or AT_ENTRY is off, so the PT_TLS it reads is the wrong one.

The next thing to measure is what musl computes for the TLS block: AT_PHDR as
we set it, the PT_TLS header it should be pointing at, and whether mmap or brk
is what it asked for. mmap is the obvious missing piece regardless, and it is
the one thing a real program cannot do without.
