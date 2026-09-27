# Host tests

`libc_test.c` links against the kernel's own `libc.c` and checks the parts
that have no libc to compare against on the host. Run it with:

    gcc -std=c11 -O1 -I../include -ffreestanding -c ../libc.c -o libc_test.o
    gcc -std=c11 -O1 -c libc_test.c -o libctest.o
    gcc libc_test.o libctest.o -o libctest -lm
    ./libctest

The printf expectations were taken from glibc so the kernel's hand written
formatter stays compatible with what Lua expects.

`tools/qemu_harness.py` boots the ISO under QEMU and drives it over QMP:
`Guest.boot()` waits for the desktop, `run([...])` types commands into the
terminal, `gdb([...])` reads guest symbols, and `ppm_pixel`/`ppm_count` inspect
a screendump.  It lives in the repo because /tmp gets cleaned.

Note that `Guest.boot()` has to wait for the splash animation to finish: before
the desktop is up the dock is empty, so a test that clicks an icon too early
finds nothing and silently does nothing.
