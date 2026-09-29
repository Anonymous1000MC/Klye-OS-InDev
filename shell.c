#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "ata.h"
#include "blob.h"
#include "doom.h"
#include "doom_level.h"
#include "fat.h"
#include "wad.h"
#include "vfs.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "heap.h"
extern long strtol(const char *text, char **end, int base);

#include "kby.h"
#include "lua_host.h"
#include "kas.h"
#include "launcher.h"
#include "kernel.h"
#include "scheduler.h"
#include "pci.h"
#include "user.h"
#include "elf.h"
#include "virtio.h"
#include "shell.h"
#include "theme.h"
#include "wm.h"

static int shell_u32_to_text(char *buffer, uint32_t value);
static void shell_printf_u64(uint64_t value);
static int shell_pad_int(char *out, uint32_t value, int width);
static int shell_pad_hex(char *out, uint32_t value, int width);
static int shell_pad_text(char *out, const char *text, int width);
static void shell_printf_hex_line(uint32_t value);
static void shell_pad(char *buffer, int width);
static void shell_row(const char *name, const char *value, int width);
static void shell_format_uptime(char *buffer, int limit);

static struct shell_state state;
static uint32_t command_counter;
static char last_command[SHELL_CWD_MAX];
static bool reboot_requested;
static bool halt_requested;

static bool text_equal(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != '\0' && a[index] == b[index]) {
        ++index;
    }
    return a[index] == b[index];
}

static void text_copy(char *destination, const char *source, int limit)
{
    int index = 0;

    while (source[index] != '\0' && index < limit - 1) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static int text_length(const char *text)
{
    int length = 0;

    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static void shell_set_cwd(const char *path)
{
    int length = 0;
    bool trailing = false;

    if (path[0] == '/') {
        text_copy(state.cwd, "/", SHELL_CWD_MAX);
        length = 1;
    }
    while (path[length] != '\0' && length < SHELL_CWD_MAX - 1) {
        if (path[length] == '/' && path[length + 1] == '/') {
            continue;
        }
        state.cwd[length] = path[length];
        length++;
        trailing = path[length - 1] == '/';
    }
    state.cwd[length] = '\0';
    if (length > 1 && trailing) {
        state.cwd[length - 1] = '\0';
    }
    if (state.cwd[0] == '\0') {
        text_copy(state.cwd, "/", SHELL_CWD_MAX);
    }
}

static char shell_operator_write[2] = ">";
static char shell_operator_append[3] = ">>";

static int shell_tokenize(char *line, char **tokens, int maximum)
{
    char *cursor = line;
    int count = 0;

    while (count < maximum) {
        char *start;
        char *write;
        char delim;

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '>') {
            if (cursor[1] == '>') {
                tokens[count++] = shell_operator_append;
                cursor += 2;
            } else {
                tokens[count++] = shell_operator_write;
                cursor += 1;
            }
            continue;
        }
        start = write = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '>') {
            if (*cursor == '"') {
                cursor++;
                while (*cursor != '\0' && *cursor != '"') {
                    *write++ = *cursor++;
                }
                if (*cursor == '"') {
                    cursor++;
                }
                continue;
            }
            *write++ = *cursor++;
        }
        delim = *cursor;
        *write = '\0';
        if (write == start && delim == '\0') {
            break;
        }
        tokens[count++] = start;
        if (delim == '\0') {
            break;
        }
        if (delim == '>') {
            continue;
        }
        *cursor = '\0';
        cursor++;
    }
    return count;
}

static uint64_t shell_uptime_ms(void)
{
    return pit_ticks();
}

static void shell_format_uptime(char *buffer, int limit)
{
    uint64_t total = shell_uptime_ms() / 1000U;
    uint32_t hours = (uint32_t)(total / 3600U);
    uint32_t minutes = (uint32_t)((total / 60U) % 60U);
    uint32_t seconds = (uint32_t)(total % 60U);
    int offset = 0;

    if (hours > 0U) {
        offset += shell_u32_to_text(buffer + offset, hours);
        buffer[offset++] = 'h';
    }
    if (offset == 0 || hours > 0U) {
        offset += shell_u32_to_text(buffer + offset, minutes);
        buffer[offset++] = 'm';
    }
    offset += shell_u32_to_text(buffer + offset, seconds);
    buffer[offset++] = 's';
    if (offset >= limit) {
        offset = limit - 1;
    }
    buffer[offset] = '\0';
}

static int shell_u32_to_text(char *buffer, uint32_t value)
{
    char digits[12];
    int length = 0;
    int offset = 0;

    if (value == 0U) {
        buffer[0] = '0';
        return 1;
    }
    while (value != 0U && length < (int)sizeof(digits)) {
        digits[length++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    }
    while (length > 0) {
        buffer[offset++] = digits[--length];
    }
    return offset;
}

static uint32_t shell_atoi_value(const char *text)
{
    uint32_t value = 0U;

    for (int index = 0; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            break;
        }
        value = value * 10U + (uint32_t)(text[index] - '0');
    }
    return value;
}

static const char *shell_u32_to_text_scratch(uint32_t value)
{
    static char scratch[12];

    (void)shell_u32_to_text(scratch, value);
    scratch[11] = '\0';
    return scratch;
}

static void shell_printf_u64(uint64_t value)
{
    char buffer[24];
    int offset = 0;

    if (value == 0U) {
        terminal_puts("0");
        return;
    }
    while (value != 0U && offset < 20) {
        buffer[offset++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    }
    buffer[offset] = '\0';
    for (int left = 0, right = offset - 1; left < right; ++left, --right) {
        char swap = buffer[left];

        buffer[left] = buffer[right];
        buffer[right] = swap;
    }
    terminal_puts(buffer);
}

static void shell_pad(char *buffer, int width)
{
    int length = text_length(buffer);

    while (length < width) {
        buffer[length++] = ' ';
        buffer[length] = '\0';
    }
}

static void shell_row(const char *name, const char *value, int width)
{
    char label[32];

    text_copy(label, name, (int)sizeof(label));
    shell_pad(label, width);
    terminal_puts(label);
    terminal_puts(value);
    terminal_puts("\n");
}

static void cmd_help(void)
{
    char size_text[16];

    terminal_puts("klye-sh built-in commands\n");
    terminal_puts("---------------------------\n");
    shell_row("help", "show this list", 14);
    shell_row("clear", "clear the screen", 14);
    shell_row("echo TEXT", "print TEXT", 14);
    shell_row("uname [-a]", "kernel identity", 14);
    shell_row("ls [-l] [DIR]", "list directory", 14);
    shell_row("cat FILE", "print a file", 14);
    shell_row("echo TEXT > FILE", "write TEXT to FILE", 14);
    shell_row("touch FILE", "create an empty file", 14);
    shell_row("mkdir DIR", "create a directory", 14);
    shell_row("rm [-r] FILE", "remove a file or directory", 14);
    shell_row("ps", "running tasks", 14);
    shell_row("uptime", "time since boot", 14);
    shell_row("neofetch", "system summary", 14);
    shell_row("reboot", "restart the machine", 14);
    shell_row("bench [N]", "measure compositor speed", 14);
    shell_row("kbyrun PROG", "run a KBY bytecode program", 14);
    shell_row("which NAME", "resolve from /bin", 14);
    shell_row("launchers", "list /bin launchers", 14);
    shell_row("dumpmem [KB] [MB]", "fill memory, then panic", 14);
    terminal_puts("\n");
    shell_row("pwd", "print directory", 14);
    shell_row("cd DIR", "change directory", 14);
    shell_row("date", "current date and time", 14);
    shell_row("whoami", "current user", 14);
    shell_row("hostname", "machine name", 14);
    shell_row("man CMD", "detail for a command", 14);
    shell_row("history", "command history", 14);
    shell_row("df", "filesystem usage", 14);
    shell_row("free", "memory usage", 14);
    shell_row("sysinfo", "hardware summary", 14);
    shell_row("apps", "installed applications", 14);
    shell_row("open APP", "launch an application", 14);
    shell_row("banner", "draw the logo", 14);
    shell_row("halt", "stop the cpu", 14);
    terminal_puts("\nfiles:\n");
    {
        int indices[VFS_LIST_MAX];
        int found = vfs_list_path("/", indices, VFS_LIST_MAX);

        for (int index = 0; index < found; ++index) {
            int node = indices[index];
            int first_file = 1;

            for (int probe = node; probe > 0; probe = vfs_parent(probe)) {
                first_file = 0;
                break;
            }
            if (!first_file || vfs_kind(node)[0] == 'd') {
                continue;
            }
            terminal_puts("  ");
            terminal_puts(vfs_path(node));
            if (vfs_size(node) > 0U) {
                terminal_puts("  ");
                shell_u32_to_text(size_text, vfs_size(node));
                terminal_puts(" bytes");
            }
            terminal_puts("\n");
        }
    }
}

static void cmd_uname(const char *argument)
{
    if (text_equal(argument, "-a") || text_equal(argument, "--all")) {
        terminal_puts("Klye OS klye-virtual 0.2.0-freestanding #1 SMP x86_64 "
                      "GNU/Linux\n");
        return;
    }
    if (text_equal(argument, "-r") || text_equal(argument, "--release")) {
        terminal_puts("0.2.0-freestanding\n");
        return;
    }
    if (text_equal(argument, "-m") || text_equal(argument, "--machine")) {
        terminal_puts("x86_64\n");
        return;
    }
    if (text_equal(argument, "-n") || text_equal(argument, "--nodename")) {
        terminal_puts(state.host);
        terminal_puts("\n");
        return;
    }
    terminal_puts("Klye OS\n");
}

static void cmd_ls(const char *directory, bool long_format)
{
    int indices[VFS_LIST_MAX];
    char size_text[16];
    int found;
    bool any = false;

    found = vfs_list(directory, indices, VFS_LIST_MAX);
    for (int pass = 0; pass < 2; ++pass) {
        for (int index = 0; index < found; ++index) {
            int node = indices[index];
            bool is_dir = vfs_kind(node)[0] == 'd';

            if ((pass == 0 && is_dir) || (pass == 1 && !is_dir)) {
                continue;
            }
            if (long_format) {
                terminal_puts(is_dir ? "  drwxr-xr-x  " : "  -rw-r--r--  ");
                if (is_dir) {
                    text_copy(size_text, "dir", (int)sizeof(size_text));
                    shell_pad(size_text, 8);
                } else {
                    shell_u32_to_text(size_text, vfs_size(node));
                    shell_pad(size_text, 8);
                }
                terminal_puts("  ");
            } else {
                terminal_puts("  ");
            }
            terminal_puts(vfs_name(node));
            if (is_dir) {
                terminal_puts("/");
            }
            terminal_puts("\n");
            any = true;
        }
    }
    if (!any) {
        terminal_puts("  (empty)\n");
    }
}

static void cmd_cat(const char *path)
{
    char resolved[VFS_PATH_MAX];
    char body[VFS_BODY_MAX];
    int length;

    vfs_absolute_of(path, state.cwd, resolved, (int)sizeof(resolved));
    if (vfs_is_directory_path(resolved)) {
        terminal_puts("cat: ");
        terminal_puts(resolved);
        terminal_puts(": Is a directory\n");
        terminal_error();
        return;
    }
    length = vfs_read(resolved, body, (uint32_t)sizeof(body) - 1U);
    if (length < 0) {
        terminal_puts("cat: ");
        terminal_puts(resolved);
        terminal_puts(": No such file or directory\n");
        terminal_error();
        return;
    }
    if (length == 0) {
        terminal_puts("(empty)\n");
        return;
    }
    body[length] = '\0';
    terminal_puts(body);
    if (body[length - 1] != '\n') {
        terminal_puts("\n");
    }
}

static void cmd_ps(void)
{
    int current = scheduler_current_task();

    terminal_puts("  TID  TASK             STATE      HEARTBEAT\n");
    terminal_printf_number((uint64_t)(current < 0 ? 0 : current));
    terminal_puts("  idle               running    -\n");
    terminal_printf_number((uint64_t)current);
    terminal_puts("  compositor         running    -\n");
    for (int index = 0; index < APP_COUNT; ++index) {
        char heartbeat[16];
        char state_text[12];

        if (!app_is_open((enum app_id)index)) {
            continue;
        }
        text_copy(state_text, "window", (int)sizeof(state_text));
        shell_u32_to_text(heartbeat, (uint32_t)wm_app_heartbeat((enum app_id)index));
        terminal_printf_number((uint64_t)(index + 2));
        terminal_puts("  ");
        terminal_puts(app_name((enum app_id)index));
        for (int pad = text_length(app_name((enum app_id)index)); pad < 16;
             ++pad) {
            terminal_puts(" ");
        }
        terminal_puts(state_text);
        for (int pad = text_length(state_text); pad < 11; ++pad) {
            terminal_puts(" ");
        }
        terminal_puts(heartbeat);
        terminal_puts(" ms\n");
    }
}

static void cmd_uptime(void)
{
    char duration[32];
    uint64_t seconds = shell_uptime_ms() / 1000U;

    shell_format_uptime(duration, (int)sizeof(duration));
    terminal_puts("up ");
    terminal_puts(duration);
    terminal_puts(", ");
    terminal_printf_number(seconds / 60U);
    terminal_puts(" min, ");
    terminal_printf_number(seconds % 60U);
    terminal_puts(" sec, ");
    terminal_printf_number((uint64_t)app_open_count() + 2U);
    terminal_puts(" tasks\n");
}

static void cmd_pwd(void)
{
    terminal_puts(state.cwd);
    terminal_puts("\n");
}

static uint8_t cmos_read(uint8_t reg)
{
    uint8_t value;

    __asm__ volatile("cli");
    outb(0x70, reg);
    value = inb(0x71);
    __asm__ volatile("sti");
    return value;
}

static bool cmos_update_in_progress(void)
{
    return (cmos_read(0x0A) & 0x80U) != 0U;
}

static uint8_t cmos_read_bcd(uint8_t reg)
{
    uint8_t value = cmos_read(reg);

    return (uint8_t)((value & 0x0FU) + ((value >> 4) * 10U));
}

static void cmd_date(void)
{
    static const char *const weekdays[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
    int century_index;

    for (int attempt = 0; attempt < 100000; ++attempt) {
        if (!cmos_update_in_progress()) {
            break;
        }
    }
    if (cmos_update_in_progress()) {
        terminal_puts("date: rtc is not responding\n");
        terminal_error();
        return;
    }
    second = cmos_read(0x08);
    minute = cmos_read(0x09);
    hour = cmos_read_bcd(0x0B);
    day = cmos_read_bcd(0x07);
    month = cmos_read_bcd(0x06);
    year = cmos_read_bcd(0x32);
    century = cmos_read_bcd(0x32);
    status_b = cmos_read(0x0B);
    century_index = (int)century - 19;
    if (status_b & 0x04U) {
        hour = (uint8_t)(hour % 12U);
    }
    if (status_b & 0x80U) {
        hour = (uint8_t)(hour + 12U);
    }
    second = (uint8_t)(second % 60U);
    if (month < 1U || month > 12U) {
        month = 1U;
    }
    if (day < 1U || day > 31U) {
        day = 1U;
    }
    if (century_index < 0 || century_index > 3) {
        century_index = 2;
    }
    terminal_puts(weekdays[cmos_read(0x04) % 7U]);
    terminal_puts(" ");
    terminal_puts(months[month - 1U]);
    terminal_puts(" ");
    terminal_printf_number((uint64_t)day);
    terminal_puts(" ");
    terminal_printf_number((uint64_t)year);
    terminal_puts(" ");
    terminal_printf_number((uint64_t)hour);
    terminal_puts(":");
    terminal_printf_number((uint64_t)minute);
    terminal_puts(":");
    terminal_printf_number((uint64_t)second);
    terminal_puts(" UTC ");
    terminal_printf_number((uint64_t)(century_index * 100 + year));
    terminal_puts("\n");
}

static void cmd_cd(const char *argument)
{
    char resolved[SHELL_CWD_MAX + 48];

    if (argument[0] == '\0') {
        shell_set_cwd("/");
        return;
    }
    if (text_equal(argument, "..")) {
        char parent[SHELL_CWD_MAX];
        int length = text_length(state.cwd);
        int cut = length - 1;

        if (length <= 1) {
            text_copy(state.cwd, "/", SHELL_CWD_MAX);
            return;
        }
        while (cut > 0 && state.cwd[cut] != '/') {
            cut--;
        }
        if (cut <= 0) {
            text_copy(state.cwd, "/", SHELL_CWD_MAX);
            return;
        }
        for (int index = 0; index < cut; ++index) {
            parent[index] = state.cwd[index];
        }
        parent[cut] = '\0';
        text_copy(state.cwd, parent, SHELL_CWD_MAX);
        return;
    }
    if (argument[0] == '/') {
        text_copy(resolved, argument, (int)sizeof(resolved));
    } else {
        int length = text_length(state.cwd);
        int offset;

        text_copy(resolved, state.cwd, (int)sizeof(resolved));
        offset = length;
        if (offset == 0 || resolved[offset - 1] != '/') {
            resolved[offset++] = '/';
            resolved[offset] = '\0';
        }
        text_copy(resolved + offset, argument,
                  (int)sizeof(resolved) - offset);
    }
    if (!vfs_is_directory(resolved)) {
        terminal_puts("cd: ");
        terminal_puts(argument);
        terminal_puts(": No such directory\n");
        terminal_error();
        return;
    }
    shell_set_cwd(resolved);
}

static void cmd_df(void)
{
    uint64_t total = vfs_bytes_total();
    uint64_t used = vfs_bytes_used();
    uint64_t avail = vfs_bytes_free();

    terminal_puts("filesystem      size   used  avail  use%  mounted on\n");
    terminal_puts("klye-vfs   ");
    terminal_printf_number(total / 1024U);
    terminal_puts("K  ");
    terminal_printf_number(used / 1024U);
    terminal_puts("K  ");
    terminal_printf_number(avail / 1024U);
    terminal_puts("K   ");
    terminal_printf_number(used * 100U / (total == 0U ? 1U : total));
    terminal_puts("%   /\n");
}


/* Exercises the allocator: many sizes, free everything, then reallocate in
 * random-ish order and confirm the data survived.  Lua's collector depends on
 * free() actually returning memory. */
/* "ata" reports what the drive says, and "ataread <lba>" pulls one sector
 * back so the transfer path can be checked against a known pattern. */
/* "files" lists the disk image, and "blobread <name>" pulls a file off the
 * disk and back onto the built-in filesystem, so a large file such as a WAD
 * can be checked against the host copy. */
/* "wadcheck" pulls a named file off the disk and hashes it, so a large
 * transfer can be confirmed end to end without a debugger attached. */
static void cmd_wadcheck(char **tokens, int count)
{
    const char *path = "doom/probe.wad";
    void *buffer = 0;
    uint32_t length = 0;
    uint32_t hash = 2166136261U;

    if (count > 1) {
        path = tokens[1];
    }
    if (!blob_load(path, &buffer, &length)) {
        terminal_puts("wadcheck: ");
        terminal_puts(blob_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    for (uint32_t index = 0; index < length; ++index) {
        hash ^= (uint32_t)((const uint8_t *)buffer)[index];
        hash *= 16777619U;
    }
    terminal_puts("wadcheck: ");
    terminal_puts(path);
    terminal_puts(" loaded ");
    terminal_printf_number((uint64_t)length);
    terminal_puts(" bytes, fnv1a ");
    terminal_printf_number((uint64_t)hash);
    terminal_puts("\n");
    /* show the markers that were planted at known offsets */
    if (length > 6U * 1024U * 1024U) {
        terminal_puts("  at 6MiB: ");
        terminal_write((const char *)buffer + 6U * 1024U * 1024U, 17);
        terminal_puts("\n  at end:  ");
        terminal_write((const char *)buffer + length - 19U, 19);
        terminal_puts("\n");
    }
    blob_release(buffer);
}

/* "wadinfo <file>" parses a WAD straight out of the disk image and reports
 * what it found, so the directory walk can be checked against a real WAD
 * without a debugger attached. */
/* "levelinfo <E1M1>" parses a level's structures and reports them, so the
 * decode can be checked against the WAD without a debugger attached. */
static void cmd_levelinfo(char **tokens, int count)
{
    const char *marker = "E1M1";
    const struct doom_level *level;
    const struct wad_lump *lump;

    if (count > 1) {
        marker = tokens[1];
    }
    if (!doom_wad_open()) {
        terminal_puts("levelinfo: no wad open\n");
        terminal_error();
        return;
    }
    if (!doom_level_load(marker)) {
        terminal_puts("levelinfo: ");
        terminal_puts(doom_level_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    level = doom_level();
    terminal_puts("levelinfo: ");
    terminal_puts(level->name);
    terminal_puts("\n  vertexes    ");
    terminal_printf_number((uint64_t)level->vertex_count);
    terminal_puts("\n  linedefs    ");
    terminal_printf_number((uint64_t)level->linedef_count);
    terminal_puts("\n  sidedefs    ");
    terminal_printf_number((uint64_t)level->sidedef_count);
    terminal_puts("\n  sectors     ");
    terminal_printf_number((uint64_t)level->sector_count);
    terminal_puts("\n  segs        ");
    terminal_printf_number((uint64_t)level->seg_count);
    terminal_puts("\n  subsectors  ");
    terminal_printf_number((uint64_t)level->subsector_count);
    terminal_puts("\n  nodes       ");
    terminal_printf_number((uint64_t)level->node_count);
    terminal_puts("\n  reject      ");
    terminal_printf_number((uint64_t)level->reject_size);
    terminal_puts(" bytes\n  blockmap    ");
    terminal_printf_number((uint64_t)level->blockmap_words);
    terminal_puts(" words\n");

    /* a couple of decoded values, so a wrong stride would show up */
    if (level->linedef_count > 0) {
        terminal_puts("  linedef[0]  vertices ");
        terminal_printf_number((uint64_t)level->linedefs[0].start);
        terminal_puts(" -> ");
        terminal_printf_number((uint64_t)level->linedefs[0].end);
        terminal_puts(", right side ");
        if (level->linedefs[0].right < 0) {
            terminal_puts("none");
        } else {
            terminal_printf_number((uint64_t)level->linedefs[0].right);
        }
        terminal_puts(", left side ");
        if (level->linedefs[0].left < 0) {
            terminal_puts("none");
        } else {
            terminal_printf_number((uint64_t)level->linedefs[0].left);
        }
        terminal_puts("\n");
    }
    if (level->sector_count > 0) {
        const struct doom_sector *s = &level->sectors[0];

        terminal_puts("  sector[0]   floor ");
        terminal_printf_number((uint64_t)(int64_t)s->floor_height);
        terminal_puts(", ceiling ");
        terminal_printf_number((uint64_t)(int64_t)s->ceiling_height);
        terminal_puts("\n  textures    floor '");
        terminal_puts(doom_level_floor_texture(s));
        terminal_puts("' ceiling '");
        terminal_puts(doom_level_ceiling_texture(s));
        terminal_puts("'  layout ");
        terminal_puts(wad_is_version_199() ? "1.9 indices" : "pre-1.9 names");
        terminal_puts("\n  pnames      ");
        {
            /* signed: -1 is a real answer here, and casting it to uint64
             * printed 18446744073709551615 */
            int index = doom_level_patch_index(doom_level_floor_texture(s));

            if (index < 0) {
                terminal_puts("-1, so the floor is a flat rather than a texture");
            } else {
                terminal_printf_number((uint64_t)index);
                terminal_puts(" for the floor texture");
            }
        }
        terminal_puts("\n");
    }
    if (level->subsector_count > 0) {
        terminal_puts("  subsector[0] ");
        terminal_printf_number((uint64_t)level->subsectors[0].count);
        terminal_puts(" segs from ");
        terminal_printf_number((uint64_t)level->subsectors[0].first);
        terminal_puts("\n");
    }
    /* and a cross check: every seg must belong to some subsector's range */
    if (level->segs != 0 && level->subsectors != 0) {
        uint32_t covered = 0;
        bool ok = true;

        for (uint16_t at = 0; at < level->subsector_count; ++at) {
            uint32_t first = level->subsectors[at].first;

            if (first + level->subsectors[at].count > level->seg_count) {
                ok = false;
            }
            covered += level->subsectors[at].count;
        }
        terminal_puts("  subsectors cover ");
        terminal_printf_number((uint64_t)covered);
        terminal_puts(" of ");
        terminal_printf_number((uint64_t)level->seg_count);
        terminal_puts(ok ? " segs, all in range\n" : " segs, OUT OF RANGE\n");
    }
    (void)lump;
}

static void cmd_wadinfo(char **tokens, int count)
{
    const char *path = "doom/DOOM1.WAD";
    void *buffer = 0;
    uint32_t length = 0;
    int levels = 0;
    bool from_fat = false;

    if (count > 1) {
        path = tokens[1];
    }
    if (!blob_load_any(path, &buffer, &length, &from_fat)) {
        terminal_puts("wadinfo: ");
        terminal_puts(blob_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    if (!wad_open(buffer, length)) {
        terminal_puts("wadinfo: ");
        terminal_puts(wad_error());
        terminal_puts("\n");
        blob_release_any(buffer, from_fat);
        terminal_error();
        return;
    }
    terminal_puts("wadinfo: ");
    terminal_puts(wad_kind());
    terminal_puts(", ");
    terminal_printf_number((uint64_t)wad_count());
    terminal_puts(" lumps, ");
    terminal_printf_number((uint64_t)length);
    terminal_puts(" bytes\n");

    /* a few lumps the game cannot start without */
    static const char *wanted[] = {"PLAYPAL", "COLORMAP", "PNAMES",
                                    "TEXTURE1", "TEXTURE2", "TITLEPIC", 0};
    for (int index = 0; wanted[index] != 0; ++index) {
        const struct wad_lump *lump = wad_find(wanted[index]);

        terminal_puts("  ");
        terminal_puts(wanted[index]);
        if (lump == 0) {
            terminal_puts(": absent\n");
            continue;
        }
        terminal_puts(": ");
        terminal_printf_number((uint64_t)lump->size);
        terminal_puts(" bytes");
        if (!lump->loaded) {
            terminal_puts(" (marker)");
        }
        terminal_puts("\n");
    }

    for (int index = 0; index < wad_count(); ++index) {
        const struct wad_lump *lump = wad_at(index);
        const struct wad_lump *thngs = 0;
        const struct wad_lump *linedef = 0;
        const struct wad_lump *sidedef = 0;

        if (!wad_is_level_marker(lump)) {
            continue;
        }
        ++levels;
        if (levels > 12) {
            continue;
        }
        terminal_puts("  level ");
        terminal_puts(lump->name);
        if (wad_level_parts(lump, &thngs, &linedef, &sidedef)) {
            terminal_puts(": things ");
            terminal_printf_number((uint64_t)thngs->size);
            terminal_puts(" linedefs ");
            terminal_printf_number((uint64_t)linedef->size);
            terminal_puts(" sidedefs ");
            terminal_printf_number((uint64_t)sidedef->size);
        } else {
            terminal_puts(": no data lumps");
        }
        terminal_puts("\n");
    }
    terminal_puts("  levels found: ");
    terminal_printf_number((uint64_t)levels);
    terminal_puts("\n");
    blob_release_any(buffer, from_fat);
}

/* "fatl [dir]" lists a directory on the FAT volume.  This is the real
 * filesystem, so it has directories rather than a flat manifest of names. */
static void cmd_fatl(char **tokens, int count)
{
    const char *path = "/";
    struct fat_stat entries[64];
    int found;

    if (count > 1) {
        path = tokens[1];
    }
    if (!fat_ready()) {
        terminal_puts("fatl: no FAT volume; ");
        terminal_puts(fat_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    found = fat_list(path, entries, 64);
    if (found < 0) {
        terminal_puts("fatl: ");
        terminal_puts(fat_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    terminal_puts("fatl ");
    terminal_puts(path);
    terminal_puts(": ");
    terminal_printf_number((uint64_t)found);
    terminal_puts(" entries\n");
    for (int index = 0; index < found; ++index) {
        terminal_puts("  ");
        terminal_puts(entries[index].directory ? "d " : "- ");
        terminal_puts(entries[index].name);
        if (entries[index].directory == 0) {
            terminal_puts("  ");
            terminal_printf_number((uint64_t)entries[index].size);
            terminal_puts(" bytes");
        }
        terminal_puts("\n");
    }
}

static void cmd_files(void)
{
    if (!blob_ready()) {
        terminal_puts("files: no image mounted; ");
        terminal_puts(blob_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    terminal_puts("files: ");
    terminal_printf_number((uint64_t)blob_count());
    terminal_puts(" entries\n");
    for (uint32_t index = 0; index < blob_count(); ++index) {
        terminal_puts("  ");
        terminal_puts(blob_name(index));
        terminal_puts("  ");
        terminal_printf_number((uint64_t)blob_size(blob_name(index)));
        terminal_puts(" bytes\n");
    }
}

static void cmd_ata(void)
{
    if (!ata_present()) {
        terminal_puts("ata: no drive; ");
        terminal_puts(ata_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    terminal_puts("ata: ");
    terminal_puts(ata_model());
    terminal_puts("\nata: ");
    terminal_printf_number((uint64_t)ata_sector_count());
    terminal_puts(" sectors, ");
    terminal_printf_number((uint64_t)ata_sector_count() * 512ULL);
    terminal_puts(" bytes\n");
}

static void cmd_ataread(char **tokens, int count)
{
    uint32_t lba;
    uint8_t sector[ATA_SECTOR_BYTES];
    uint32_t shown;

    if (count < 2) {
        terminal_puts("ataread: usage: ataread <lba>\n");
        terminal_error();
        return;
    }
    lba = (uint32_t)shell_atoi_value(tokens[1]);
    if (!ata_read(lba, 1U, sector)) {
        terminal_puts("ataread: ");
        terminal_puts(ata_error());
        terminal_puts("\n");
        terminal_error();
        return;
    }
    terminal_puts("ataread ");
    terminal_printf_number((uint64_t)lba);
    terminal_puts(": ");
    for (shown = 0; shown < 32U; ++shown) {
        char pair[3];

        pair[0] = "0123456789abcdef"[sector[shown] >> 4];
        pair[1] = "0123456789abcdef"[sector[shown] & 0x0FU];
        pair[2] = 0;
        terminal_puts(pair);
    }
    terminal_puts("\n");
}

static void cmd_heapcheck(void)
{
    static void *slots[64];
    uint32_t seed = 12345U;
    int failures = 0;

    for (int round = 0; round < 3; ++round) {
        for (int index = 0; index < 64; ++index) {
            seed = seed * 1103515245U + 12345U;
            slots[index] = heap_malloc(16U + (seed >> 16) % 4096U);
            if (slots[index] == 0) {
                terminal_puts("heapcheck: allocation failed at ");
                shell_printf_u64((uint64_t)index);
                terminal_puts("\n");
                ++failures;
                slots[index] = 0;
                continue;
            }
            __builtin_memset(slots[index], (int)(index + 1), 16U);
        }
        for (int index = 0; index < 64; ++index) {
            if (slots[index] == 0) {
                continue;
            }
            if (((uint8_t *)(uintptr_t)slots[index])[0] != (uint8_t)(index + 1) ||
                ((uint8_t *)(uintptr_t)slots[index])[15] != (uint8_t)(index + 1)) {
                ++failures;
            }
            heap_free(slots[index]);
        }
    }
    /* realloc must preserve the old contents */
    for (int index = 0; index < 16; ++index) {
        uint8_t *block = (uint8_t *)(uintptr_t)heap_malloc(32);

        if (block == 0) {
            ++failures;
            continue;
        }
        for (int at = 0; at < 32; ++at) {
            block[at] = (uint8_t)(at + 1);
        }
        block = (uint8_t *)(uintptr_t)heap_realloc(block, 4096);
        if (block == 0) {
            ++failures;
            continue;
        }
        for (int at = 0; at < 32; ++at) {
            if (block[at] != (uint8_t)(at + 1)) {
                ++failures;
                break;
            }
        }
        heap_free(block);
    }
    /* calloc must zero */
    {
        uint8_t *zeroed = (uint8_t *)(uintptr_t)heap_calloc(256, 4);

        if (zeroed == 0) {
            ++failures;
        } else {
            for (int at = 0; at < 1024; ++at) {
                if (zeroed[at] != 0U) {
                    ++failures;
                    break;
                }
            }
            heap_free(zeroed);
        }
    }
    /* raw pages, then hand them back */
    {
        void *raw[8];
        int taken = 0;

        for (int index = 0; index < 8; ++index) {
            raw[index] = heap_alloc_pages(4096U * 4U);
            if (raw[index] != 0) {
                ++taken;
                __builtin_memset(raw[index], 0xAB, 4096U * 4U);
            }
        }
        for (int index = 0; index < 8; ++index) {
            if (raw[index] != 0) {
                heap_free_pages(raw[index], 4096U * 4U);
            }
        }
        terminal_puts("heapcheck: 4 page runs, ");
        shell_printf_u64((uint64_t)taken);
        terminal_puts(" granted\n");
    }
    terminal_puts("heapcheck: ");
    if (failures == 0) {
        terminal_puts("PASS\n");
    } else {
        terminal_puts("FAIL, ");
        shell_printf_u64((uint64_t)failures);
        terminal_puts(" problems\n");
    }
}

static void cmd_free(void)
{
    terminal_puts("              total        used        free\n");
    terminal_puts("mem:        512M        ");
    terminal_printf_number((uint64_t)gfx_alloc_bytes_used() / (1024U * 1024U) + 18U);
    terminal_puts("M       ");
    terminal_printf_number((uint64_t)gfx_alloc_bytes_used() / (1024U * 1024U));
    terminal_puts("M\n");
    terminal_puts("heap:     ");
    shell_printf_u64((uint64_t)heap_total_bytes() / (1024U * 1024U));
    terminal_puts("M pool, ");
    shell_printf_u64((uint64_t)heap_free_bytes() / (1024U * 1024U));
    terminal_puts("M free, ");
    shell_printf_u64((uint64_t)heap_live_bytes() / 1024U);
    terminal_puts("K live\n");
    terminal_puts("pages:    ");
    shell_printf_u64((uint64_t)heap_page_count());
    terminal_puts(" total, ");
    shell_printf_u64((uint64_t)heap_free_page_count());
    terminal_puts(" free, largest block ");
    shell_printf_u64((uint64_t)heap_largest_block() / 1024U);
    terminal_puts("K\n");
    terminal_puts("frames:   ");
    shell_printf_u64((uint64_t)gfx_present_count());
    terminal_puts(" presented, ");
    shell_printf_u64((uint64_t)wm_frame_count());
    terminal_puts(" composed at ");
    shell_printf_u64((uint64_t)wm_fps());
    terminal_puts(" fps\n");
}

/* "pci" walks the bus and prints what is on it.
 *
 * This is the enumeration layer on its own: identity and class, with the base
 * address registers shown as the device set them and not sized or mapped.  A
 * BAR needs to be sized by writing all ones to it and reading back the mask,
 * which makes the device briefly claim an address range it does not have, and
 * that does not belong in a command that only means to look. */
static void cmd_pci(char **tokens, int count)
{
    int found;
    bool map = false;

    (void)count;
    found = pci_enumerate();
    if (count > 1 && text_equal(tokens[1], "map")) {
        int mapped = pci_map_all();

        map = true;

        terminal_puts("  sized and mapped ");
        shell_printf_u64((uint64_t)mapped);
        terminal_puts(" of ");
        shell_printf_u64((uint64_t)found);
        terminal_puts(" device(s)\n");
    }
    terminal_puts("  bus dev fn  vendor  device  class              bars\n");
    for (int index = 0; index < found; ++index) {
        const struct pci_device *device = pci_device_at(index);
        const char *vendor = pci_vendor_name(device->vendor);
        char row[280];
        int at = 0;

        at += shell_pad_int(row + at, device->bus, 3);
        at += shell_pad_int(row + at, device->device, 4);
        at += shell_pad_int(row + at, device->function, 4);
        at += shell_pad_hex(row + at, device->vendor, 4);
        at += shell_pad_hex(row + at, device->device_id, 5);
        at += shell_pad_text(row + at, pci_class_name(device->class_code,
                                                      device->subclass), 12);
        for (int bar = 0; bar < 6; ++bar) {
            if (device->bar[bar].present == false) {
                continue;
            }
            row[at++] = ' ';
            at += shell_pad_hex(row + at, (uint32_t)device->bar[bar].address, 8);
            if (device->bar[bar].is_io) {
                row[at++] = 'i';
                row[at++] = 'o';
            }
            if (device->bar[bar].unsized) {
                row[at++] = '?';
            }
        }
        row[at] = 0;
        terminal_puts("  ");
        terminal_puts(row);
        if (vendor != 0) {
            terminal_puts("  ");
            terminal_puts(vendor);
        }
        terminal_puts("\n");
        /* the sizes and the virtual addresses go on their own lines, because
         * putting them on the device line runs past 80 columns and wraps */
        for (int bar = 0; bar < 6; ++bar) {
            if (device->bar[bar].present == false) {
                continue;
            }

        if (device->bar[bar].size == 0U) {
                terminal_puts("      bar");
                shell_printf_u64((uint64_t)bar);
                terminal_puts(device->bar[bar].unsized ? ": no size reported\n"
                                                        : ": unused\n");
                continue;
            }
            terminal_puts("      bar");
            shell_printf_u64((uint64_t)bar);
            terminal_puts(device->bar[bar].is_io ? " io   " : " mem  ");
            shell_printf_u64((uint64_t)device->bar[bar].size);
            terminal_puts(" bytes");
            if (device->bar[bar].mapped) {
                /* the first word of device memory, which is how to tell a
                 * working mapping from one that reads nothing */
                terminal_puts("  reads ");
                shell_printf_hex_line(
                    *(volatile uint32_t *)(uintptr_t)device->bar[bar].virtual_address);
                terminal_puts("  at ");
                shell_printf_u64(device->bar[bar].virtual_address);
            } else if (device->bar[bar].is_io == false) {
                terminal_puts("  (not mapped)");
            }
            terminal_puts("\n");
        }
    }
    if (found == 0) {
        terminal_puts("  no PCI bus\n");
        return;
    }
    terminal_puts("  ");
    shell_printf_u64((uint64_t)found);
    terminal_puts(" device(s)");
    if (map) {
        terminal_puts("; sized, memory ones mapped, decoding enabled\n");
    } else {
        terminal_puts("; base address registers as set, not sized or mapped\n");
    }
}

/* A decimal or hex number, right aligned in `width`, for the tables the shell
 * prints.  The kernel has no printf with a width specifier, and a bus listing
 * is unreadable without the columns lining up. */
static int shell_pad_int(char *out, uint32_t value, int width)
{
    char digits[16];
    int length = 0;
    int at = 0;

    if (value == 0U) {
        digits[length++] = '0';
    }
    while (value != 0U && length < (int)sizeof(digits)) {
        digits[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    for (int pad = length; pad < width; ++pad) {
        out[at++] = ' ';
    }
    for (int index = length - 1; index >= 0; --index) {
        out[at++] = digits[index];
    }
    return at;
}

static int shell_pad_hex(char *out, uint32_t value, int width)
{
    static const char digits[] = "0123456789abcdef";
    char text[16];
    int length = 0;
    int at = 0;

    if (value == 0U) {
        text[length++] = '0';
    }
    while (value != 0U && length < (int)sizeof(text)) {
        text[length++] = digits[value & 0xFU];
        value >>= 4;
    }
    for (int pad = length; pad < width; ++pad) {
        out[at++] = ' ';
    }
    for (int index = length - 1; index >= 0; --index) {
        out[at++] = text[index];
    }
    return at;
}

/* A number in hex, for a device id. */
static void shell_printf_hex_line(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[16];
    int length = 0;

    if (value == 0U) {
        text[length++] = '0';
    }
    while (value != 0U && length < 8) {
        text[length++] = digits[value & 0xFU];
        value >>= 4;
    }
    for (int index = length - 1; index >= 0; --index) {
        { char one[2] = { text[index], 0 }; terminal_puts(one); }
    }
}

/* A string, left aligned in `width`, for the same tables.  It does not
 * terminate: these rows are built up in place and the caller ends the string
 * once, and text_copy's habit of writing a terminator would cut the rest of
 * the row off at the first call. */
static int shell_pad_text(char *out, const char *text, int width)
{
    int length = text_length(text);
    int at = 0;

    for (int index = 0; index < length && index < width; ++index) {
        out[at++] = text[index];
    }
    for (int pad = (length > width ? width : length); pad < width; ++pad) {
        out[at++] = ' ';
    }
    return at;
}

/* "vblkread" reads one sector through the virtio queue.
 *
 * The point of this command is the comparison, not the reading.  The same
 * sector can be read with ataread, which goes out on the legacy PIO path and
 * has been working, so a virtio result has something to be checked against
 * rather than merely looked at. */
static void cmd_vblkread(char **tokens, int count)
{
    static uint8_t sector[512];
    struct virtio_device *device;
    uint64_t lba = 0U;
    int rc;

    if (count < 2) {
        terminal_puts("vblkread: usage: vblkread <lba>\n");
        return;
    }
    for (int at = 0; tokens[1][at] != 0; ++at) {
        if (tokens[1][at] < '0' || tokens[1][at] > '9') {
            terminal_puts("vblkread: not a number\n");
            return;
        }
        lba = lba * 10U + (uint64_t)(tokens[1][at] - '0');
    }
    device = virtio_find(1U);
    if (device == 0) {
        terminal_puts("vblkread: no virtio block device\n");
        return;
    }
    /* Set up only if something has not already.  Setting up writes zero to the
     * status register, which resets the device and throws away the queue it
     * was given, so doing it again before every request would undo the last
     * one. */
    if (device->ready == false && virtio_setup(device) == false) {
        terminal_puts("vblkread: setup failed: ");
        terminal_puts(virtio_error());
        terminal_puts("\n");
        return;
    }
    for (int at = 0; at < 512; ++at) {
        sector[at] = 0;
    }
    rc = virtio_blk_read(device, lba, sector);
    terminal_puts("  virtio sector ");
    shell_printf_u64(lba);
    if (rc != 0) {
        terminal_puts(": request failed, code ");
        shell_printf_u64((uint64_t)(-rc));
        return;
    }
    terminal_puts(" read back\n    first 32 bytes:");
    for (int at = 0; at < 32; ++at) {
        char pair[3];

        pair[0] = "0123456789abcdef"[sector[at] >> 4];
        pair[1] = "0123456789abcdef"[sector[at] & 0xFU];
        pair[2] = 0;
        if ((at % 8) == 0) {
            terminal_puts("\n     ");
        }
        terminal_puts(pair);
        terminal_puts(" ");
    }
    terminal_puts("\n    used ");
    shell_printf_u64((uint64_t)virtio_queue_used(device, 0));
    terminal_puts("  free ");
    shell_printf_u64((uint64_t)virtio_queue_free(device, 0));
    terminal_puts("\n");
}

/* "virtio" finds a virtio device, maps it and sets up its queues, then
 * prints what was negotiated.  The transport only: no data has moved through
 * the queue yet, so the numbers here are what the device offered and what this
 * driver agreed to, not the result of a transfer. */
static void cmd_virtio(void)
{
    struct virtio_device *device = virtio_find(1U); /* 1 is block */
    const struct pci_device *pci;

    if (device == 0) {
        terminal_puts("  no virtio block device on the bus\n");
        terminal_puts("  try: qemu -device virtio-blk-pci,drive=...\n");
        return;
    }
    pci = device->pci;
    terminal_puts("  virtio block device\n");
    terminal_puts("    interface  ");
    terminal_puts(device->use_modern ? "modern (memory)\n" : "legacy (ports)\n");
    terminal_puts("    vendor  1af4  device  ");
    shell_printf_hex_line(device->device_id);
    terminal_puts("\n");

    if (virtio_setup(device) == false) {
        terminal_puts("    setup failed: ");
        terminal_puts(virtio_error());
        terminal_puts("\n");
        return;
    }
    terminal_puts("    device features  0x");
    shell_printf_u64(device->device_features);
    terminal_puts("\n    driver features  0x");
    shell_printf_u64(device->driver_features);
    terminal_puts("\n    status  driver ok, features negotiated\n");
    terminal_puts("    queues   ");
    shell_printf_u64((uint64_t)device->queue_count);
    terminal_puts("\n");
    for (int index = 0; index < 2 && index < (int)device->queue_count; ++index) {
        terminal_puts("      q");
        shell_printf_u64((uint64_t)index);
        terminal_puts("  size ");
        shell_printf_u64((uint64_t)device->queue[index].size);
        terminal_puts("  free ");
        shell_printf_u64((uint64_t)virtio_queue_free(device, index));
        terminal_puts("  used ");
        shell_printf_u64((uint64_t)virtio_queue_used(device, index));
        terminal_puts("\n");
    }
    (void)pci;
}

/* "usertest" starts the ring 3 test program.
 *
 * It does come back, and that is correct: the program is started as a task of
 * its own, so this only waits for the scheduler to take it.  It used not to --
 * the test was entered with an iretq from here and never returned, which is
 * why the scheduler work was needed at all, and why the message that used to
 * sit under this said coming back was unexpected.  It was, then.
 */
static void cmd_usertest(void)
{
    if (user_run_test() == false) {
        terminal_puts("  usertest: could not start the test program\n");
        return;
    }
    terminal_puts("  usertest: started; its output follows on the serial port\n");
}

/* "elf <path>" loads an ELF executable from the filesystem and runs it.
 *
 * This is the first program that has ever been loaded from a file rather than
 * linked into the kernel, so it is the point where the loader is exercised for
 * real.  The program becomes a task of its own, exactly as the ring 3 test
 * does, and its output arrives on the serial port while the shell is still
 * here to have started it. */
static void cmd_elf(const char *path)
{
    if (path == 0 || path[0] == 0) {
        terminal_puts("  elf: give a path, for example: elf /bin/hello.elf\n");
        return;
    }
    if (elf_run(path) == false) {
        terminal_puts("  elf: ");
        terminal_puts(elf_error());
        terminal_puts("\n");
        return;
    }
    terminal_puts("  elf: started; its output follows on the serial port\n");
}

static void cmd_sysinfo(void)
{
    char duration[32];

    terminal_puts("Klye Virtual PC\n");
    terminal_puts("----------------\n");
    shell_row("CPU", "x86_64 (qemu)", 16);
    shell_row("Cores", "1", 16);
    shell_row("Memory", "512 MiB", 16);
    terminal_puts("Display:      ");
    shell_printf_u64(gfx_width());
    terminal_puts("x");
    shell_printf_u64(gfx_height());
    terminal_puts(" 32bpp\n");
    terminal_puts("Compositor:   damage-tracked, 60 fps\n");
    terminal_puts("Frames:       ");
    shell_printf_u64(gfx_present_count());
    terminal_puts(" (");
    shell_printf_u64((uint64_t)gfx_last_present_rows());
    terminal_puts(" rows last)\n");
    shell_row("Mouse", input_mouse_ready() ? "PS/2 aux ready" : "absent", 16);
    terminal_puts("Scancodes:    ");
    shell_printf_u64(input_key_scancodes());
    terminal_puts("\nMouse pkts:   ");
    shell_printf_u64(input_mouse_packets());
    terminal_puts("\nUptime:       ");
    shell_format_uptime(duration, (int)sizeof(duration));
    terminal_puts(duration);
    terminal_puts("\n");
}

static void cmd_neofetch(void)
{
    char duration[32];
    static const char *const logo[] = {
        "        /\\        ",
        "       /  \\       ",
        "      / /\\ \\      ",
        "     / ____ \\     ",
        "    /_/    \\_\\    ",
        "               ",
        "               "
    };
    char value[64];

    for (int index = 0; index < (int)(sizeof(logo) / 8U); ++index) {
        terminal_puts(logo[index]);
        if (index == 3) {
            text_copy(value, state.user, (int)sizeof(value));
            text_copy(value + text_length(value), "@", 2);
            text_copy(value + text_length(value), state.host,
                      (int)sizeof(value) - text_length(value));
            shell_row("", value, 16);
        } else if (index == 4) {
            text_copy(value, "----------------", (int)sizeof(value));
            shell_row("", value, 16);
        } else if (index == 5) {
            text_copy(value, "OS: Klye OS 0.2 x86_64", (int)sizeof(value));
            shell_row("", value, 16);
        } else if (index == 6) {
            text_copy(value, "Kernel: 0.2.0-freestanding", (int)sizeof(value));
            shell_row("", value, 16);
        } else {
            terminal_puts("               \n");
        }
    }
    terminal_puts("\n");
    shell_format_uptime(duration, (int)sizeof(duration));
    text_copy(value, "Uptime: ", (int)sizeof(value));
    text_copy(value + text_length(value), duration,
              (int)sizeof(value) - text_length(value));
    shell_row("", value, 16);
    text_copy(value, "Shell: klye-sh 1.2", (int)sizeof(value));
    shell_row("", value, 16);
    text_copy(value, "Resolution: ", (int)sizeof(value));
    text_copy(value + text_length(value), "1280x720", (int)sizeof(value));
    shell_row("", value, 16);
    text_copy(value, "Compositor: 60 fps", (int)sizeof(value));
    shell_row("", value, 16);
    text_copy(value, "Terminal: ", (int)sizeof(value));
    text_copy(value + text_length(value),
              input_mouse_ready() ? "PS/2 keyboard + mouse" : "keyboard only",
              (int)sizeof(value) - text_length(value));
    shell_row("", value, 16);
    text_copy(value, "Apps: ", (int)sizeof(value));
    {
        int offset = text_length(value);
        int total = 0;

        for (int index = 0; index < APP_COUNT; ++index) {
            if (app_is_open((enum app_id)index)) {
                total++;
            }
        }
        offset += shell_u32_to_text(value + offset, (uint32_t)total);
        text_copy(value + offset, " open", 6);
    }
    shell_row("", value, 16);
}

static void cmd_apps(void)
{
    terminal_puts("installed applications\n");
    for (int index = 0; index < APP_COUNT; ++index) {
        char status[12];

        text_copy(status, app_is_open((enum app_id)index) ? "running" : "idle",
                  (int)sizeof(status));
        shell_pad(status, 10);
        terminal_puts("  ");
        terminal_puts(app_name((enum app_id)index));
        for (int pad = text_length(app_name((enum app_id)index)); pad < 12;
             ++pad) {
            terminal_puts(" ");
        }
        terminal_puts(status);
        terminal_puts(app_title((enum app_id)index));
        terminal_puts("\n");
    }
}

static void cmd_history(void)
{
    for (int index = 0; index < state.history_count; ++index) {
        char number[8];

        shell_u32_to_text(number, (uint32_t)(index + 1));
        shell_pad(number, 5);
        terminal_puts(number);
        terminal_puts(state.history[index]);
        terminal_puts("\n");
    }
}

static void cmd_banner(void)
{
    terminal_puts("\n");
    terminal_puts("  #   #  #    #   #  #####  #####  #####  \n");
    terminal_puts("  #  #   #     # #   #      #   #  #      \n");
    terminal_puts("  ####   #     #    ####    #   #  #####  \n");
    terminal_puts("  #  #   #     #    #       #   #      #  \n");
    terminal_puts("  #   #  #####   #    #####   #####  #####  \n");
    terminal_puts("\n");
}

static void cmd_man(const char *name)
{
    if (text_equal(name, "ls")) {
        terminal_puts("ls [-l] [DIR]\n\n"
                      "  -l   long format with sizes and modes\n"
                      "  DIR  directory to list, defaults to the cwd\n");
        return;
    }
    if (text_equal(name, "cat")) {
        terminal_puts("cat FILE\n\n  print the contents of FILE\n");
        return;
    }
    if (text_equal(name, "uname")) {
        terminal_puts("uname [-a|-r|-m|-n]\n\n"
                      "  -a  all information\n  -r  kernel release\n"
                      "  -m  machine architecture\n  -n  node name\n");
        return;
    }
    if (text_equal(name, "reboot")) {
        terminal_puts("reboot\n\n  restart the machine\n");
        return;
    }
    if (text_equal(name, "touch") || text_equal(name, "mkdir") ||
        text_equal(name, "rm")) {
        terminal_puts(name);
        terminal_puts(" [-r] PATH\n\n"
                      "  -r  recurse into directories (rm)\n"
                      "  PATH  relative to the current directory\n");
        return;
    }
    if (text_equal(name, "echo")) {
        terminal_puts("echo [-n] TEXT [> FILE | >> FILE]\n\n"
                      "  -n   do not print a trailing newline\n"
                      "  >    truncate FILE and write TEXT\n"
                      "  >>   append TEXT to FILE\n");
        return;
    }
    if (text_equal(name, "neofetch")) {
        terminal_puts("neofetch\n\n  print a system summary with the logo\n");
        return;
    }
    if (text_equal(name, "ps")) {
        terminal_puts("ps\n\n  list running tasks and window heartbeats\n");
        return;
    }
    terminal_puts("man: no manual entry for ");
    terminal_puts(name);
    terminal_puts("\n");
    terminal_error();
}

static void shell_write_out(char **tokens, int count, int first, bool append)
{
    char resolved[VFS_PATH_MAX];
    char scratch[VFS_BODY_MAX];
    int length = 0;

    if (count - first < 2) {
        terminal_puts(append ? "echo: missing file after >>\n" : "echo: missing file after >\n");
        terminal_error();
        return;
    }
    vfs_absolute_of(tokens[count - 1], state.cwd, resolved, (int)sizeof(resolved));
    if (vfs_is_directory_path(resolved)) {
        terminal_puts("echo: ");
        terminal_puts(resolved);
        terminal_puts(": Is a directory\n");
        terminal_error();
        return;
    }
    for (int index = first; index < count - 2; ++index) {
        if (index > first) {
            scratch[length++] = ' ';
        }
        for (int position = 0; tokens[index][position] != '\0' && length < (int)sizeof(scratch) - 1; ++position) {
            scratch[length++] = tokens[index][position];
        }
    }
    if (append) {
        scratch[length++] = '\n';
    }
    if ((append ? vfs_append(resolved, scratch, (uint32_t)length)
                : vfs_write(resolved, scratch, (uint32_t)length)) < 0) {
        terminal_puts("echo: ");
        terminal_puts(resolved);
        terminal_puts(": write failed\n");
        terminal_error();
        return;
    }
    if (!append) {
        terminal_puts(scratch);
        terminal_puts("\n");
    }
}

static void cmd_echo(char **tokens, int count)
{
    bool newline = true;
    int start = 1;
    int redirect = -1;
    bool append = false;

    while (start < count &&
           (text_equal(tokens[start], "-n") || text_equal(tokens[start], "-e"))) {
        if (text_equal(tokens[start], "-n")) {
            newline = false;
        }
        start++;
    }
    for (int index = start; index < count; ++index) {
        if (text_equal(tokens[index], ">>")) {
            redirect = index;
            append = true;
            break;
        }
        if (text_equal(tokens[index], ">")) {
            redirect = index;
            break;
        }
    }
    if (redirect >= 0) {
        shell_write_out(tokens, count, start, append);
        return;
    }
    for (int index = start; index < count; ++index) {
        if (index > start) {
            terminal_puts(" ");
        }
        terminal_puts(tokens[index]);
    }
    if (newline) {
        terminal_puts("\n");
    }
}

static void shell_basename(char *out, int max, const char *path)
{
    int last = 0;
    int length = 0;

    for (int index = 0; path[index] != 0; ++index) {
        if (path[index] == '/' || path[index] == '\\') {
            last = index + 1;
        }
    }
    while (path[last + length] != 0 && length < max - 1) {
        out[length] = path[last + length];
        ++length;
    }
    out[length] = 0;
}

static void cmd_kbyrun(char **tokens, int count)
{
    char resolved[VFS_PATH_MAX];
    static uint8_t image[KBY_MAX_CODE];
    char name[KBY_NAME_MAX];
    struct kby_app *app;
    int length;
    int node;

    if (count < 2) {
        terminal_puts("kbyrun: missing program path\n");
        terminal_error();
        return;
    }
    vfs_absolute_of(tokens[1], state.cwd, resolved, (int)sizeof(resolved));
    node = vfs_resolve(resolved, state.cwd);
    if (node <= 0) {
        terminal_puts("kbyrun: ");
        terminal_puts(resolved);
        terminal_puts(": no such file\n");
        terminal_error();
        return;
    }
    if (vfs_size((uint32_t)node) > (uint32_t)sizeof(image)) {
        terminal_puts("kbyrun: program is too large\n");
        terminal_error();
        return;
    }
    length = vfs_read(resolved, (char *)image, (uint32_t)sizeof(image));
    if (length <= 0) {
        terminal_puts("kbyrun: cannot read program\n");
        terminal_error();
        return;
    }
    shell_basename(name, (int)sizeof(name), tokens[1]);
    app = kby_load(name, image, (uint32_t)length);
    if (app == 0) {
        terminal_puts("kbyrun: ");
        terminal_puts(kby_last_error() != 0 ? kby_last_error() : "load failed");
        terminal_puts("\n");
        terminal_error();
        return;
    }
    while (kby_app_loaded(app)) {
        kby_run(app, KBY_BUDGET);
        kby_flush_output(app);
    }
    kby_flush_output(app);
    terminal_puts("kbyrun: ");
    terminal_puts(name);
    terminal_puts(" steps ");
    terminal_printf_number((uint64_t)kby_app_steps(app));
    terminal_puts(", result ");
    terminal_printf_number((uint64_t)kby_app_result(app));
    if (kby_error(app) != 0) {
        terminal_puts(", stopped: ");
        terminal_puts(kby_error(app));
    } else if (kby_app_starved(app)) {
        terminal_puts(", STARVED: hit the per-frame budget without reaching vsync");
    }
    terminal_puts("\n");
    kby_unload(app);
}

static char dumpmem_message[128];

static int dumpmem_text(char *destination, int at, const char *text)
{
    int index = 0;

    while (text[index] != 0) {
        destination[at + index] = text[index];
        ++index;
    }
    destination[at + index] = 0;
    return at + index;
}

static int dumpmem_number(char *destination, int at, uint32_t value)
{
    return at + shell_u32_to_text(destination + at, value);
}

static void dumpmem_fill(uint8_t *bytes, uint32_t length, uint8_t seed)
{
    for (uint32_t index = 0; index < length; ++index) {
        bytes[index] = (uint8_t)(seed + (index & 0x1FU));
    }
}

static bool dumpmem_verify(const uint8_t *bytes, uint32_t length, uint8_t seed)
{
    for (uint32_t index = 0; index < length; ++index) {
        if (bytes[index] != (uint8_t)(seed + (index & 0x1FU))) {
            return false;
        }
    }
    return true;
}

static void cmd_dumpmem(char **tokens, int count)
{
    uint32_t block = 64U * 1024U;
    uint32_t limit = 0U;
    uint32_t total = 0U;
    uint32_t blocks = 0U;
    uint8_t seed = 0U;
    int at;

    if (count > 1) {
        block = (uint32_t)shell_atoi_value(tokens[1]) * 1024U;
        if (block < 4U * 1024U) {
            block = 4U * 1024U;
        }
        if (block > 1024U * 1024U) {
            block = 1024U * 1024U;
        }
    }
    if (count > 2) {
        limit = (uint32_t)shell_atoi_value(tokens[2]) * 1024U * 1024U;
    }

    terminal_puts("dumpmem: block ");
    terminal_printf_number((uint64_t)block / 1024U);
    terminal_puts(" KiB, arena free ");
    terminal_printf_number((uint64_t)gfx_frame_bytes_free() / 1024U);
    terminal_puts(" KiB\n");

    for (;;) {
        uint8_t *chunk = (uint8_t *)gfx_alloc(block);

        if (chunk == 0) {
            break;
        }
        dumpmem_fill(chunk, block, seed);
        if (dumpmem_verify(chunk, block, seed) == false) {
            at = dumpmem_text(dumpmem_message, 0,
                              "dumpmem: pattern check failed at ");
            at = dumpmem_number(dumpmem_message, at, total);
            dumpmem_text(dumpmem_message, at, " bytes");
            panic(dumpmem_message);
        }
        total += block;
        blocks++;
        seed = (uint8_t)(seed + 1U);
        if ((blocks % 16U) == 0U) {
            terminal_puts("  allocated ");
            terminal_printf_number((uint64_t)total / 1024U);
            terminal_puts(" KiB in ");
            terminal_printf_number((uint64_t)blocks);
            terminal_puts(" blocks\n");
        }
        if (limit != 0U && total >= limit) {
            terminal_puts("dumpmem: reached requested limit, stopping\n");
            terminal_puts("  total ");
            terminal_printf_number((uint64_t)total / 1024U);
            terminal_puts(" KiB, free now ");
            terminal_printf_number((uint64_t)gfx_frame_bytes_free() / 1024U);
            terminal_puts(" KiB\n");
            return;
        }
    }

    at = dumpmem_text(dumpmem_message, 0, "dumpmem: out of memory after ");
    at = dumpmem_number(dumpmem_message, at, total / 1024U);
    at = dumpmem_text(dumpmem_message, at, " KiB in ");
    at = dumpmem_number(dumpmem_message, at, blocks);
    dumpmem_text(dumpmem_message, at, " blocks");
    panic(dumpmem_message);
}

static void cmd_touch(char **tokens, int count)
{
    for (int index = 1; index < count; ++index) {
        char resolved[VFS_PATH_MAX];

        if (text_equal(tokens[index], "-l")) {
            continue;
        }
        vfs_absolute_of(tokens[index], state.cwd, resolved, (int)sizeof(resolved));
        if (vfs_is_directory_path(resolved)) {
            terminal_puts("touch: ");
            terminal_puts(resolved);
            terminal_puts(": Is a directory\n");
            terminal_error();
            continue;
        }
        if (vfs_touch(resolved) < 0) {
            terminal_puts("touch: cannot touch ");
            terminal_puts(resolved);
            terminal_puts("\n");
            terminal_error();
            continue;
        }
        terminal_puts("created ");
        terminal_puts(resolved);
        terminal_puts("\n");
    }
}

static void cmd_mkdir(char **tokens, int count)
{
    for (int index = 1; index < count; ++index) {
        char resolved[VFS_PATH_MAX];

        if (text_equal(tokens[index], "-p")) {
            continue;
        }
        vfs_absolute_of(tokens[index], state.cwd, resolved, (int)sizeof(resolved));
        if (vfs_create(resolved, true) < 0) {
            terminal_puts("mkdir: cannot create directory ");
            terminal_puts(resolved);
            terminal_puts("\n");
            terminal_error();
            continue;
        }
        terminal_puts("created ");
        terminal_puts(resolved);
        terminal_puts("/\n");
    }
}

static void cmd_rm(char **tokens, int count)
{
    bool recursive = false;

    for (int index = 1; index < count; ++index) {
        char resolved[VFS_PATH_MAX];

        if (text_equal(tokens[index], "-r") || text_equal(tokens[index], "-rf") ||
            text_equal(tokens[index], "-fr")) {
            recursive = true;
            continue;
        }
        int node;

        vfs_absolute_of(tokens[index], state.cwd, resolved, (int)sizeof(resolved));
        node = vfs_resolve(resolved, state.cwd);
        if (node <= 0) {
            terminal_puts("rm: ");
            terminal_puts(resolved);
            terminal_puts(": No such file or directory\n");
            terminal_error();
            continue;
        }
        if (vfs_kind(node)[0] == 'd' && !recursive) {
            terminal_puts("rm: ");
            terminal_puts(resolved);
            terminal_puts(": is a directory\n");
            terminal_error();
            continue;
        }
        if (vfs_delete(resolved) < 0) {
            terminal_puts("rm: cannot remove ");
            terminal_puts(resolved);
            terminal_puts("\n");
            terminal_error();
            continue;
        }
        terminal_puts("removed ");
        terminal_puts(resolved);
        terminal_puts("\n");
    }
}

/* A launcher entry names a file; an absolute path is used as-is and a bare
 * name is looked up in /bin. */
static void take_path(char *out, int max, const char *path)
{
    int used = 0;

    if (path[0] == '/') {
        while (path[used] != 0 && used < max - 1) {
            out[used] = path[used];
            ++used;
        }
        out[used] = 0;
        return;
    }
    out[used++] = '/';
    out[used++] = 'b';
    out[used++] = 'i';
    out[used++] = 'n';
    out[used++] = '/';
    while (path[used - 5] != 0 && used < max - 1) {
        out[used] = path[used - 5];
        ++used;
    }
    out[used] = 0;
}

static bool shell_run_script(const char *name);
static bool shell_run_lua(const char *name);

static int shell_launcher_target(const char *name, char *entry, int entry_max)
{
    int slot = launcher_resolve(name);
    int builtin;

    if (slot >= 0) {
        if (launcher_is_builtin(slot, entry, entry_max)) {
            return app_from_name(entry);
        }
        if (launcher_is_bytecode(slot)) {
            return -2;
        }
        return -3;
    }
    builtin = app_from_name(name);
    if (builtin >= 0) {
        text_copy(entry, name, entry_max);
        return builtin;
    }
    return -1;
}

static bool shell_run_script(const char *name)
{
    int slot = launcher_resolve(name);
    const struct launcher *item;
    char path[VFS_PATH_MAX];
    char resolved[VFS_PATH_MAX];
    static uint8_t image[KBY_MAX_CODE];
    struct kby_app *app;
    int length;

    if (slot < 0) {
        terminal_puts("open: '");
        terminal_puts(name);
        terminal_puts("': no such program in /bin\n");
        terminal_error();
        return false;
    }
    item = launcher_at(slot);
    take_path(path, (int)sizeof(path), item->entry);
    vfs_absolute_of(path, "/", resolved, (int)sizeof(resolved));
    if (vfs_exists(resolved) == false) {
        terminal_puts("open: ");
        terminal_puts(resolved);
        terminal_puts(": program not found\n");
        terminal_error();
        return false;
    }
    length = vfs_read(resolved, (char *)image, (uint32_t)sizeof(image));
    if (length <= 0) {
        terminal_puts("open: cannot read ");
        terminal_puts(resolved);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    app = kby_load(item->file, image, (uint32_t)length);
    if (app == 0) {
        terminal_puts("open: ");
        terminal_puts(kby_last_error() != 0 ? kby_last_error() : "bad image");
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    wm_launch_script(kby_app_slot(app), item->title, 520, 380);
    return true;
}

bool shell_run_app(const char *name)
{
    char entry[LAUNCHER_ENTRY_MAX];
    int builtin = shell_launcher_target(name, entry, (int)sizeof(entry));

    if (builtin == -2) {
        return shell_run_script(name);
    }
    if (builtin == -3) {
        return shell_run_lua(name);
    }
    if (builtin < 0) {
        terminal_puts("open: unknown application '");
        terminal_puts(name);
        terminal_puts("'\n");
        terminal_error();
        return false;
    }
    wm_launch_app((enum app_id)builtin);
    return true;
}


/* ---------------------------------------------------------------- kpm ---- */

#define KPM_SOURCE_MAX 8192
#define KPM_IMAGE_MAX 4096

static void kpm_usage(void)
{
    terminal_puts("usage:\n");
    terminal_puts("  kpm list                  installed programs\n");
    terminal_puts("  kpm build <name>.kby      assemble and install\n");
    terminal_puts("  kpm remove <name>         uninstall\n");
    terminal_puts("  kpm run <name>            run an installed program\n");
}

/* copies name into out with a known suffix removed; false if absent */
static bool kpm_stem(const char *name, const char *suffix, char *out, int max)
{
    int stem = text_length(name);
    int cut = text_length(suffix);
    int start = 0;

    if (stem <= cut) {
        return false;
    }
    for (int index = 0; index < cut; ++index) {
        if (name[stem - cut + index] != suffix[index]) {
            return false;
        }
    }
    stem -= cut;
    /* Only the last path component is the program's name.  Keeping the
     * directory turns "kpm build lua/clock.lua" into an install to
     * /bin/lua/clock.lua, whose parent does not exist, so the write fails for
     * every script that has a directory in its name.  That is all of them:
     * the sources live in /home/klye/lua. */
    for (int index = 0; index < stem; ++index) {
        if (name[index] == '/') {
            start = index + 1;
        }
    }
    stem -= start;
    if (stem >= max) {
        stem = max - 1;
    }
    for (int index = 0; index < stem; ++index) {
        out[index] = name[start + index];
    }
    out[stem] = 0;
    return true;
}

/* builds "/bin/<stem>.kbin" */
static void kpm_bin_path(const char *stem, const char *ext, char *out, int max)
{
    int at = 0;
    const char *prefix = "/bin/";

    for (int index = 0; prefix[index] != 0 && at < max - 1; ++index) {
        out[at++] = prefix[index];
    }
    for (int index = 0; stem[index] != 0 && at < max - 1; ++index) {
        out[at++] = stem[index];
    }
    for (int index = 0; ext[index] != 0 && at < max - 1; ++index) {
        out[at++] = ext[index];
    }
    out[at] = 0;
}

static void kpm_report(const char *what, const char *detail)
{
    terminal_puts("kpm ");
    terminal_puts(what);
    terminal_puts(": ");
    terminal_puts(detail);
    terminal_puts("\n");
    terminal_error();
}

static void kpm_list(void)
{
    int found = 0;

    for (int index = 0; index < launcher_count(); ++index) {
        const struct launcher *item = launcher_at(index);

        if (item == 0 || item->is_record == false) {
            continue;
        }
        terminal_puts("  ");
        terminal_puts(item->file);
        terminal_puts("  ");
        terminal_puts(item->title);
        terminal_puts("  (");
        terminal_puts(item->kind);
        terminal_puts(")\n");
        ++found;
    }
    if (found == 0) {
        terminal_puts("  nothing installed\n");
    }
}

/* kpm build handles two kinds of source.  A .kby file is assembled to bytecode
 * in the kernel; a .lua file is copied verbatim, because Lua is interpreted and
 * needs no build step.  Either way the result lands in /bin next to a launcher
 * record so it appears in the dock. */
static bool kpm_install_lua(const char *name)
{
    static char source[8192];
    char source_path[VFS_PATH_MAX];
    char script_path[VFS_PATH_MAX];
    char record_path[VFS_PATH_MAX];
    char stem[LAUNCHER_NAME_MAX];
    char entry[LAUNCHER_ENTRY_MAX];
    char line[LAUNCHER_NAME_MAX * 2 + 96];
    int length;
    int at;

    if (kpm_stem(name, ".lua", stem, (int)sizeof(stem)) == false) {
        kpm_report("build", "name must end in .lua");
        return false;
    }
    {
        static const char prefix[] = "/home/klye/";
        int used = 0;

        for (int index = 0; prefix[index] != 0; ++index) {
            source_path[used++] = prefix[index];
        }
        for (int index = 0; name[index] != 0 &&
             used < (int)sizeof(source_path) - 1; ++index) {
            source_path[used++] = name[index];
        }
        source_path[used] = 0;
    }
    if (vfs_exists(source_path) == false) {
        terminal_puts("kpm build: no source at ");
        terminal_puts(source_path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    length = vfs_read(source_path, source, (uint32_t)sizeof(source));
    if (length <= 0) {
        terminal_puts("kpm build: cannot read ");
        terminal_puts(source_path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    kpm_bin_path(stem, ".lua", script_path, (int)sizeof(script_path));
    if (vfs_write(script_path, source, (uint32_t)length) < 0) {
        kpm_report("build", "cannot write the script");
        return false;
    }
    kpm_bin_path(stem, "", record_path, (int)sizeof(record_path));
    take_path(entry, (int)sizeof(entry), stem);
    {
        static const char head1[] = "title=";
        static const char head2[] = "\nkind=lua\nentry=";
        static const char tail[] = ".lua\nicon=-1\n";

        at = 0;
        text_copy(line + at, head1, (int)sizeof(line) - at);
        at += (int)sizeof(head1) - 1;
        text_copy(line + at, stem, (int)sizeof(line) - at);
        at += text_length(stem);
        text_copy(line + at, head2, (int)sizeof(line) - at);
        at += (int)sizeof(head2) - 1;
        text_copy(line + at, entry, (int)sizeof(line) - at);
        at += text_length(entry);
        text_copy(line + at, tail, (int)sizeof(line) - at);
        at += (int)sizeof(tail) - 1;
        line[at] = 0;
    }
    if (vfs_write(record_path, line, (uint32_t)at) < 0) {
        kpm_report("build", "cannot write the launcher record");
        return false;
    }
    launcher_scan();
    terminal_puts("installed ");
    terminal_puts(stem);
    terminal_puts(" (lua, ");
    shell_printf_u64((uint64_t)length);
    terminal_puts(" bytes)\n");
    return true;
}

static bool kpm_build(const char *name)
{
    static char source[KPM_SOURCE_MAX];
    static uint8_t image[KPM_IMAGE_MAX];
    char source_path[VFS_PATH_MAX];
    char binary_path[VFS_PATH_MAX];
    char record_path[VFS_PATH_MAX];
    char record[LAUNCHER_NAME_MAX];
    char stem[LAUNCHER_NAME_MAX];
    char line[LAUNCHER_NAME_MAX * 2 + 64];
    const char *why = 0;
    int length;
    int bytes = 0;
    int at = 0;

    if (kpm_stem(name, ".lua", stem, (int)sizeof(stem))) {
        return kpm_install_lua(name);
    }
    if (kpm_stem(name, ".kby", stem, (int)sizeof(stem)) == false) {
        kpm_report("build", "name must end in .kby or .lua");
        return false;
    }
    /* look in the home directory, then in the kby source folder */
    {
        static const char *const dirs[2] = { "/home/klye/", "/home/klye/kby/" };
        int found = 0;

        source_path[0] = 0;
        for (int which = 0; which < 2 && found == 0; ++which) {
            int used = 0;

            for (int index = 0; dirs[which][index] != 0; ++index) {
                source_path[used++] = dirs[which][index];
            }
            for (int index = 0; name[index] != 0 &&
                 used < (int)sizeof(source_path) - 1; ++index) {
                source_path[used++] = name[index];
            }
            source_path[used] = 0;
            if (vfs_exists(source_path) != 0) {
                found = 1;
            }
        }
    }
    if (source_path[0] == 0) {
        terminal_puts("kpm build: no source at ");
        terminal_puts(name);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    length = vfs_read(source_path, source, (uint32_t)sizeof(source));
    if (length <= 0) {
        terminal_puts("kpm build: cannot read ");
        terminal_puts(source_path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    if (kas_assemble(source, length, image, (int)sizeof(image), &bytes,
                     &why) == false) {
        terminal_puts("kpm build: ");
        terminal_puts(why != 0 ? why : "assembly failed");
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    kpm_bin_path(stem, ".kbin", binary_path, (int)sizeof(binary_path));
    if (vfs_write(binary_path, (const char *)image, (uint32_t)bytes) < 0) {
        kpm_report("build", "cannot write the program image");
        return false;
    }
    kpm_bin_path(stem, "", record_path, (int)sizeof(record_path));
    /* build the record with sizeof literals so a length can never drift */
    {
        static const char title_key[] = "title=";
        static const char kind_key[] = "\nkind=kby\nentry=";
        static const char tail[] = ".kbin\nicon=-1\n";

        text_copy(line, title_key, (int)sizeof(line));
        at = (int)sizeof(title_key) - 1;
        text_copy(line + at, stem, (int)sizeof(line) - at);
        at += text_length(stem);
        text_copy(line + at, kind_key, (int)sizeof(line) - at);
        at += (int)sizeof(kind_key) - 1;
        text_copy(line + at, stem, (int)sizeof(line) - at);
        at += text_length(stem);
        text_copy(line + at, tail, (int)sizeof(line) - at);
        at += (int)sizeof(tail) - 1;
        line[at] = 0;
    }
    if (vfs_write(record_path, line, (uint32_t)at) < 0) {
        kpm_report("build", "cannot write the launcher record");
        return false;
    }
    text_copy(record, stem, (int)sizeof(record));
    launcher_scan();
    terminal_puts("installed ");
    terminal_puts(record);
    terminal_puts(" (");
    shell_printf_u64((uint64_t)bytes);
    terminal_puts(" bytes)\n");
    return true;
}

static bool kpm_remove(const char *name)
{
    char record_path[VFS_PATH_MAX];
    char binary_path[VFS_PATH_MAX];
    char stem[LAUNCHER_NAME_MAX];

    if (kpm_stem(name, "", stem, (int)sizeof(stem)) == false &&
        kpm_stem(name, ".kby", stem, (int)sizeof(stem)) == false) {
        kpm_report("remove", "bad program name");
        return false;
    }
    kpm_bin_path(stem, "", record_path, (int)sizeof(record_path));
    if (vfs_exists(record_path) == false) {
        kpm_report("remove", "not installed");
        return false;
    }
    vfs_delete(record_path);
    kpm_bin_path(stem, ".kbin", binary_path, (int)sizeof(binary_path));
    vfs_delete(binary_path);
    {
        char script_path[VFS_PATH_MAX];

        kpm_bin_path(stem, ".lua", script_path, (int)sizeof(script_path));
        vfs_delete(script_path);
    }
    launcher_scan();
    terminal_puts("removed ");
    terminal_puts(stem);
    terminal_puts("\n");
    return true;
}

static void cmd_kpm(char **tokens, int count)
{
    if (count < 2) {
        kpm_usage();
        return;
    }
    if (text_equal(tokens[1], "list") || text_equal(tokens[1], "ls")) {
        kpm_list();
    } else if (text_equal(tokens[1], "build") || text_equal(tokens[1], "install")) {
        kpm_build(count > 2 ? tokens[2] : "");
    } else if (text_equal(tokens[1], "remove") || text_equal(tokens[1], "rm")) {
        kpm_remove(count > 2 ? tokens[2] : "");
    } else if (text_equal(tokens[1], "run")) {
        if (count > 2) {
            shell_run_app(tokens[2]);
        } else {
            kpm_usage();
        }
    } else {
        kpm_usage();
    }
}


/* Runs a .lua file from the filesystem.  Without a window this is the quick
 * way to check a script; `open` gives it a real window instead. */
static bool cmd_lua(const char *name)
{
    static char source[8192];
    static char path[VFS_PATH_MAX];
    char error[256];
    int length;

    if (name[0] == 0) {
        terminal_puts("lua: missing script name\n");
        terminal_error();
        return false;
    }
    {
        static const char *const dirs[2] = { "/home/klye/", "/bin/" };
        int found = 0;

        path[0] = 0;
        for (int which = 0; which < 2 && found == 0; ++which) {
            int at = 0;

            for (int index = 0; dirs[which][index] != 0; ++index) {
                path[at++] = dirs[which][index];
            }
            for (int index = 0; name[index] != 0 &&
                 at < (int)sizeof(path) - 1; ++index) {
                path[at++] = name[index];
            }
            path[at] = 0;
            if (vfs_exists(path) != 0) {
                found = 1;
            }
        }
        if (found == 0) {
            terminal_puts("lua: no script at ");
            terminal_puts(name);
            terminal_puts("\n");
            terminal_error();
            return false;
        }
    }
    length = vfs_read(path, source, (uint32_t)sizeof(source));
    if (length <= 0) {
        terminal_puts("lua: cannot read ");
        terminal_puts(path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    if (lua_host_run_once(name, source, length, error, (int)sizeof(error))) {
        terminal_puts("lua: ");
        terminal_puts(path);
        terminal_puts(" ran with no errors\n");
        return true;
    }
    terminal_puts("lua: ");
    terminal_puts(error[0] != 0 ? error : "unknown error");
    terminal_puts("\n");
    terminal_error();
    return false;
}

/* Loads a .lua launcher into a windowed host.  The window manager keeps the
 * host alive, so a Lua app owns a real lua_State for as long as its window is
 * open and keeps whatever tables and closures the script created. */
static bool shell_run_lua(const char *name)
{
    static char source[8192];
    int slot = launcher_resolve(name);
    const struct launcher *item;
    char path[VFS_PATH_MAX];
    char error[256];
    struct lua_host *host;
    int length;

    if (slot < 0) {
        terminal_puts("open: '");
        terminal_puts(name);
        terminal_puts("': no such program in /bin\n");
        terminal_error();
        return false;
    }
    item = launcher_at(slot);
    if (item == 0) {
        terminal_puts("open: bad launcher record\n");
        terminal_error();
        return false;
    }
    take_path(path, (int)sizeof(path), item->entry);
    vfs_absolute_of(path, "/", path, (int)sizeof(path));
    if (vfs_exists(path) == false) {
        terminal_puts("open: ");
        terminal_puts(path);
        terminal_puts(": script not found\n");
        terminal_error();
        return false;
    }
    length = vfs_read(path, source, (uint32_t)sizeof(source));
    if (length <= 0) {
        terminal_puts("open: cannot read ");
        terminal_puts(path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    host = lua_host_load(item->file, source, length, error, (int)sizeof(error));
    if (host == 0) {
        terminal_puts("open: ");
        terminal_puts(error[0] != 0 ? error : "cannot start");
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    if (lua_host_loaded(host) == false) {
        terminal_puts("open: ");
        terminal_puts(lua_host_error(host) != 0 ? lua_host_error(host)
                                                : "script error");
        terminal_puts("\n");
        terminal_error();
        return true;
    }
    wm_launch_lua(lua_host_index(host), item->title, 520, 380);
    return true;
}

/* Services a lua host synchronously, for tests that need to know whether a
 * fault is in the interpreter or in the task context it runs under. */
static bool cmd_luastep(const char *name, uint32_t times)
{
    static char source[8192];
    char path[VFS_PATH_MAX];
    char error[256];
    struct lua_host *host;
    int length;
    int at = 0;

    while (at < (int)sizeof(path) - 1 && name[at] != 0) {
        path[at] = name[at];
        ++at;
    }
    path[at] = 0;
    if (path[0] != '/') {
        static const char prefix[] = "/home/klye/";

        at = 0;
        for (int index = 0; prefix[index] != 0; ++index) {
            path[at++] = prefix[index];
        }
        for (int index = 0; name[index] != 0 && at < (int)sizeof(path) - 1;
             ++index) {
            path[at++] = name[index];
        }
        path[at] = 0;
    }
    length = vfs_read(path, source, (uint32_t)sizeof(source));
    if (length <= 0) {
        terminal_puts("luastep: cannot read ");
        terminal_puts(path);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    host = lua_host_load("step", source, length, error, (int)sizeof(error));
    if (host == 0) {
        terminal_puts("luastep: ");
        terminal_puts(error);
        terminal_puts("\n");
        terminal_error();
        return false;
    }
    for (uint32_t step = 0; step < times; ++step) {
        lua_host_service(host);
    }
    terminal_puts("luastep: ");
    shell_printf_u64((uint64_t)times);
    terminal_puts(" steps, frame ");
    shell_printf_u64((uint64_t)lua_host_frame(host));
    terminal_puts(", draw ");
    shell_printf_u64((uint64_t)lua_host_draw_count(host));
    terminal_puts("\n");
    if (lua_host_loaded(host) == false) {
        terminal_puts("luastep: failed: ");
        terminal_puts(lua_host_error(host) != 0 ? lua_host_error(host) : "?");
        terminal_puts("\n");
    }
    lua_host_unload(host);
    return true;
}

static void cmd_which(char **tokens, int count)
{
    if (count < 2) {
        terminal_puts("which: missing program name\n");
        terminal_error();
        return;
    }
    for (int index = 1; index < count; ++index) {
        int slot = launcher_resolve(tokens[index]);

        if (slot >= 0) {
            const struct launcher *item = launcher_at(slot);

            terminal_puts(tokens[index]);
            terminal_puts(" -> ");
            terminal_puts(item->path);
            terminal_puts(" (");
            terminal_puts(item->kind);
            if (item->entry[0] != 0) {
                terminal_puts(" ");
                terminal_puts(item->entry);
            }
            terminal_puts(")\n");
        } else {
            terminal_puts(tokens[index]);
            terminal_puts(": not found in path\n");
        }
    }
}

static void cmd_launchers(void)
{
    int total = launcher_count();

    terminal_puts("launchers from /bin\n");
    if (total == 0) {
        terminal_puts("  (none - filesystem is empty)\n");
        return;
    }
    for (int index = 0; index < total; ++index) {
        const struct launcher *item = launcher_at(index);

        terminal_puts("  ");
        terminal_puts(item->file);
        terminal_puts("  title=");
        terminal_puts(item->title);
        terminal_puts("  kind=");
        terminal_puts(item->kind);
        if (item->entry[0] != 0) {
            terminal_puts("  entry=");
            terminal_puts(item->entry);
        }
        terminal_puts("\n");
    }
}

void shell_execute(const char *line)
{
    char buffer[SHELL_MAX_TOKENS * SHELL_TOKEN_MAX];
    char *tokens[SHELL_MAX_TOKENS];
    int count;
    bool long_format = false;
    const char *argument;

    if (line[0] == '\0') {
        return;
    }
    command_counter++;
    text_copy(last_command, line, (int)sizeof(last_command));
    if (state.history_count >= SHELL_HISTORY_MAX) {
        for (int index = 0; index < SHELL_HISTORY_MAX - 1; ++index) {
            text_copy(state.history[index], state.history[index + 1],
                      SHELL_TOKEN_MAX * 4);
        }
        state.history_count = SHELL_HISTORY_MAX - 1;
    }
    text_copy(state.history[state.history_count], line,
              SHELL_TOKEN_MAX * 4);
    state.history_count++;

    text_copy(buffer, line, (int)sizeof(buffer));
    count = shell_tokenize(buffer, tokens, SHELL_MAX_TOKENS);
    if (count == 0) {
        return;
    }
    argument = count > 1 ? tokens[1] : "";
    for (int index = 1; index < count; ++index) {
        if (text_equal(tokens[index], "-l") || text_equal(tokens[index], "-la") ||
            text_equal(tokens[index], "-al")) {
            long_format = true;
        }
    }

    if (text_equal(tokens[0], "help") || text_equal(tokens[0], "?")) {
        cmd_help();
    } else if (text_equal(tokens[0], "wadcheck")) {
        cmd_wadcheck(tokens, count);
    } else if (text_equal(tokens[0], "levelinfo")) {
        cmd_levelinfo(tokens, count);
    } else if (text_equal(tokens[0], "wadinfo")) {
        cmd_wadinfo(tokens, count);
    } else if (text_equal(tokens[0], "fatl")) {
        cmd_fatl(tokens, count);
    } else if (text_equal(tokens[0], "files")) {
        cmd_files();
    } else if (text_equal(tokens[0], "ata")) {
        cmd_ata();
    } else if (text_equal(tokens[0], "elf")) {
        cmd_elf(count > 1 ? tokens[1] : 0);
    } else if (text_equal(tokens[0], "usertest")) {
        cmd_usertest();
    } else if (text_equal(tokens[0], "virtio")) {
        cmd_virtio();
    } else if (text_equal(tokens[0], "vblkread")) {
        cmd_vblkread(tokens, count);
    } else if (text_equal(tokens[0], "pci")) {
        cmd_pci(tokens, count);
    } else if (text_equal(tokens[0], "ataread")) {
        cmd_ataread(tokens, count);
    } else if (text_equal(tokens[0], "bench")) {
        uint32_t rounds = 30U;

        if (count > 1) {
            rounds = (uint32_t)shell_atoi_value(tokens[1]);
            if (rounds == 0U || rounds > 600U) {
                rounds = 30U;
            }
        }
        wm_benchmark(rounds);
        terminal_puts("bench: measured ");
        terminal_puts(shell_u32_to_text_scratch(rounds));
        terminal_puts(" full composite frames (cycles in serial log)\n");
    } else if (text_equal(tokens[0], "kbyrun")) {
        cmd_kbyrun(tokens, count);
    } else if (text_equal(tokens[0], "which")) {
        cmd_which(tokens, count);
    } else if (text_equal(tokens[0], "launchers")) {
        cmd_launchers();
    } else if (text_equal(tokens[0], "dumpmem")) {
        cmd_dumpmem(tokens, count);
    } else if (text_equal(tokens[0], "clear")) {
        terminal_clear();
    } else if (text_equal(tokens[0], "echo")) {
        cmd_echo(tokens, count);
    } else if (text_equal(tokens[0], "uname")) {
        cmd_uname(argument);
    } else if (text_equal(tokens[0], "ls") || text_equal(tokens[0], "dir")) {
        const char *target = "";

        for (int index = 1; index < count; ++index) {
            if (tokens[index][0] == '-') {
                long_format = true;
            } else {
                target = tokens[index];
            }
        }
        if (target[0] == '\0') {
            cmd_ls(state.cwd, long_format);
            return;
        }
        {
            char resolved[VFS_PATH_MAX];

            vfs_absolute_of(target, state.cwd, resolved, (int)sizeof(resolved));
            if (!vfs_is_directory_path(resolved)) {
                terminal_puts("ls: ");
                terminal_puts(resolved);
                terminal_puts(": No such file or directory\n");
                terminal_error();
                return;
            }
            cmd_ls(resolved, long_format);
        }
    } else if (text_equal(tokens[0], "cat") || text_equal(tokens[0], "type")) {
        if (argument[0] == '\0') {
            terminal_puts("cat: missing operand\n");
            terminal_error();
            return;
        }
        cmd_cat(argument);
    } else if (text_equal(tokens[0], "touch")) {
        if (count < 2) {
            terminal_puts("touch: missing file operand\n");
            terminal_error();
            return;
        }
        cmd_touch(tokens, count);
    } else if (text_equal(tokens[0], "mkdir")) {
        if (count < 2) {
            terminal_puts("mkdir: missing operand\n");
            terminal_error();
            return;
        }
        cmd_mkdir(tokens, count);
    } else if (text_equal(tokens[0], "rm") || text_equal(tokens[0], "del")) {
        if (count < 2) {
            terminal_puts("rm: missing operand\n");
            terminal_error();
            return;
        }
        cmd_rm(tokens, count);
    } else if (text_equal(tokens[0], "ps")) {
        cmd_ps();
    } else if (text_equal(tokens[0], "uptime")) {
        cmd_uptime();
    } else if (text_equal(tokens[0], "neofetch")) {
        cmd_neofetch();
    } else if (text_equal(tokens[0], "reboot")) {
        terminal_puts("rebooting...\n");
        reboot_requested = true;
    } else if (text_equal(tokens[0], "halt")) {
        terminal_puts("halting...\n");
        halt_requested = true;
    } else if (text_equal(tokens[0], "pwd")) {
        cmd_pwd();
    } else if (text_equal(tokens[0], "cd")) {
        cmd_cd(argument);
    } else if (text_equal(tokens[0], "date")) {
        cmd_date();
    } else if (text_equal(tokens[0], "whoami")) {
        terminal_puts(state.user);
        terminal_puts("\n");
    } else if (text_equal(tokens[0], "hostname")) {
        terminal_puts(state.host);
        terminal_puts("\n");
    } else if (text_equal(tokens[0], "sysinfo")) {
        cmd_sysinfo();
    } else if (text_equal(tokens[0], "df")) {
        cmd_df();
    } else if (text_equal(tokens[0], "free")) {
        cmd_free();
    } else if (text_equal(tokens[0], "apps")) {
        cmd_apps();
    } else if (text_equal(tokens[0], "open") || text_equal(tokens[0], "launch")) {
        if (argument[0] == '\0') {
            terminal_puts("open: missing application name\n");
            terminal_error();
            return;
        }
        shell_run_app(argument);
    } else if (text_equal(tokens[0], "heapcheck")) {
        cmd_heapcheck();
    } else if (text_equal(tokens[0], "luastep")) {
        uint32_t times = 200;

        if (count > 2) {
            times = (uint32_t)strtol(tokens[2], 0, 10);
        }
        cmd_luastep(argument, times);
    } else if (text_equal(tokens[0], "lua")) {
        cmd_lua(argument);
    } else if (text_equal(tokens[0], "kpm")) {
        cmd_kpm(tokens, count);
    } else if (text_equal(tokens[0], "man") || text_equal(tokens[0], "help2")) {
        if (argument[0] == '\0') {
            terminal_puts("man: missing command name\n");
            terminal_error();
            return;
        }
        cmd_man(argument);
    } else if (text_equal(tokens[0], "history")) {
        cmd_history();
    } else if (text_equal(tokens[0], "banner")) {
        cmd_banner();
    } else if (text_equal(tokens[0], "version")) {
        terminal_puts("klye-sh 1.2\n");
    } else {
        terminal_puts("klye-sh: ");
        terminal_puts(tokens[0]);
        terminal_puts(": command not found\n");
        terminal_error();
    }
}

const struct shell_state *shell_get_state(void)
{
    return &state;
}

uint32_t shell_command_count(void)
{
    return command_counter;
}

const char *shell_last_command(void)
{
    return last_command;
}

void shell_init(void)
{
    text_copy(state.cwd, "/home/klye", SHELL_CWD_MAX);
    text_copy(state.user, "klye", (int)sizeof(state.user));
    text_copy(state.host, "klye-virtual", (int)sizeof(state.host));
    state.history_count = 0;
    command_counter = 0;
    reboot_requested = false;
    halt_requested = false;
}

void shell_machine_reboot(void)
{
    reboot_requested = false;
    outb(0x64, 0xFE);
    outb(0xCF9, 0x0E);
    __asm__ volatile("cli; hlt");
}

void shell_machine_halt(void)
{
    halt_requested = false;
    outw(0x604, 0x2000);
    outb(0xCF9, 0x02);
    __asm__ volatile("cli; hlt");
}

bool shell_reboot_pending(void)
{
    return reboot_requested;
}

bool shell_halt_pending(void)
{
    return halt_requested;
}
