#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel.h"
#include "heap.h"
#include "mmu.h"
#include "vfs.h"

struct vfs_node {
    char name[VFS_NAME_MAX];
    char path[VFS_PATH_MAX];
    int parent;
    int first_child;
    int next_sibling;
    int prev_sibling;
    int block_count;
    uint16_t blocks[VFS_MAX_BLOCKS_PER_FILE];
    uint32_t size;
    uint32_t created;
    uint32_t modified;
    bool used;
    bool is_dir;
};

/* The three tables live on the heap, sized at mount time.
 *
 * They were static arrays, which put the whole block store and the whole node
 * table in BSS whether or not anything was stored.  Raising the caps to
 * something usable then means the kernel image grows by megabytes before the
 * first file exists, so the tables move to the heap and the caps become
 * ceilings that a small guest can undershoot.
 *
 * vfs_storage is flat, block * VFS_BLOCK_SIZE + offset, rather than an array
 * of arrays, so one allocation covers it.
 */
static uint8_t *vfs_storage;
static uint8_t *vfs_block_used;   /* one byte per block, 0 free */
static struct vfs_node *vfs_nodes;
static int vfs_block_count;
static int vfs_node_count;
static bool vfs_storage_mapped;   /* storage came from the MMU, not the heap */
static char vfs_scratch[VFS_BODY_MAX];
static int vfs_used_nodes;
static uint32_t vfs_used_blocks;
static uint32_t vfs_clock;
static bool vfs_ready;

static int vfs_strlen(const char *text)
{
    int length = 0;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void vfs_strcpy(char *out, const char *text, int max)
{
    int index = 0;

    if (max <= 0) {
        return;
    }
    while (index < max - 1 && text[index] != '\0') {
        out[index] = text[index];
        ++index;
    }
    out[index] = '\0';
}

static bool vfs_streq(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != '\0' && a[index] == b[index]) {
        ++index;
    }
    return a[index] == b[index];
}

static bool vfs_prefix_equal(const char *text, const char *prefix, int length)
{
    for (int index = 0; index < length; ++index) {
        if (text[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

static bool vfs_valid_name(const char *name)
{
    int length = vfs_strlen(name);

    if (length == 0 || length >= VFS_NAME_MAX) {
        return false;
    }
    if (vfs_streq(name, ".") || vfs_streq(name, "..")) {
        return false;
    }
    for (int index = 0; index < length; ++index) {
        char character = name[index];

        if (character == '/' || character < 32) {
            return false;
        }
    }
    return true;
}

static void vfs_normalize(char *path)
{
    int read = 0;
    int write = 0;
    bool leading_slash = path[0] == '/';
    int length = vfs_strlen(path);

    while (read < length) {
        if (path[read] == '/') {
            ++read;
            continue;
        }
        {
            int start = read;

            while (read < length && path[read] != '/') {
                ++read;
            }
            if (read - start == 1 && path[start] == '.') {
                continue;
            }
            if (read - start == 2 && path[start] == '.' &&
                path[start + 1] == '.') {
                if (write > 0) {
                    --write;
                    while (write > 0 && path[write - 1] != '/') {
                        --write;
                    }
                    if (write > 0) {
                        --write;
                    }
                }
                continue;
            }
            if (write > 0) {
                path[write++] = '/';
            }
            for (int index = start; index < read; ++index) {
                path[write++] = path[index];
            }
        }
    }
    if (write == 0) {
        path[write++] = '/';
    }
    if (leading_slash && write > 1) {
        for (int index = write; index > 0; --index) {
            path[index] = path[index - 1];
        }
        path[0] = '/';
        ++write;
    }
    path[write] = '\0';
}

static void vfs_absolute(const char *path, const char *cwd, char *out, int max)
{
    char joined[VFS_PATH_MAX * 2];

    if (path[0] == '/') {
        vfs_strcpy(joined, path, (int)sizeof(joined));
    } else {
        vfs_strcpy(joined, cwd == 0 ? "/" : cwd, (int)sizeof(joined));
        if (joined[0] != '\0' && joined[vfs_strlen(joined) - 1] != '/') {
            int length = vfs_strlen(joined);

            joined[length] = '/';
            joined[length + 1] = '\0';
        }
        {
            int offset = vfs_strlen(joined);

            vfs_strcpy(joined + offset, path,
                       (int)sizeof(joined) - offset);
        }
    }
    vfs_normalize(joined);
    vfs_strcpy(out, joined, max);
}

static int vfs_alloc_node(void)
{
    for (int index = 0; index < vfs_node_count; ++index) {
        if (!vfs_nodes[index].used) {
            return index;
        }
    }
    return -1;
}

static void vfs_unlink(int index)
{
    struct vfs_node *node = &vfs_nodes[index];
    int parent = node->parent;

    if (parent >= 0 && vfs_nodes[parent].first_child == index) {
        vfs_nodes[parent].first_child = node->next_sibling;
    }
    if (node->prev_sibling >= 0) {
        vfs_nodes[node->prev_sibling].next_sibling = node->next_sibling;
    }
    if (node->next_sibling >= 0) {
        vfs_nodes[node->next_sibling].prev_sibling = node->prev_sibling;
    }
    for (int slot = 0; slot < node->block_count; ++slot) {
        int block = node->blocks[slot];

        if (block >= 0 && block < vfs_block_count && vfs_block_used[block]) {
            vfs_block_used[block] = false;
            vfs_used_blocks--;
        }
    }
    node->used = false;
    node->block_count = 0;
    node->size = 0;
    vfs_used_nodes--;
}

static int vfs_new_node(int parent, const char *name, bool directory)
{
    int index = vfs_alloc_node();
    struct vfs_node *node;
    int parent_first;

    if (index < 0) {
        return -1;
    }
    node = &vfs_nodes[index];
    /* wipe the whole slot: a reused node would otherwise keep the previous
     * name's tail bytes after the terminator, which shows up in dumps and
     * confuses anything that inspects name[] directly */
    __builtin_memset(node, 0, sizeof(*node));
    node->used = true;
    node->is_dir = directory;
    node->parent = parent;
    node->first_child = -1;
    node->next_sibling = -1;
    node->prev_sibling = -1;
    node->block_count = 0;
    node->size = 0;
    node->created = ++vfs_clock;
    node->modified = node->created;
    vfs_strcpy(node->name, name, VFS_NAME_MAX);
    if (parent < 0) {
        vfs_strcpy(node->path, "/", VFS_PATH_MAX);
    } else {
        char joined[VFS_PATH_MAX * 2];

        vfs_strcpy(joined, vfs_nodes[parent].path, (int)sizeof(joined));
        if (vfs_strlen(joined) > 1) {
            int length = vfs_strlen(joined);

            joined[length] = '/';
            joined[length + 1] = '\0';
        }
        vfs_strcpy(joined + vfs_strlen(joined), name, (int)sizeof(joined));
        vfs_strcpy(node->path, joined, VFS_PATH_MAX);
    }
    parent_first = parent < 0 ? -1 : vfs_nodes[parent].first_child;
    node->next_sibling = parent_first;
    if (parent_first >= 0) {
        vfs_nodes[parent_first].prev_sibling = index;
    }
    if (parent >= 0) {
        vfs_nodes[parent].first_child = index;
    }
    vfs_used_nodes++;
    return index;
}

static int vfs_child_named(int parent, const char *name, int length)
{
    for (int child = vfs_nodes[parent].first_child; child >= 0;
         child = vfs_nodes[child].next_sibling) {
        if ((int)vfs_strlen(vfs_nodes[child].name) == length &&
            vfs_prefix_equal(vfs_nodes[child].name, name, length)) {
            return child;
        }
    }
    return -1;
}

static int vfs_walk(const char *absolute)
{
    int current = 0;
    int index = 0;
    int length = vfs_strlen(absolute);

    if (length <= 1) {
        return vfs_nodes[0].used ? 0 : -1;
    }
    index = 1;
    while (index < length) {
        int start = index;
        int next;

        while (index < length && absolute[index] != '/') {
            ++index;
        }
        next = vfs_child_named(current, absolute + start, index - start);
        if (next < 0) {
            return -1;
        }
        current = next;
        while (index < length && absolute[index] == '/') {
            ++index;
        }
    }
    return current;
}

static int vfs_block_alloc(void)
{
    for (int block = 0; block < vfs_block_count; ++block) {
        if (!vfs_block_used[block]) {
            vfs_block_used[block] = true;
            vfs_used_blocks++;
            return block;
        }
    }
    return -1;
}

static uint8_t *vfs_file_pointer(int index, uint32_t offset)
{
    int slot = (int)(offset / VFS_BLOCK_SIZE);

    if (slot >= vfs_nodes[index].block_count) {
        return 0;
    }
    return &vfs_storage[(size_t)vfs_nodes[index].blocks[slot] * VFS_BLOCK_SIZE +
                        (offset % VFS_BLOCK_SIZE)];
}

static bool vfs_file_reserve(int index, uint32_t needed)
{
    struct vfs_node *node = &vfs_nodes[index];
    uint32_t wanted = (needed + VFS_BLOCK_SIZE - 1U) / VFS_BLOCK_SIZE;

    if (wanted > VFS_MAX_BLOCKS_PER_FILE) {
        return false;
    }
    while (node->block_count < (int)wanted) {
        int block = vfs_block_alloc();

        if (block < 0) {
            return false;
        }
        node->blocks[node->block_count++] = (uint16_t)block;
    }
    return true;
}

static int vfs_seed(const char *path, const char *body)
{
    char absolute[VFS_PATH_MAX * 2];

    vfs_absolute(path, "/", absolute, (int)sizeof(absolute));
    {
        int index = vfs_create(absolute, false);

        if (index >= 0 && body != 0) {
            (void)vfs_write(absolute, body, (uint32_t)vfs_strlen(body));
        }
        return index;
    }
}

static void vfs_seed_directory(const char *path)
{
    char absolute[VFS_PATH_MAX * 2];

    vfs_absolute(path, "/", absolute, (int)sizeof(absolute));
    (void)vfs_create(absolute, true);
}

void vfs_format(void)
{
    for (int index = 0; index < vfs_node_count; ++index) {
        vfs_nodes[index].used = false;
        vfs_nodes[index].first_child = -1;
        vfs_nodes[index].next_sibling = -1;
        vfs_nodes[index].prev_sibling = -1;
        vfs_nodes[index].parent = -1;
        vfs_nodes[index].block_count = 0;
        vfs_nodes[index].size = 0;
    }
    for (int block = 0; block < vfs_block_count; ++block) {
        vfs_block_used[block] = false;
    }
    vfs_used_nodes = 0;
    vfs_used_blocks = 0;
    vfs_clock = 0;
    (void)vfs_new_node(-1, "/", true);
    vfs_strcpy(vfs_nodes[0].path, "/", VFS_PATH_MAX);
    vfs_nodes[0].first_child = -1;
    vfs_ready = true;
}

extern const uint32_t vfs_image_magic;
extern const uint32_t vfs_image_count;
extern const uint32_t vfs_image_bytes;
extern const unsigned char vfs_image_data[];

#define VFS_IMAGE_MAGIC 0x3146564BU
#define VFS_IMAGE_KIND_DIR 0U
#define VFS_IMAGE_KIND_FILE 1U

static bool vfs_load_image(void)
{
    const unsigned char *cursor = vfs_image_data;
    uint32_t total = vfs_image_bytes;
    uint32_t count = vfs_image_count;
    uint32_t index;
    uint32_t skipped = 0;
    /* Scratch for file bodies.  This used to be a 4 KiB stack buffer, which
     * meant any file larger than that aborted the entire mount: one oversized
     * file left the filesystem half built with no indication why. */
    char *scratch = (char *)(uintptr_t)heap_malloc(
        (size_t)VFS_MAX_BLOCKS_PER_FILE * VFS_BLOCK_SIZE);

    if (vfs_image_magic != VFS_IMAGE_MAGIC) {
        heap_free(scratch);
        return false;
    }
    for (index = 0; index < count; ++index) {
        uint32_t path_len;
        uint32_t body_len;
        uint32_t kind;
        char path[VFS_PATH_MAX];

        if ((uint32_t)(vfs_image_data + total - cursor) < 8U) {
            heap_free(scratch);
            return false;
        }
        path_len = (uint32_t)cursor[0] | ((uint32_t)cursor[1] << 8);
        body_len = (uint32_t)cursor[2] | ((uint32_t)cursor[3] << 8);
        kind = cursor[4];
        cursor += 8;
        if ((uint32_t)(vfs_image_data + total - cursor) < path_len + body_len) {
            heap_free(scratch);
            return false;
        }
        if (path_len == 0U || path_len >= sizeof(path)) {
            heap_free(scratch);
            return false;
        }
        __builtin_memcpy(path, cursor, (size_t)path_len);
        path[path_len] = 0;
        cursor += path_len;
        if (vfs_streq(path, "/")) {
            cursor += body_len;
            continue;
        }
        if (kind == VFS_IMAGE_KIND_DIR) {
            if (vfs_create(path, true) < 0) {
                return false;
            }
        } else if (body_len >
                   (uint32_t)VFS_MAX_BLOCKS_PER_FILE * VFS_BLOCK_SIZE ||
                   scratch == 0) {
            /* one file the filesystem cannot hold must not cost us the whole
             * mount, so drop it and keep going */
            ++skipped;
        } else {
            int node = vfs_create(path, false);

            if (node < 0) {
                ++skipped;
            } else if (body_len != 0U) {
                __builtin_memcpy(scratch, cursor, (size_t)body_len);
                if (vfs_write(path, scratch, body_len) < 0) {
                    ++skipped;
                }
            }
        }
        cursor += body_len;
    }
    heap_free(scratch);
    if (skipped != 0U) {
        serial_write("vfs: skipped a file that does not fit\n");
    }
    return true;
}

/* Allocate the three tables, halving the request until it fits.
 *
 * A guest with plenty of memory gets the full ceiling.  A small one gets a
 * smaller filesystem rather than a failed mount, because a kernel that cannot
 * mount its own root filesystem is worse off than one with a small root
 * filesystem, and the block size and the on-disk format do not change.
 *
 * The big table goes through the MMU first, because it wants several megabytes
 * and the heap can only serve a physically contiguous run, which on a real
 * guest is often a megabyte or two once the kernel, the framebuffer and Lua
 * have taken their share.  The MMU does not care: it maps one 4 KiB frame per
 * page and hands back something that is virtually contiguous, which is all
 * this ever needed.  Without it the filesystem silently comes up at a quarter
 * of its size, and the boot log still says the ceiling, which is how the
 * original 160 KiB went unnoticed.
 *
 * The largest free block is consulted before the heap fallback rather than
 * after, because a failed attempt that has already taken the big table is the
 * worst order: the storage succeeds, the next two fail, and the space is gone
 * for a smaller attempt that would have fitted. */
static bool vfs_alloc_tables(void)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        int blocks = VFS_TOTAL_BLOCKS >> attempt;
        int nodes = VFS_MAX_NODES >> attempt;
        size_t storage_bytes = (size_t)blocks * VFS_BLOCK_SIZE;
        size_t needed = storage_bytes + (size_t)blocks +
                        (size_t)nodes * sizeof(struct vfs_node);

        if (blocks < 64 || nodes < 32) {
            break;
        }
        if (attempt == 0) {
            vfs_storage = vm_alloc_pages(storage_bytes);
            if (vfs_storage != 0) {
                vfs_storage_mapped = true;
            }
        }
        if (vfs_storage == 0) {
            if (heap_largest_block() < needed) {
                continue;
            }
            vfs_storage = heap_malloc(storage_bytes);
        }
        vfs_block_used = heap_malloc((size_t)blocks);
        vfs_nodes = heap_calloc((size_t)nodes, sizeof(*vfs_nodes));
        if (vfs_storage != 0 && vfs_block_used != 0 && vfs_nodes != 0) {
            vfs_block_count = blocks;
            vfs_node_count = nodes;
            return true;
        }
        /* Should not happen given the check above, but never hold on to a
         * partial set: every later function indexes all three. */
        if (vfs_storage != 0) {
            if (vfs_storage_mapped) {
                vm_free_pages(vfs_storage, storage_bytes);
                vfs_storage_mapped = false;
            } else {
                heap_free(vfs_storage);
            }
            vfs_storage = 0;
        }
        if (vfs_block_used != 0) {
            heap_free(vfs_block_used);
            vfs_block_used = 0;
        }
        if (vfs_nodes != 0) {
            heap_free(vfs_nodes);
            vfs_nodes = 0;
        }
    }
    return false;
}

bool vfs_mount(void)
{
    if (vfs_alloc_tables() == false) {
        return false;
    }
    vfs_format();
    if (vfs_load_image()) {
        return true;
    }
    vfs_seed_directory("/bin");
    vfs_seed_directory("/etc");
    vfs_seed_directory("/home");
    vfs_seed_directory("/home/klye");
    vfs_seed_directory("/var");
    vfs_seed_directory("/var/log");
    vfs_seed("/bin/klye-sh", 0);
    vfs_seed("/bin/klye-edit", 0);
    vfs_seed("/bin/klye-fs", 0);
    vfs_seed("/etc/hostname", "klye-virtual\n");
    vfs_seed("/etc/motd",
             "Welcome to Klye OS.\nType 'help' for a list of commands.\n");
    vfs_seed("/etc/klye.conf",
             "[display]\nresolution = auto\ncompositor = damage\nfps = 60\n\n"
             "[shell]\nprompt = klye\ncolour = neutral\n");
    vfs_seed("/home/klye/readme.txt",
             "Klye OS\n=======\n\nA small freestanding x86_64 kernel with a\n"
             "damage-tracked software compositor.\n\n"
             "Try these in the terminal:\n"
             "  help      list every command\n"
             "  neofetch  system summary\n"
             "  ps        running tasks\n"
             "  uptime    time since boot\n");
    vfs_seed("/home/klye/notes.md",
             "# Notes\n\n- compositor runs at 60 fps\n"
             "- dock magnifies on hover\n- terminal is line buffered\n"
             "- editor supports full line editing\n");
    vfs_seed("/home/klye/todo.txt",
             "[x] boot loader\n[x] memory map\n[x] PS/2 mouse\n"
             "[x] PS/2 keyboard\n[x] compositor\n[x] dock\n"
             "[ ] network stack\n[ ] sound\n");
    vfs_seed("/var/log/klye.log",
             "00.000 kernel entry\n00.004 gdt ready\n00.006 idt ready\n"
             "00.008 pic ready\n00.011 pit 1000 hz\n00.014 ps/2 aux enabled\n"
             "00.021 framebuffer 1280x720\n00.024 compositor online\n");
    return true;
}

bool vfs_mounted(void)
{
    return vfs_ready;
}

uint32_t vfs_bytes_used(void)
{
    return vfs_used_blocks * VFS_BLOCK_SIZE;
}

uint32_t vfs_bytes_total(void)
{
    return (uint32_t)vfs_block_count * VFS_BLOCK_SIZE;
}

uint32_t vfs_bytes_free(void)
{
    return vfs_bytes_total() - vfs_bytes_used();
}

int vfs_node_total(void)
{
    return vfs_used_nodes;
}

int vfs_node_capacity(void)
{
    return vfs_node_count;
}

void vfs_absolute_of(const char *path, const char *cwd, char *out, int max)
{
    char joined[VFS_PATH_MAX * 2];

    vfs_absolute(path, cwd, joined, (int)sizeof(joined));
    vfs_strcpy(out, joined, max);
}

int vfs_resolve(const char *path, const char *cwd)
{
    char absolute[VFS_PATH_MAX * 2];

    if (!vfs_ready || path == 0 || path[0] == '\0') {
        return -1;
    }
    vfs_absolute(path, cwd, absolute, (int)sizeof(absolute));
    return vfs_walk(absolute);
}

int vfs_resolve_parent(const char *path, const char *cwd, char *name,
                       int name_max)
{
    char absolute[VFS_PATH_MAX * 2];
    int length;
    int start;
    int parent;

    if (!vfs_ready || path == 0 || path[0] == '\0') {
        return -1;
    }
    vfs_absolute(path, cwd, absolute, (int)sizeof(absolute));
    length = vfs_strlen(absolute);
    start = length;
    while (start > 1 && absolute[start - 1] != '/') {
        --start;
    }
    if (length - start >= name_max) {
        return -1;
    }
    for (int index = 0; index < length - start; ++index) {
        name[index] = absolute[start + index];
    }
    name[length - start] = '\0';
    if (start <= 1) {
        parent = 0;
    } else {
        char trimmed[VFS_PATH_MAX * 2];

        vfs_strcpy(trimmed, absolute, (int)sizeof(trimmed));
        trimmed[start - 1] = '\0';
        parent = vfs_walk(trimmed);
    }
    if (parent < 0) {
        return -1;
    }
    return parent;
}

bool vfs_exists(const char *path)
{
    return vfs_resolve(path, 0) >= 0;
}

bool vfs_is_directory_path(const char *path)
{
    int index = vfs_resolve(path, 0);

    return index >= 0 && vfs_nodes[index].is_dir;
}

int vfs_touch(const char *path)
{
    int index = vfs_resolve(path, 0);

    if (index > 0 && !vfs_nodes[index].is_dir) {
        vfs_nodes[index].modified = ++vfs_clock;
        return index;
    }
    if (index > 0) {
        return -1;
    }
    return vfs_create(path, false);
}

int vfs_create(const char *path, bool directory)
{
    char name[VFS_NAME_MAX];
    int parent = vfs_resolve_parent(path, 0, name, (int)sizeof(name));
    int index;

    if (parent < 0 || !vfs_nodes[parent].is_dir) {
        return -1;
    }
    if (!vfs_valid_name(name)) {
        return -1;
    }
    if (vfs_child_named(parent, name, vfs_strlen(name)) >= 0) {
        return -1;
    }
    index = vfs_new_node(parent, name, directory);
    if (index >= 0) {
        vfs_nodes[parent].modified = ++vfs_clock;
    }
    return index;
}

int vfs_delete(const char *path)
{
    int index = vfs_resolve(path, 0);

    if (index <= 0) {
        return -1;
    }
    while (vfs_nodes[index].first_child >= 0) {
        vfs_unlink(vfs_nodes[index].first_child);
    }
    {
        int parent = vfs_nodes[index].parent;

        vfs_unlink(index);
        if (parent >= 0) {
            vfs_nodes[parent].modified = ++vfs_clock;
        }
    }
    return 0;
}

static void vfs_rename_subtree(int index, const char *new_name)
{
    struct vfs_node *node = &vfs_nodes[index];
    int parent = node->parent;

    vfs_strcpy(node->name, new_name, VFS_NAME_MAX);
    {
        char joined[VFS_PATH_MAX * 2];

        vfs_strcpy(joined, parent < 0 ? "/" : vfs_nodes[parent].path,
                   (int)sizeof(joined));
        if (vfs_strlen(joined) > 1) {
            int length = vfs_strlen(joined);

            joined[length] = '/';
            joined[length + 1] = '\0';
        }
        vfs_strcpy(joined + vfs_strlen(joined), new_name, (int)sizeof(joined));
        vfs_strcpy(node->path, joined, VFS_PATH_MAX);
    }
    node->modified = ++vfs_clock;
    for (int child = node->first_child; child >= 0;
         child = vfs_nodes[child].next_sibling) {
        vfs_rename_subtree(child, vfs_nodes[child].name);
    }
}

int vfs_rename(const char *path, const char *new_name)
{
    int index = vfs_resolve(path, 0);
    int parent;

    if (index <= 0 || !vfs_valid_name(new_name)) {
        return -1;
    }
    parent = vfs_nodes[index].parent;
    if (vfs_child_named(parent, new_name, vfs_strlen(new_name)) >= 0) {
        return -1;
    }
    vfs_rename_subtree(index, new_name);
    if (parent >= 0) {
        vfs_nodes[parent].modified = ++vfs_clock;
    }
    return 0;
}

int vfs_open(const char *path)
{
    int index = vfs_resolve(path, 0);

    if (index < 0 || vfs_nodes[index].is_dir) {
        return -1;
    }
    return index;
}

int vfs_read(const char *path, char *out, uint32_t max)
{
    int index = vfs_open(path);
    uint32_t length;

    if (index < 0 || out == 0) {
        return -1;
    }
    length = vfs_nodes[index].size;
    if (length > max) {
        length = max;
    }
    for (uint32_t offset = 0; offset < length;) {
        uint32_t chunk = VFS_BLOCK_SIZE - (offset % VFS_BLOCK_SIZE);
        const uint8_t *source;

        if (chunk > length - offset) {
            chunk = length - offset;
        }
        source = vfs_file_pointer(index, offset);
        if (source == 0) {
            break;
        }
        for (uint32_t index_byte = 0; index_byte < chunk; ++index_byte) {
            out[offset + index_byte] = (char)source[index_byte];
        }
        offset += chunk;
    }
    return (int)length;
}

int vfs_write(const char *path, const char *data, uint32_t length)
{
    int index = vfs_resolve(path, 0);
    uint32_t written = 0;

    if (index < 0 || vfs_nodes[index].is_dir) {
        index = vfs_create(path, false);
        if (index < 0) {
            return -1;
        }
    }
    if (length > (uint32_t)VFS_MAX_BLOCKS_PER_FILE * VFS_BLOCK_SIZE) {
        return -1;
    }
    if (length > 0 && !vfs_file_reserve(index, length)) {
        return -1;
    }
    while (written < length) {
        uint32_t chunk = VFS_BLOCK_SIZE - (written % VFS_BLOCK_SIZE);
        uint8_t *target;

        if (chunk > length - written) {
            chunk = length - written;
        }
        target = vfs_file_pointer(index, written);
        if (target == 0) {
            return -1;
        }
        for (uint32_t index_byte = 0; index_byte < chunk; ++index_byte) {
            target[index_byte] = (uint8_t)data[written + index_byte];
        }
        written += chunk;
    }
    vfs_nodes[index].size = length;
    vfs_nodes[index].modified = ++vfs_clock;
    return (int)length;
}

int vfs_append(const char *path, const char *data, uint32_t length)
{
    int index = vfs_open(path);
    char staged[VFS_BODY_MAX];
    uint32_t existing;

    if (index < 0) {
        return vfs_write(path, data, length);
    }
    existing = vfs_nodes[index].size;
    if (existing + length > (uint32_t)sizeof(staged)) {
        return -1;
    }
    if (existing > 0 && vfs_read(path, staged, existing) < 0) {
        return -1;
    }
    for (uint32_t offset = 0; offset < length; ++offset) {
        staged[existing + offset] = data[offset];
    }
    return vfs_write(path, staged, existing + length);
}

int vfs_truncate(const char *path)
{
    int index = vfs_open(path);

    if (index < 0) {
        return -1;
    }
    vfs_nodes[index].size = 0;
    vfs_nodes[index].modified = ++vfs_clock;
    return 0;
}

int vfs_list(const char *directory, int *out_indices, int maximum)
{
    int index = vfs_resolve(directory, 0);
    int found = 0;

    if (index < 0 || !vfs_nodes[index].is_dir || out_indices == 0) {
        return 0;
    }
    for (int child = vfs_nodes[index].first_child; child >= 0 && found < maximum;
         child = vfs_nodes[child].next_sibling) {
        out_indices[found++] = child;
    }
    return found;
}

int vfs_list_path(const char *path, int *out_indices, int maximum)
{
    return vfs_list(path, out_indices, maximum);
}

int vfs_child_count(int index)
{
    int count = 0;

    if (index < 0 || !vfs_nodes[index].used) {
        return 0;
    }
    for (int child = vfs_nodes[index].first_child; child >= 0;
         child = vfs_nodes[child].next_sibling) {
        ++count;
    }
    return count;
}

int vfs_parent(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return -1;
    }
    return vfs_nodes[index].parent;
}

int vfs_node_created(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return 0;
    }
    return (int)vfs_nodes[index].created;
}

uint32_t vfs_node_modified(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return 0U;
    }
    return vfs_nodes[index].modified;
}

int vfs_count(void)
{
    return vfs_used_nodes;
}

const char *vfs_path(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return "/";
    }
    return vfs_nodes[index].path;
}

const char *vfs_name(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return "/";
    }
    return vfs_nodes[index].name;
}

const char *vfs_kind(int index)
{
    static const char *const table[][2] = {
        { "txt", "text" }, { "md", "text" },  { "text", "text" },
        { "conf", "conf" }, { "cfg", "conf" }, { "log", "log" },
        { "bin", "bin" }, { "sh", "bin" }
    };

    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return "file";
    }
    if (vfs_nodes[index].is_dir) {
        return "dir";
    }
    {
        const char *name = vfs_nodes[index].name;
        int length = vfs_strlen(name);
        int dot = -1;

        for (int position = length - 1; position >= 0; --position) {
            if (name[position] == '.') {
                dot = position;
                break;
            }
        }
        if (dot < 0 || dot == length - 1) {
            return "file";
        }
        for (int slot = 0;
             slot < (int)(sizeof(table) / sizeof(table[0])); ++slot) {
            if (vfs_streq(name + dot + 1, table[slot][0])) {
                return table[slot][1];
            }
        }
    }
    return "file";
}

uint32_t vfs_size(int index)
{
    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used) {
        return 0U;
    }
    if (vfs_nodes[index].is_dir) {
        return 0U;
    }
    return vfs_nodes[index].size;
}

const char *vfs_body(int index)
{
    uint32_t size;
    int length;

    if (index < 0 || index >= vfs_node_count || !vfs_nodes[index].used ||
        vfs_nodes[index].is_dir) {
        vfs_scratch[0] = '\0';
        return vfs_scratch;
    }
    size = vfs_nodes[index].size;
    if (size >= VFS_BODY_MAX) {
        size = VFS_BODY_MAX - 1U;
    }
    length = vfs_read(vfs_nodes[index].path, vfs_scratch, size);
    if (length < 0) {
        length = 0;
    }
    vfs_scratch[length] = '\0';
    return vfs_scratch;
}

const char *vfs_find(const char *path)
{
    int index = vfs_resolve(path, 0);

    if (index < 0) {
        return 0;
    }
    return vfs_kind(index);
}

bool vfs_is_directory(const char *path)
{
    return vfs_is_directory_path(path);
}
