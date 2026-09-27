# Host tests

`libc_test.c`, `math_test.c` and `float_test.c` link against the kernel's own
`libc.c` and check the parts that have no libc to compare against on the host.
Run all three with:

    ./tests/run.sh

It exits nonzero if any check fails, so it is usable from a pre-commit hook.

Compiling `libc.c` for the host is not as plain as it looks. `libc.c` is
freestanding and defines `stdout`, `stderr`, `stdin`, `__errno_location` and
the `__ctype_*_loc` tables, which also live inside glibc. Linked as-is it
interposes on glibc's copies and every `printf` in the test binary segfaults,
so `run.sh` renames those symbols in the object file with `objcopy`. The kernel
still gets the names it expects; only the test build changes. `stubs.c` supplies
the heap and timer entry points that `libc.c` now calls into.

The printf expectations were taken from glibc so the kernel's hand written
formatter stays compatible with what Lua expects.

`fmt_diff.c` is a differential test: it holds the kernel's formatter and
glibc's in the same binary and compares 1440 value/format pairs. 63 of them
still disagree, all for reasons that need exact big-integer decimal
conversion; the file explains both classes. It fails only when the count goes
*above* that baseline, so the known gaps stay visible without blocking work
while a regression is still caught.

`tools/qemu_harness.py` boots the ISO under QEMU and drives it over QMP:
`Guest.boot()` waits for the desktop, `run([...])` types commands into the
terminal, `gdb([...])` reads guest symbols, and `ppm_pixel`/`ppm_count` inspect
a screendump.  It lives in the repo because /tmp gets cleaned.

Note that `Guest.boot()` has to wait for the splash animation to finish: before
the desktop is up the dock is empty, so a test that clicks an icon too early
finds nothing and silently does nothing.
