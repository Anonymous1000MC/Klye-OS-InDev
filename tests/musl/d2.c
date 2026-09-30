/* Loop counter with NO syscalls at all.  If this is also wrong, the register
 * preservation is broken for something that never enters the kernel, and the
 * bug is not in the syscall path. */
static void put(const char *s){ char *p=(char*)s; while(*p){ __asm__ volatile("syscall"::"a"(1L),"D"(1L),"S"(p),"d"(1L):"rcx","r11","memory"); p++; } }
static void dec(long n){ char t[24]; int k=0,i; if(n<0){t[k++]='-';n=-n;} if(!n)t[k++]='0';
  while(n){t[k++]='0'+n%10;n/=10;} for(i=0;i<k;i++) put(&t[i]); }
int main(void)
{
    long plain = 7;                 /* no syscall involved */
    dec(plain); put("\n");
    for (int i = 0; i < 3; i++) { dec(i); put("\n"); }  /* loop, no syscall */
    return 42;
}
