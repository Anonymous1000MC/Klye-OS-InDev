/* vfs_test.c - check the filesystem against the heap stub.
 *
 * The VFS had no host test, which is why a write path that had never worked
 * went unnoticed: every application in /bin is baked into the ISO by the build
 * system, so the guest's own kpm build was never on the path that mattered.
 */

#include <stdio.h>
#include <string.h>

#include "vfs.h"

/* The filesystem logs through the serial port, which does not exist here. */
void serial_write(const char *text)
{
    (void)text;
}

void serial_write_decimal(uint64_t value)
{
    (void)value;
}

static int failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

int main(void)
{
    static char buffer[8192];
    static char whole[70000];
    static char pattern[70000];
    int length;
    int i;

    if (!vfs_mount()) {
        printf("vfs_mount failed\n");
        return 1;
    }
    printf("  %u bytes total, %u used, %d node slots\n", vfs_bytes_total(),
           vfs_bytes_used(), vfs_node_capacity());

    /* A plain file that does not exist yet.  This is the case kpm build hits
     * and the one that was broken: create, write, read back. */
    check(vfs_exists("/bin/probe.lua") == false, "a fresh path does not exist");
    length = vfs_write("/bin/probe.lua", "hello", 5);
    check(length == 5, "write to a fresh path returns the length");
    check(vfs_exists("/bin/probe.lua"), "the file exists afterwards");
    length = vfs_read("/bin/probe.lua", buffer, (uint32_t)sizeof(buffer));
    check(length == 5, "read returns the length");
    check(memcmp(buffer, "hello", 5) == 0, "the contents match");

    /* A dotted name, because that is what every script is called. */
    check(vfs_write("/bin/other.name.lua", "abc", 3) == 3,
          "a name with two dots is accepted");

    /* Overwriting, which has to reuse the node rather than fail on it. */
    check(vfs_write("/bin/probe.lua", "hi", 2) == 2, "overwrite works");
    length = vfs_read("/bin/probe.lua", buffer, (uint32_t)sizeof(buffer));
    check(length == 2 && memcmp(buffer, "hi", 2) == 0,
          "the overwrite is what is read back");

    /* A file larger than one block, so the block chain is exercised. */
    for (i = 0; i < (int)sizeof(pattern); i++) {
        pattern[i] = (char)('a' + (i % 26));
    }
    check(vfs_write("/home/klye/big.bin", pattern, (uint32_t)sizeof(pattern)) ==
              (int)sizeof(pattern),
          "a file of many blocks can be written");
    /* read it back in one go, into a buffer the size of the file, because
     * vfs_read stops at whichever of the two runs out first */
    length = vfs_read("/home/klye/big.bin", whole, (uint32_t)sizeof(whole));
    check(length == (int)sizeof(pattern), "the large file reads back its length");
    check(length > 0 && memcmp(whole, pattern, (size_t)length) == 0,
          "the large file's contents match");
    /* and a short buffer must return a prefix, not overrun */
    length = vfs_read("/home/klye/big.bin", buffer, (uint32_t)sizeof(buffer));
    check(length == (int)sizeof(buffer),
          "a short read stops at the buffer size");
    check(memcmp(buffer, pattern, sizeof(buffer)) == 0,
          "the short read is a correct prefix");

    /* The per file ceiling, from the header. */
    check(vfs_write("/home/klye/toobig.bin", pattern,
                    (uint32_t)VFS_MAX_BLOCKS_PER_FILE * VFS_BLOCK_SIZE + 1U) < 0,
          "a file past the per file ceiling is refused");

    /* Capacity, now that the tables are on the heap. */
    check(vfs_bytes_total() >= 1024U * 1024U,
          "the filesystem is at least a megabyte");
    printf("  %u bytes total, %u used, %d node slots\n", vfs_bytes_total(),
           vfs_bytes_used(), vfs_node_capacity());

    if (failures != 0) {
        printf("vfs_test FAILED (%d)\n", failures);
        return 1;
    }
    printf("vfs_test ok\n");
    return 0;
}
