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

# the filesystem.  The heap stub stands in for the real one, and the point is
# the write path: every app in /bin is baked into the ISO, so the guest's own
# kpm build was never exercised until something needed it to work.
$cc $cflags -ffreestanding -c "$here/../vfs.c" -o "$out/vfs.o"
$cc $cflags -c "$here/vfs_test.c" -o "$out/vfs_test.o"
# the baked in image, when the build has produced one, so the test sees the
# same filesystem the kernel boots with rather than a freshly seeded one
vfs_image=""
if [ -f "$here/../build/vfs_image.c" ]; then
    $cc $cflags -c "$here/../build/vfs_image.c" -o "$out/vfs_image.o"
    vfs_image="$out/vfs_image.o"
fi
$cc "$out/vfs.o" $vfs_image "$out/stubs.o" "$out/vfs_test.o" \
    -o "$out/vfs_test" -lm
if ! "$out/vfs_test"; then
    echo "vfs_test FAILED"
    status=1
fi

# the untextured 3D view, checked against a real level when one is available
if [ -f "$WAD" ]; then
    $cc $cflags -ffreestanding -c "$here/../doom_level.c" -o "$out/doom_level.o"
    $cc $cflags -ffreestanding -c "$here/../doom3d.c" -o "$out/doom3d.o"
    $cc $cflags -c "$here/doom3d_test.c" -o "$out/doom3d_test.o"
    $cc "$out/wad.o" "$out/doom_level.o" "$out/doom3d.o" \
        "$out/stubs.o" "$out/doom3d_test.o" -o "$out/doom3d_test" -lm
    if ! "$out/doom3d_test" "$WAD"; then
        echo "doom3d_test FAILED"
        status=1
    fi
else
    echo "skipping doom3d_test: no WAD at $WAD"
fi

# Blending, pinned at the two ends where an inverted alpha argument hides.
$cc $cflags -c "$here/gfx_test.c" -o "$out/gfx_test.o"
$cc $cflags -c "$here/../gfx.c" -o "$out/gfx.o"
$cc "$out/gfx_test.o" "$out/gfx.o" "$out/stubs.o" -o "$out/gfx_test"
if ! "$out/gfx_test"; then
    echo "gfx_test FAILED"
    status=1
fi

# The settings store: clamping on the way in and out, unknown keys tolerated,
# a save that truncates.  Stubbed filesystem, so this is the store and not the
# VFS under it.
$cc $cflags -c "$here/settings_test.c" -o "$out/settings_test.o"
$cc $cflags -c "$here/../settings.c" -o "$out/settings.o"
$cc "$out/settings_test.o" "$out/settings.o" -o "$out/settings_test"
if ! "$out/settings_test"; then
    echo "settings_test FAILED"
    status=1
fi

# The 8x16 font.  The properties here are ones a wrong table offset still
# passes -- letters that have ink, glyphs that are not blank -- and the ones it
# fails: a capital with no ink in the top half is a table shifted by a row, and
# looks like text while sitting half a line below where it belongs.
$cc $cflags -c "$here/font8x16_test.c" -o "$out/font8x16_test.o"
$cc $cflags -c "$here/../font8x16.c" -o "$out/font8x16.o"
$cc "$out/font8x16_test.o" "$out/font8x16.o" "$out/stubs.o" \
    -o "$out/font8x16_test"
if ! "$out/font8x16_test"; then
    echo "font8x16_test FAILED"
    status=1
fi

# PNG decoding, against a decode of the wallpaper made with zlib.  The window
# bug this covers only shows up on an image larger than the 32 KiB DEFLATE
# back reference window, so the small fixtures that came before it were all
# green while every real wallpaper came out black below the fold.
$cc $cflags -c "$here/png_test.c" -o "$out/png_test.o"
$cc $cflags -c "$here/../png.c" -o "$out/png.o"
$cc $cflags -c "$here/../inflate.c" -o "$out/inflate.o"
$cc "$out/png_test.o" "$out/png.o" "$out/inflate.o" "$out/stubs.o" \
    -o "$out/png_test"
if ! "$out/png_test" "$here/../rootfs/home/klye/picture.png"; then
    echo "png_test FAILED"
    status=1
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
