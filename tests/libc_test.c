#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern double strtod(const char *t, char **e);
extern int snprintf(char *b, unsigned long n, const char *f, ...);
extern unsigned long strlen(const char *s);
int main(void) {
    struct { const char *in; double want; } cases[] = {
        {"0", 0.0}, {"1", 1.0}, {"3.5", 3.5}, {"-2.25", -2.25},
        {"1e3", 1000.0}, {"1.5e2", 150.0}, {"2.5e-1", 0.25},
        {"0.1", 0.1}, {"123.456", 123.456}, {"-0.5", -0.5},
        {".5", 0.5}, {"7.", 7.0}, {"1e-3", 0.001},
    };
    int fails = 0, n = sizeof(cases)/sizeof(cases[0]);
    for (int i = 0; i < n; i++) {
        char *end = 0;
        double got = strtod(cases[i].in, &end);
        double diff = got - cases[i].want;
        if (diff < 0) diff = -diff;
        if (diff > 1e-9) { printf("strtod FAIL %-10s got %f want %f\n", cases[i].in, got, cases[i].want); fails++; }
    }
    char buf[128];
    struct { const char *fmt; char *want; } f[] = {
        {"%d", "42"}, {"%+d", "+7"}, {"%05d", "00042"}, {"%x", "ff"},
        {"%X", "FF"}, {"%u", "12345"}, {"%s", "hey"}, {"%c", "Q"},
        {"%d-%s", "3-abc"}, {"%08.3f", "0002.500"}, {"%.2f", "1.25"}, {"%8.3f", "   2.500"},
        {"%f", "1.500000"}, {"%ld", "1000000"}, {"%lld", "-5000000000"},
        {"%o", "17"}, {"%e", "1.000000e+00"}, {"%-6d|", "-42   |"},
        {"%6d|", "    42|"}, {"%+.1f", "+1.5"}, {"%%", "%"},
    };
    int m = sizeof(f)/sizeof(f[0]);
    for (int i = 0; i < m; i++) {
        if      (strcmp(f[i].fmt,"%d")==0)     snprintf(buf,sizeof buf,f[i].fmt,42);
        else if (strcmp(f[i].fmt,"%+d")==0)    snprintf(buf,sizeof buf,f[i].fmt,7);
        else if (strcmp(f[i].fmt,"%05d")==0)   snprintf(buf,sizeof buf,f[i].fmt,42);
        else if (strcmp(f[i].fmt,"%x")==0)     snprintf(buf,sizeof buf,f[i].fmt,255);
        else if (strcmp(f[i].fmt,"%X")==0)     snprintf(buf,sizeof buf,f[i].fmt,255);
        else if (strcmp(f[i].fmt,"%u")==0)     snprintf(buf,sizeof buf,f[i].fmt,12345u);
        else if (strcmp(f[i].fmt,"%s")==0)     snprintf(buf,sizeof buf,f[i].fmt,"hey");
        else if (strcmp(f[i].fmt,"%c")==0)     snprintf(buf,sizeof buf,f[i].fmt,'Q');
        else if (strcmp(f[i].fmt,"%d-%s")==0)  snprintf(buf,sizeof buf,f[i].fmt,3,"abc");
        else if (strcmp(f[i].fmt,"%08.3f")==0) snprintf(buf,sizeof buf,f[i].fmt,2.5);
        else if (strcmp(f[i].fmt,"%8.3f")==0)  snprintf(buf,sizeof buf,f[i].fmt,2.5);
        else if (strcmp(f[i].fmt,"%.2f")==0)   snprintf(buf,sizeof buf,f[i].fmt,1.25);
        else if (strcmp(f[i].fmt,"%f")==0)     snprintf(buf,sizeof buf,f[i].fmt,1.5);
        else if (strcmp(f[i].fmt,"%ld")==0)    snprintf(buf,sizeof buf,f[i].fmt,1000000L);
        else if (strcmp(f[i].fmt,"%lld")==0)   snprintf(buf,sizeof buf,f[i].fmt,-5000000000LL);
        else if (strcmp(f[i].fmt,"%o")==0)     snprintf(buf,sizeof buf,f[i].fmt,15);
        else if (strcmp(f[i].fmt,"%e")==0)     snprintf(buf,sizeof buf,f[i].fmt,1.0);
        else if (strcmp(f[i].fmt,"%-6d|")==0)  snprintf(buf,sizeof buf,f[i].fmt,-42);
        else if (strcmp(f[i].fmt,"%6d|")==0)   snprintf(buf,sizeof buf,f[i].fmt,42);
        else if (strcmp(f[i].fmt,"%+.1f")==0)  snprintf(buf,sizeof buf,f[i].fmt,1.5);
        else                                    snprintf(buf,sizeof buf,"%%");
        if (strcmp(buf, f[i].want) != 0) { printf("snprintf FAIL %-9s got [%s] want [%s]\n", f[i].fmt, buf, f[i].want); fails++; }
    }
    /* string functions */
    if (strlen("hello") != 5) { printf("strlen FAIL\n"); fails++; }
    if (strcmp("abc","abd") >= 0) { printf("strcmp FAIL\n"); fails++; }
    if (strchr("hello",'l') == 0) { printf("strchr FAIL\n"); fails++; }
    if (strrchr("hello",'l') == 0) { printf("strrchr FAIL\n"); fails++; }
    if (strstr("hello world","wor") == 0) { printf("strstr FAIL\n"); fails++; }
    if (atoi("-123") != -123) { printf("atoi FAIL\n"); fails++; }
    if (strtol("0x1F",0,16) != 31) { printf("strtol hex FAIL\n"); fails++; }
    printf(fails ? "FAILURES: %d\n" : "all libc checks passed (%d failures)\n", fails);
    return 0;
}
