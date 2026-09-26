/* mkvfs - build a Klye OS VFS image from a host directory tree.
 *
 * Host tool. Uses libc; never linked into the kernel.
 *
 *   usage: mkvfs <rootfs-dir> <output.c>
 *
 * Emits a C source file containing a packed image that vfs_load_image()
 * replays at mount time. Directories are always emitted before their
 * contents so the kernel can create nodes in a single forward pass.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define IMAGE_MAGIC 0x3146564BU /* "KVF1" little endian */
#define KIND_DIR 0U
#define KIND_FILE 1U
#define MAX_ENTRIES 256
#define MAX_PATH 160
#define MAX_BODY 12288

struct entry {
    char path[MAX_PATH];
    int kind;
    unsigned char *body;
    long body_len;
};

static struct entry entries[MAX_ENTRIES];
static int entry_count;

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "mkvfs: %s: %s\n", what, detail);
    exit(1);
}

static void add_entry(const char *path, int kind, const unsigned char *body,
                      long body_len)
{
    struct entry *slot;

    if (entry_count >= MAX_ENTRIES) {
        fail("too many entries", path);
    }
    if (body_len > MAX_BODY) {
        fprintf(stderr, "mkvfs: warning: %s is %ld bytes, truncated to %d\n",
                path, body_len, MAX_BODY);
        body_len = MAX_BODY;
    }
    slot = &entries[entry_count++];
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->kind = kind;
    slot->body_len = body_len;
    slot->body = NULL;
    if (body_len > 0) {
        slot->body = (unsigned char *)malloc((size_t)body_len);
        if (slot->body == NULL) {
            fail("out of memory", path);
        }
        memcpy(slot->body, body, (size_t)body_len);
    }
}

static int compare_depth(const void *a, const void *b)
{
    const struct entry *left = (const struct entry *)a;
    const struct entry *right = (const struct entry *)b;
    size_t left_len = strlen(left->path);
    size_t right_len = strlen(right->path);

    if (left->kind != right->kind) {
        return left->kind == KIND_DIR ? -1 : 1;
    }
    if (left_len != right_len) {
        return left_len < right_len ? -1 : 1;
    }
    return strcmp(left->path, right->path);
}

static char child_dir_fs[512][1024];
static char child_dir_vfs[512][MAX_PATH];
static int child_dir_count;

static void walk(const char *fs_dir, const char *prefix)
{
    DIR *dir;
    struct dirent *item;
    char names[512][256];
    int count = 0;
    int index;

    dir = opendir(fs_dir);
    if (dir == NULL) {
        fail("cannot open directory", fs_dir);
    }
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') {
            continue;
        }
        if (count >= 512) {
            fail("too many names in directory", fs_dir);
        }
        snprintf(names[count], sizeof(names[count]), "%s", item->d_name);
        ++count;
    }
    closedir(dir);

    for (index = 0; index < count; ++index) {
        char child_prefix[MAX_PATH];
        char child_fs[1024];
        struct stat info;

        if (strcmp(prefix, "/") == 0) {
            if ((size_t)snprintf(child_prefix, sizeof(child_prefix), "/%s",
                                 names[index]) >= sizeof(child_prefix)) {
                fail("path too long", names[index]);
            }
        } else if ((size_t)snprintf(child_prefix, sizeof(child_prefix), "%s/%s",
                                    prefix,
                                    names[index]) >= sizeof(child_prefix)) {
            fail("path too long", names[index]);
        }
        if ((size_t)snprintf(child_fs, sizeof(child_fs), "%s/%s", fs_dir,
                             names[index]) >= sizeof(child_fs)) {
            fail("path too long", names[index]);
        }
        if (stat(child_fs, &info) != 0) {
            fail("cannot stat", child_fs);
        }
        if (S_ISDIR(info.st_mode)) {
            add_entry(child_prefix, KIND_DIR, NULL, 0);
            if (child_dir_count >= 512) {
                fail("too many subdirectories", fs_dir);
            }
            snprintf(child_dir_fs[child_dir_count],
                     sizeof(child_dir_fs[0]), "%s", child_fs);
            snprintf(child_dir_vfs[child_dir_count],
                     sizeof(child_dir_vfs[0]), "%s", child_prefix);
            ++child_dir_count;
        } else if (S_ISREG(info.st_mode)) {
            FILE *in = fopen(child_fs, "rb");
            unsigned char buffer[MAX_BODY];
            long got;

            if (in == NULL) {
                fail("cannot read", child_fs);
            }
            got = (long)fread(buffer, 1, sizeof(buffer), in);
            fclose(in);
            add_entry(child_prefix, KIND_FILE, buffer, got);
        }
    }

    {
        char saved_fs[512][1024];
        char saved_vfs[512][MAX_PATH];
        int saved_count = child_dir_count;

        memcpy(saved_fs, child_dir_fs, sizeof(saved_fs));
        memcpy(saved_vfs, child_dir_vfs, sizeof(saved_vfs));
        child_dir_count = 0;
        for (index = 0; index < saved_count; ++index) {
            walk(saved_fs[index], saved_vfs[index]);
        }
    }
}

static void emit_bytes(FILE *out, const unsigned char *data, long length)
{
    long index;

    for (index = 0; index < length; ++index) {
        fprintf(out, "0x%02x,", data[index]);
        if ((index % 16) == 15) {
            fputc('\n', out);
        }
    }
}

static void emit_cstring(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != 0) {
        if (*p == '"' || *p == '\\') {
            fprintf(out, "\\%c", *p);
        } else if (*p < 32 || *p > 126) {
            fprintf(out, "\\%03o", *p);
        } else {
            fputc(*p, out);
        }
        ++p;
    }
}

int main(int argc, char **argv)
{
    const char *root;
    const char *output;
    FILE *out;
    int index;
    long payload = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: mkvfs <rootfs-dir> <output.c>\n");
        return 2;
    }
    root = argv[1];
    output = argv[2];

    add_entry("/", KIND_DIR, NULL, 0);
    child_dir_count = 0;
    walk(root, "/");
    qsort(entries, (size_t)entry_count, sizeof(entries[0]), compare_depth);

    for (index = 0; index < entry_count; ++index) {
        payload += 8L + (long)strlen(entries[index].path) + entries[index].body_len;
    }

    out = fopen(output, "w");
    if (out == NULL) {
        fail("cannot write", output);
    }
    fprintf(out, "/* generated by tools/mkvfs from %s - do not edit */\n", root);
    fprintf(out, "#include <stdint.h>\n\n");
    fprintf(out, "const uint32_t vfs_image_magic = 0x%08XU;\n", IMAGE_MAGIC);
    fprintf(out, "const uint32_t vfs_image_count = %dU;\n", entry_count);
    fprintf(out, "const uint32_t vfs_image_bytes = %ldU;\n\n", payload);
    fprintf(out, "const unsigned char vfs_image_data[] = {\n");
    for (index = 0; index < entry_count; ++index) {
        unsigned char header[8];
        size_t path_len = strlen(entries[index].path);

        header[0] = (unsigned char)(path_len & 0xFF);
        header[1] = (unsigned char)(path_len >> 8);
        header[2] = (unsigned char)(entries[index].body_len & 0xFF);
        header[3] = (unsigned char)((entries[index].body_len >> 8) & 0xFF);
        header[4] = (unsigned char)entries[index].kind;
        header[5] = 0;
        header[6] = 0;
        header[7] = 0;
        fputs("    /* ", out);
        emit_cstring(out, entries[index].path);
        fputs(" */\n", out);
        emit_bytes(out, header, 8);
        emit_bytes(out, (const unsigned char *)entries[index].path, (long)path_len);
        if (entries[index].body_len > 0) {
            emit_bytes(out, entries[index].body, entries[index].body_len);
        }
        fputc('\n', out);
    }
    fprintf(out, "};\n");
    fclose(out);

    fprintf(stderr, "mkvfs: %d entries, %ld bytes -> %s\n", entry_count, payload,
            output);
    for (index = 0; index < entry_count; ++index) {
        fprintf(stderr, "  %c %s (%ld bytes)\n",
                entries[index].kind == KIND_DIR ? 'd' : '-',
                entries[index].path, entries[index].body_len);
    }
    return 0;
}
