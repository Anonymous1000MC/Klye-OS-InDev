/* hello.c - the first program loaded from a file rather than linked into the
 * kernel.
 *
 * Deliberately freestanding.  A program linked against a C library cannot run
 * yet: glibc and musl both call arch_prctl, set_tid_address, brk and futex
 * before main, and there is no signal handling, no futex and no dynamic linker
 * here.  Getting that far is real work and it is listed in TODO.md, but it is
 * not this file's job, and pretending otherwise by writing a program that
 * needs a library would only produce a fault inside libc with nothing to say
 * why.
 *
 * So this talks to the kernel directly, the way the ring 3 test does, and the
 * only thing being proved is the loader: that a file on the filesystem can
 * become a running program in ring 3.
 *
 * Build it with a position independent base so the loader has to do real work:
 *   gcc -static-pie -nostdlib -fno-stack-protector -O2 \
 *       -o hello.elf hello.c -Wl,-e,_start
 */

static long sys_write(int fd, const char *buf, unsigned long count)
{
    long result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(count)
                     : "rcx", "r11", "memory");
    return result;
}

static void sys_exit(int code)
{
    __asm__ volatile("syscall"
                     :
                     : "a"(60L), "D"((long)code)
                     : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static unsigned long strlen(const char *s)
{
    unsigned long n = 0;

    while (s[n] != 0) {
        n++;
    }
    return n;
}

static void say(const char *s)
{
    sys_write(1, s, strlen(s));
}

/* _start, not main: there is no C runtime to call main, and the entry point
 * has to be one the kernel can jump to directly. */
void _start(void)
{
    say("hello from a loaded ELF program\n");
    say("  this text was read off the filesystem\n");
    sys_exit(0);
}
