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

# a second copy with snprintf renamed, so the differential test can hold the
# kernel's formatter and glibc's in the same binary and compare them
objcopy \
    --redefine-sym printf=klibc_printf \
    --redefine-sym stdout=klibc_stdout \
    --redefine-sym stderr=klibc_stderr \
    --redefine-sym stdin=klibc_stdin \
    --redefine-sym __errno_location=klibc_errno_location \
    --redefine-sym __ctype_b_loc=klibc_ctype_b_loc \
    --redefine-sym __ctype_tolower_loc=klibc_ctype_tolower_loc \
    --redefine-sym __ctype_toupper_loc=klibc_ctype_toupper_loc \
    --redefine-sym snprintf=ksnprintf \
    "$out/libc.o" "$out/libc_diff.o"

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

# the wad reader, checked against a real WAD when one is available.  The name
# comparison it covers is the part that has produced two silent wrong answers.
WAD=${WAD:-$(dirname "$0")/../DOOM1.wad}
if [ -f "$WAD" ]; then
    $cc $cflags -ffreestanding -c "$here/../wad.c" -o "$out/wad.o"
    $cc $cflags -c "$here/wad_test.c" -o "$out/wad_test.o"
    $cc "$out/wad.o" "$out/stubs.o" "$out/wad_test.o" -o "$out/wad_test" -lm
    if ! "$out/wad_test" "$WAD"; then
        echo "wad_test FAILED"
        status=1
    fi
else
    echo "skipping wad_test: no WAD at $WAD"
fi

# differential test against glibc; see the comment in fmt_diff.c for the two
# known classes of disagreement and why the baseline is not zero
$cc $cflags -c "$here/fmt_diff.c" -o "$out/fmt_diff.o"
$cc "$out/libc_diff.o" "$out/stubs.o" "$out/fmt_diff.o" -o "$out/fmt_diff" -lm
if ! "$out/fmt_diff"; then
    echo "fmt_diff FAILED"
    status=1
fi
exit $status
