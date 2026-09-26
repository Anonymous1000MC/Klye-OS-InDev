#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "vfs.h"
#include "gfx.h"
#include "input.h"
#include "io.h"
#include "kby.h"
#include "launcher.h"
#include "kernel.h"
#include "scheduler.h"
#include "shell.h"
#include "theme.h"
#include "wm.h"

static int shell_u32_to_text(char *buffer, uint32_t value);
static void shell_printf_u64(uint64_t value);
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

static void cmd_free(void)
{
    terminal_puts("              total        used        free\n");
    terminal_puts("mem:        512M        ");
    terminal_printf_number((uint64_t)gfx_alloc_bytes_used() / (1024U * 1024U) + 18U);
    terminal_puts("M       ");
    terminal_printf_number((uint64_t)gfx_alloc_bytes_used() / (1024U * 1024U));
    terminal_puts("M\n");
    terminal_puts("frames:   ");
    shell_printf_u64((uint64_t)gfx_present_count());
    terminal_puts(" presented, ");
    shell_printf_u64((uint64_t)wm_frame_count());
    terminal_puts(" composed at ");
    shell_printf_u64((uint64_t)wm_fps());
    terminal_puts(" fps\n");
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
    terminal_puts("  ###  #    # #### #  # ######  ####  \n");
    terminal_puts("  #   # #    # #   # # # #   # #   # \n");
    terminal_puts("  ####  #    # #### # # #   # ####  \n");
    terminal_puts("  #   # #    # #   # # #   # #   # \n");
    terminal_puts("  #   #  ####  ####  # ######  #    \n");
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

static int shell_launcher_target(const char *name, char *entry, int entry_max)
{
    int slot = launcher_resolve(name);
    int builtin;

    if (slot >= 0) {
        if (launcher_is_builtin(slot, entry, entry_max)) {
            return app_from_name(entry);
        }
        return -2;
    }
    builtin = app_from_name(name);
    if (builtin >= 0) {
        text_copy(entry, name, entry_max);
        return builtin;
    }
    return -1;
}

bool shell_run_app(const char *name)
{
    char entry[LAUNCHER_ENTRY_MAX];
    int builtin = shell_launcher_target(name, entry, (int)sizeof(entry));

    if (builtin == -2) {
        terminal_puts("open: '");
        terminal_puts(name);
        terminal_puts("': bytecode apps are not wired up yet\n");
        terminal_error();
        return false;
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
