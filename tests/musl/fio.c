#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    FILE *f;
    char buf[64];
    size_t n;
    int fd;

    /* raw open + read, no stdio in the way */
    fd = open("/etc/motd", O_RDONLY);
    printf("open -> %d\n", fd);
    if (fd < 0) {
        printf("open failed\n");
        return 1;
    }
    n = (size_t)read(fd, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("read %d bytes: %s", (int)n, buf);
    } else {
        printf("read returned %d\n", (int)n);
    }
    close(fd);

    /* now the same through stdio, which is the path that needs readv */
    f = fopen("/etc/motd", "r");
    printf("fopen -> %d\n", f != 0);
    if (f != 0) {
        int c = fgetc(f);
        printf("fgetc -> %d (%c)\n", c, (c > 31 && c < 127) ? c : '?');
        fseek(f, 0, SEEK_SET);
        n = fread(buf, 1, 20, f);
        buf[n] = 0;
        printf("fread %d: %s", (int)n, buf);
        fclose(f);
    }
    printf("done\n");
    return 0;
}
