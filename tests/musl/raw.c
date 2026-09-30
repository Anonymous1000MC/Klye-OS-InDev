#include <fcntl.h>
#include <unistd.h>

/* No stdio at all.  If these work, the kernel's fd table, open, read, write
 * and close are correct and anything wrong above is the library. */
int main(void)
{
    char buf[128];
    int fd, n;

    write(1, "A open: ", 8);
    fd = open("/etc/motd", O_RDONLY);
    write(1, "B fd: ", 6);
    n = fd;
    /* print fd as two hex-ish chars without stdio */
    buf[0] = '0' + (n / 10) % 10; buf[1] = '0' + n % 10; buf[2] = '\n';
    write(1, buf, 3);

    write(1, "C read: ", 8);
    n = read(fd, buf, 40);
    buf[40] = 0;
    write(1, buf, n > 40 ? 40 : n);
    write(1, "\n", 1);

    close(fd);
    write(1, "D done\n", 7);
    return 42;
}
