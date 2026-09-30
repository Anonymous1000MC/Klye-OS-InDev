/* A syscall that returns immediately in C: syscall 999 (ENOSYS, no output).
 * If holding a value across THIS corrupts it, the bug is in the syscall entry
 * or return, not in any particular handler. */
static void put1(const char *s){ __asm__ volatile("syscall"::"a"(1L),"D"(1L),"S"(s),"d"(1L):"rcx","r11","memory"); }
static void dec(long n){ char t[24]; int k=0,i; if(n<0){t[k++]='-';n=-n;} if(!n)t[k++]='0';
  while(n){t[k++]='0'+n%10;n/=10;} for(i=0;i<k;i++) put1(&t[i]); }
static long sys999(void){ long r; __asm__ volatile("syscall":"=a"(r):"a"(999L):"rcx","r11","memory"); return r; }
int main(void)
{
    long c = 5;
    long rc = sys999();
    put1("|"); dec(c); put1(" rc="); dec(rc); put1("\n");
    return 42;
}
