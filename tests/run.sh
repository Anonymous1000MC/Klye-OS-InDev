#!/bin/sh
# Build and run the host tests against the kernel's own libc.c.
#
# libc.c is freestanding and defines a few symbols that also live inside glibc
# (stdout/stderr/stdin, __errno_location, the __ctype_*_loc tables).  Linked
# as-is it interposes on glibc's copies, and every printf in the test binary
# segfaults.  So the colliding names are renamed in the object file only; the
# kernel still gets the names it expects, and the test binary keeps glibc's.
set -e

here=$(dirname "$0")
out=${TMPDIR:-/tmp}/klye-hosttest
mkdir -p "$out"

cc=${CC:-gcc}
cflags="-std=c11 -O1 -I$here/../include"

# heap and timer stand-ins, since libc.c now calls into the heap
$cc $cflags -ffreestanding -c "$here/../libc.c" -o "$out/libc.o"
$cc $cflags -c "$here/stubs.c" -o "$out/stubs.o"

objcopy \
    --redefine-sym printf=klibc_printf \
    --redefine-sym puts=klibc_puts \
    --redefine-sym stdout=klibc_stdout \
    --redefine-sym stderr=klibc_stderr \
    --redefine-sym stdin=klibc_stdin \
    --redefine-sym __errno_location=klibc_errno_location \
    --redefine-sym __ctype_b_loc=klibc_ctype_b_loc \
    --redefine-sym __ctype_tolower_loc=klibc_ctype_tolower_loc \
    --redefine-sym __ctype_toupper_loc=klibc_ctype_toupper_loc \
    "$out/libc.o" "$out/libc_renamed.o"

status=0
for t in libc_test math_test float_test; do
    if [ ! -f "$here/$t.c" ]; then
        continue
    fi
    $cc $cflags -c "$here/$t.c" -o "$out/$t.o"
    $cc "$out/libc_renamed.o" "$out/stubs.o" "$out/$t.o" -o "$out/$t" -lm
    if "$out/$t"; then
        :
    else
        echo "$t FAILED"
        status=1
    fi
done
exit $status
