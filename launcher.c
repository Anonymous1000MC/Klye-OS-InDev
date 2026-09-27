#include <stdbool.h>
#include <stdint.h>

#include "launcher.h"
#include "vfs.h"

#define LAUNCHER_BODY_MAX 512
#define LAUNCHER_PATH_DIRS 3

static struct launcher launchers[LAUNCHER_MAX];
static int launcher_used;

static const char *const search_dirs[LAUNCHER_PATH_DIRS] = {
    "/bin", "/home/klye/bin", "/usr/bin"
};

static bool text_same(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != 0 && a[index] == b[index]) {
        ++index;
    }
    return a[index] == b[index];
}

static void text_take(char *destination, int max, const char *source)
{
    int index = 0;

    while (source[index] != 0 && index < max - 1) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = 0;
}

static void trim(char *text)
{
    int length = 0;
    int start = 0;
    int end;

    while (text[length] != 0) {
        ++length;
    }
    end = length;
    while (start < end && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' ||
            text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    for (int index = 0; index < end - start; ++index) {
        text[index] = text[start + index];
    }
    text[end - start] = 0;
}

static int parse_int(const char *text)
{
    int value = 0;
    int negative = 0;
    int index = 0;

    if (text[0] == '-') {
        negative = 1;
        index = 1;
    }
    while (text[index] >= '0' && text[index] <= '9') {
        value = value * 10 + (text[index] - '0');
        ++index;
    }
    return negative ? -value : value;
}

static void parse_record(struct launcher *slot, const char *body)
{
    char line[LAUNCHER_BODY_MAX];
    int read = 0;
    int written = 0;

    text_take(slot->title, LAUNCHER_TITLE_MAX, slot->file);
    text_take(slot->kind, LAUNCHER_KIND_MAX, "builtin");
    slot->is_record = false;
    slot->entry[0] = 0;
    slot->icon = -1;

    while (read < LAUNCHER_BODY_MAX - 1 && body[read] != 0) {
        char key[32];
        char value[LAUNCHER_TITLE_MAX];
        int keylen = 0;
        int valuelen = 0;

        if (body[read] == '\n') {
            ++read;
            continue;
        }
        if (body[read] == '#') {
            while (read < LAUNCHER_BODY_MAX - 1 && body[read] != '\n' &&
                   body[read] != 0) {
                ++read;
            }
            continue;
        }
        while (read < LAUNCHER_BODY_MAX - 1 && body[read] != '\n' &&
               body[read] != 0) {
            if (written < LAUNCHER_BODY_MAX - 1) {
                line[written] = body[read];
                ++written;
            }
            ++read;
        }
        line[written] = 0;
        written = 0;
        trim(line);
        if (line[0] == 0) {
            continue;
        }
        for (int index = 0; line[index] != 0 && line[index] != '='; ++index) {
            if (keylen < (int)sizeof(key) - 1) {
                key[keylen] = line[index];
                ++keylen;
            }
        }
        key[keylen] = 0;
        trim(key);
        if (line[keylen] == '=') {
            const char *rest = line + keylen + 1;

            while (*rest != 0 && valuelen < (int)sizeof(value) - 1) {
                value[valuelen] = *rest;
                ++valuelen;
                ++rest;
            }
            value[valuelen] = 0;
        } else {
            value[0] = 0;
        }
        trim(value);

        if (text_same(key, "title")) {
            text_take(slot->title, LAUNCHER_TITLE_MAX, value);
        } else if (text_same(key, "kind")) {
            text_take(slot->kind, LAUNCHER_KIND_MAX, value);
            slot->is_record = true;
        } else if (text_same(key, "name") || text_same(key, "entry")) {
            text_take(slot->entry, LAUNCHER_ENTRY_MAX, value);
        } else if (text_same(key, "icon")) {
            slot->icon = parse_int(value);
        }
    }
}

void launcher_reset(void)
{
    for (int index = 0; index < LAUNCHER_MAX; ++index) {
        launchers[index].used = false;
        launchers[index].icon = -1;
        launchers[index].title[0] = 0;
        launchers[index].kind[0] = 0;
        launchers[index].entry[0] = 0;
        launchers[index].file[0] = 0;
        launchers[index].path[0] = 0;
        launchers[index].is_record = false;
    }
    launcher_used = 0;
}

void launcher_scan(void)
{
    int indices[VFS_LIST_MAX];
    int found = vfs_list_path("/bin", indices, VFS_LIST_MAX);

    launcher_reset();
    for (int index = 0; index < found; ++index) {
        int node = indices[index];
        char body[LAUNCHER_BODY_MAX];
        char path[LAUNCHER_PATH_MAX];
        const char *name = vfs_name(node);
        int length;
        struct launcher *slot;

        if (launcher_used >= LAUNCHER_MAX) {
            return;
        }
        if (name == 0 || vfs_kind(node)[0] == 'd') {
            continue;
        }
        {
            int used = 4;

            path[0] = '/';
            path[1] = 'b';
            path[2] = 'i';
            path[3] = 'n';
            path[used++] = '/';
            for (int add = 0; name[add] != 0 && used < (int)sizeof(path) - 1;
                 ++add) {
                path[used++] = name[add];
            }
            path[used] = 0;
        }
        length = vfs_read(path, body, (uint32_t)sizeof(body) - 1U);
        if (length <= 0) {
            continue;
        }
        body[length] = 0;
        slot = &launchers[launcher_used];
        text_take(slot->file, LAUNCHER_NAME_MAX, name);
        text_take(slot->path, LAUNCHER_PATH_MAX, path);
        parse_record(slot, body);
        if (slot->is_record == false) {
            continue;
        }
        if (slot->entry[0] == 0) {
            text_take(slot->entry, LAUNCHER_ENTRY_MAX, name);
        }
        slot->used = true;
        ++launcher_used;
    }
}

int launcher_count(void)
{
    return launcher_used;
}

const struct launcher *launcher_at(int index)
{
    if (index < 0 || index >= launcher_used) {
        return 0;
    }
    return &launchers[index];
}

int launcher_find(const char *name)
{
    for (int index = 0; index < launcher_used; ++index) {
        if (text_same(launchers[index].file, name)) {
            return index;
        }
    }
    return -1;
}

int launcher_resolve(const char *name)
{
    char candidate[LAUNCHER_PATH_MAX];

    if (name == 0 || name[0] == 0) {
        return -1;
    }
    if (name[0] == '/') {
        for (int index = 0; index < launcher_used; ++index) {
            if (text_same(launchers[index].path, name)) {
                return index;
            }
        }
        return -1;
    }
    for (int dir = 0; dir < LAUNCHER_PATH_DIRS; ++dir) {
        int used = 0;

        while (search_dirs[dir][used] != 0 && used < (int)sizeof(candidate) - 2) {
            candidate[used] = search_dirs[dir][used];
            ++used;
        }
        candidate[used++] = '/';
        for (int add = 0; name[add] != 0 && used < (int)sizeof(candidate) - 1;
             ++add) {
            candidate[used++] = name[add];
        }
        candidate[used] = 0;
        for (int index = 0; index < launcher_used; ++index) {
            if (text_same(launchers[index].path, candidate)) {
                return index;
            }
        }
    }
    return -1;
}

bool launcher_is_builtin(int index, char *entry, int entry_max)
{
    const struct launcher *slot = launcher_at(index);

    if (slot == 0) {
        return false;
    }
    if (text_same(slot->kind, "builtin") == false) {
        return false;
    }
    if (entry != 0 && entry_max > 0) {
        text_take(entry, entry_max, slot->entry);
    }
    return true;
}

bool launcher_is_bytecode(int index)
{
    const struct launcher *slot = launcher_at(index);

    if (slot == 0) {
        return false;
    }
    return text_same(slot->kind, "kby");
}
