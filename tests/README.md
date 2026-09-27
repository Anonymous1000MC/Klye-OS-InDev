# Host tests

`libc_test.c` links against the kernel's own `libc.c` and checks the parts
that have no libc to compare against on the host. Run it with:

    gcc -std=c11 -O1 -I../include -ffreestanding -c ../libc.c -o libc_test.o
    gcc -std=c11 -O1 -c libc_test.c -o libctest.o
    gcc libc_test.o libctest.o -o libctest -lm
    ./libctest

The printf expectations were taken from glibc so the kernel's hand written
formatter stays compatible with what Lua expects.
