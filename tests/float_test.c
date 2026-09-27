#include <stdio.h>
extern int snprintf(char *b, unsigned long n, const char *f, ...);
static void chk(const char *fmt, double v, const char *want) {
    char b[128];
    snprintf(b, sizeof b, fmt, v);
    if (__builtin_strcmp(b, want) != 0)
        printf("MISMATCH %-8s of %-10g got [%s] want [%s]\n", fmt, v, b, want);
    else
        printf("ok       %-8s of %-10g -> [%s]\n", fmt, v, b);
}
int main(void){
    /* Lua uses "%.14g" for every number it prints */
    chk("%.14g", 0.5, "0.5");
    chk("%.14g", 1.0, "1");
    chk("%.14g", 144.0, "144");
    chk("%.14g", 0.1, "0.1");
    chk("%.14g", 1.0/3.0, "0.33333333333333");
    chk("%.14g", 1e20, "1e+20");
    chk("%.14g", 1e-20, "1e-20");
    chk("%.14g", 123456789.0, "123456789");
    chk("%.3f", 2.5, "2.500");
    chk("%.2f", 1.25, "1.25");
    chk("%f", 1.5, "1.500000");
    chk("%.3e", 1234.5, "1.234e+03");
    chk("%.1e", 0.000123, "1.2e-04");
    return 0;
}
