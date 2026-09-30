/* Isolate: hold a value across ONE syscall. */
static void put1(const char *s){ __asm__ volatile("syscall"::"a"(1L),"D"(1L),"S"(s),"d"(1L):"rcx","r11","memory"); }
static void dec(long n){ char t[24]; int k=0,i; if(n<0){t[k++]='-';n=-n;} if(!n)t[k++]='0';
  while(n){t[k++]='0'+n%10;n/=10;} for(i=0;i<k;i++) put1(&t[i]); }
int main(void)
{
    long a = 3;
    put1("|"); dec(a); put1("\n");      /* a held across zero syscalls */
    long b = 4;
    put1("|"); (void)0; dec(b); put1("\n");
    long c = 5;
    put1("|"); put1("Z"); dec(c); put1("\n");   /* ONE syscall while c is live */
    return 42;
}
