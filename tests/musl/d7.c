/* rdi across syscall 999, checked immediately, no call and no stack. */
static void put1(const char *s){ __asm__ volatile("syscall"::"a"(1L),"D"(1L),"S"(s),"d"(1L):"rcx","r11","memory"); }
int main(void)
{
    long after;
    __asm__ volatile ("mov %1, %%rdi\n\tsyscall\n\tmov %%rdi, %0"
                      : "=r"(after) : "r"(5L) : "rcx","r11","memory");
    put1(after == 5 ? "RDI=5 OK\n" : "RDI=WRONG\n");
    return 42;
}
