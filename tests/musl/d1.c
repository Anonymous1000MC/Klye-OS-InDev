/* Does a bare writev loop survive?  Every previous failure blamed a later
 * syscall.  This is one syscall in a loop with nothing else: if it fails, the
 * return path is wrong in a way that shows up on repetition, not on the first
 * call. */
static void put(const char *s){ char *p=(char*)s; while(*p){ __asm__ volatile("syscall"::"a"(1L),"D"(1L),"S"(p),"d"(1L):"rcx","r11","memory"); p++; } }
static void dec(long n){ char t[24]; int k=0,i; if(n<0){t[k++]='-';n=-n;} if(!n)t[k++]='0';
  while(n){t[k++]='0'+n%10;n/=10;} for(i=0;i<k;i++) put(&t[i]); }
int main(void)
{
    put("start\n");
    for (int i = 0; i < 5; i++) { put("x"); dec(i); put(" "); }
    put("\nend\n");
    return 42;
}
