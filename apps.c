#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "font.h"
#include "font_ext.h"
#include "gfx.h"
#include "input.h"
#include "kernel.h"
#include "shell.h"
#include "theme.h"
#include "vfs.h"

#define TERMINAL_LOGICAL_COLS 92
#define TERMINAL_LINE_HEIGHT 12
#define TERMINAL_PAD_X 10
#define TERMINAL_PAD_Y 8
#define TERMINAL_HEADER 22
#define TERMINAL_FLAG_PROMPT 0x01U
#define TERMINAL_FLAG_OUTPUT 0x02U
#define TERMINAL_FLAG_ERROR 0x04U
#define TERMINAL_FLAG_COMMAND 0x08U

#define EDITOR_LINE_HEIGHT 12
#define EDITOR_GUTTER 34
#define EDITOR_PAD_Y 8
#define EDITOR_TAB 4
#define EDITOR_STATUS 20
#define CURSOR_BLINK_TICKS 530U


static bool app_open_flags[APP_COUNT];

/* Terminal colours.
 *
 * Sixteen slots, addressed by an escape sequence's parameter, plus a default
 * that everything starts in.  A terminal that can only be one colour per line
 * cannot show a prompt and a path and an error on the same line without
 * picking one, which is why the old one derived its colour from a per-line
 * flag: one colour, decided by what kind of line it was.
 *
 * The palette is the theme's own, so the terminal cannot drift from the rest
 * of the desktop. */
#define TERM_COLOR_DEFAULT 0U
#define TERM_COLOR_COUNT   16U

static const uint32_t terminal_palette[TERM_COLOR_COUNT] = {
    THEME_TERMINAL_TEXT,    /*  0 default          */
    THEME_TERMINAL_DIM,     /*  1 dim / comment    */
    THEME_TERMINAL_PROMPT,  /*  2 prompt / info    */
    THEME_TERMINAL_CURSOR,  /*  3 accent / ok      */
    THEME_GREEN,            /*  4 green            */
    THEME_RED,              /*  5 red / error      */
    THEME_ORANGE,           /*  6 orange / warn    */
    THEME_PURPLE,           /*  7 purple           */
    THEME_TEAL,             /*  8 teal             */
    THEME_INDIGO,           /*  9 indigo           */
    THEME_ACCENT,           /* 10 blue             */
    THEME_ACCENT_DEEP,      /* 11 deep blue        */
    THEME_ACCENT_SOFT,      /* 12 pale blue        */
    THEME_TEXT_ON_DARK,     /* 13 bright           */
    THEME_TEXT_ON_DARK_DIM, /* 14 grey             */
    THEME_TEXT_SECONDARY    /* 15 muted            */
};

/* The colour new text is written in, set by the last escape sequence seen. */
static uint8_t terminal_color_current = TERM_COLOR_DEFAULT;
/* Non zero while part way through an escape sequence, so a sequence split
 * across two writes is not mistaken for text. */
static int terminal_escape_state;
static uint8_t terminal_escape_parameter;

static char term_lines[TERMINAL_MAX_LINES][TERMINAL_MAX_COLS];
/* One byte of colour attribute per character, so a line can be more than one
 * colour.  The attribute is an index into terminal_palette, not a packed RGB:
 * 108 lines of 108 cells is a lot of state to give up, and a palette index is
 * one byte where an RGB triple is four. */
static uint8_t term_attrs[TERMINAL_MAX_LINES][TERMINAL_MAX_COLS];
static uint8_t term_lengths[TERMINAL_MAX_LINES];
static uint8_t term_flags[TERMINAL_MAX_LINES];
static int term_count;
static int term_cursor_line;
static int term_cursor_column;
static int term_scroll_offset;

static char edit_lines[EDITOR_MAX_LINES][EDITOR_MAX_COLS];
static uint8_t edit_lengths[EDITOR_MAX_LINES];
static int edit_count;
static int edit_cursor_line;
static int edit_cursor_column;
static int edit_top_line;
static bool edit_modified;
static bool edit_saved;
static char edit_name[EDITOR_NAME_MAX];

static const char *const app_names[APP_COUNT] = {
    "terminal", "editor", "files", "settings", "browser", "about"
};

static const char *const app_titles[APP_COUNT] = {
    "Terminal", "Text Editor", "Files", "Settings", "Browser", "About Klye"
};

static const uint32_t app_accents[APP_COUNT] = {
    THEME_TEXT_PRIMARY, THEME_TEAL, THEME_ACCENT, THEME_SEPARATOR_STRONG,
    THEME_PURPLE, THEME_ORANGE
};

static const char *const app_symbols[APP_COUNT] = {
    ">_", "Aa", "##", "==", "@@", "k"
};


static bool text_equal(const char *a, const char *b)
{
    int index = 0;

    while (a[index] != '\0' && a[index] == b[index]) {
        ++index;
    }
    return a[index] == b[index];
}












const char *app_name(enum app_id app)
{
    if (app >= APP_COUNT) {
        return "unknown";
    }
    return app_names[app];
}

const char *app_title(enum app_id app)
{
    if (app >= APP_COUNT) {
        return "Unknown";
    }
    return app_titles[app];
}

uint32_t app_accent(enum app_id app)
{
    if (app >= APP_COUNT) {
        return THEME_ACCENT;
    }
    return app_accents[app];
}

bool app_is_open(enum app_id app)
{
    if (app >= APP_COUNT) {
        return false;
    }
    return app_open_flags[app];
}

int app_open_count(void)
{
    int total = 0;

    for (int index = 0; index < APP_COUNT; ++index) {
        if (app_open_flags[index]) {
            total++;
        }
    }
    return total;
}

void app_open(enum app_id app)
{
    if (app < APP_COUNT) {
        app_open_flags[app] = true;
    }
}

void app_close(enum app_id app)
{
    if (app < APP_COUNT) {
        app_open_flags[app] = false;
    }
}

void app_toggle(enum app_id app)
{
    if (app < APP_COUNT) {
        app_open_flags[app] = !app_open_flags[app];
    }
}

void apps_close_all(void)
{
    for (int index = 0; index < APP_COUNT; ++index) {
        app_open_flags[index] = false;
    }
}

int app_from_name(const char *name)
{
    for (int index = 0; index < APP_COUNT; ++index) {
        if (text_equal(name, app_names[index])) {
            return index;
        }
    }
    if (text_equal(name, "term") || text_equal(name, "shell") ||
        text_equal(name, "console")) {
        return APP_TERMINAL;
    }
    if (text_equal(name, "edit") || text_equal(name, "texteditor") ||
        text_equal(name, "notepad")) {
        return APP_EDITOR;
    }
    if (text_equal(name, "finder") || text_equal(name, "explorer")) {
        return APP_FILES;
    }
    return -1;
}

void app_draw_icon(struct gfx_surface *surface, enum app_id app, int x, int y,
                   int size)
{
    uint32_t accent = app_accent(app);
    int radius = size / 4;

    if (size < 8) {
        return;
    }
    /* Flat tile, no drop shadow and no gloss.  The shadow smeared a black
     * halo around every icon and, because the desktop icons fade in, it
     * appeared as a black stain that slowly turned transparent.  The white
     * gloss sat in the middle of the tile as a blob over the symbol.  Both
     * were left over from the old skeuomorphic look. */
    gfx_rounded_rect(surface, x, y, size, size, radius, accent);
    font_draw_centered(surface, x + size / 2, y + size / 2 + 3,
                       app_symbols[app], THEME_TEXT_ON_DARK, size >= 40 ? 2 : 1);
}

static void terminal_shift_up(void)
{
    for (int row = 0; row < TERMINAL_MAX_LINES - 1; ++row) {
        __builtin_memcpy(term_lines[row], term_lines[row + 1],
                         TERMINAL_MAX_COLS);
        term_lengths[row] = term_lengths[row + 1];
        term_flags[row] = term_flags[row + 1];
    }
    term_lines[TERMINAL_MAX_LINES - 1][0] = '\0';
    term_lengths[TERMINAL_MAX_LINES - 1] = 0;
    term_flags[TERMINAL_MAX_LINES - 1] = 0;
    term_count--;
    if (term_cursor_line > 0) {
        term_cursor_line--;
    }
}

static void terminal_new_line(uint8_t flags)
{
    if (term_count >= TERMINAL_MAX_LINES) {
        terminal_shift_up();
    }
    term_lines[term_count][0] = '\0';
    term_lengths[term_count] = 0;
    term_flags[term_count] = flags;
    /* the new line starts in the colour that is current, so a colour set
     * before a newline carries across it rather than resetting on every line */
    term_attrs[term_count][0] = terminal_color_current;
    term_cursor_line = term_count;
    term_cursor_column = 0;
    term_count++;
}

#define TERMINAL_PROMPT_TEXT "klye os    :~$ "
#define TERMINAL_PROMPT_COLUMNS 15

static void terminal_begin_prompt(void)
{
    terminal_new_line(TERMINAL_FLAG_PROMPT);
    terminal_puts(TERMINAL_PROMPT_TEXT);
    term_flags[term_cursor_line] = TERMINAL_FLAG_PROMPT;
}

void terminal_reset(void)
{
    term_count = 0;
    term_cursor_line = 0;
    term_cursor_column = 0;
    term_scroll_offset = 0;
    terminal_begin_prompt();
}

void terminal_clear(void)
{
    term_count = 0;
    term_cursor_line = 0;
    term_cursor_column = 0;
    term_scroll_offset = 0;
    terminal_begin_prompt();
}

/* One character through the terminal, with the control characters handled the
 * way a tty would.  Shared by the string and counted forms. */
/* An SGR escape sequence: ESC [ <params> m
 *
 * Only SGR is handled, and only the colour selectors.  Everything else is
 * consumed and dropped rather than printed, so a program emitting a sequence
 * this does not implement shows nothing rather than showing "^[[1m" on screen.
 * A state machine rather than a scan for the terminating 'm', because the
 * sequence can be split across two writes and a partial one is not a
 * sequence. */
static void terminal_sgr(uint8_t parameter)
{
    if (parameter < TERM_COLOR_COUNT) {
        terminal_color_current = parameter;
    } else if (parameter == 39U) {
        terminal_color_current = TERM_COLOR_DEFAULT;
    }
    /* 0 resets, 1 bright, 22 normal intensity and 2x 30-37 / 90-97 map onto the
     * palette above; the bold and dim variants are folded into their plain
     * colours rather than inventing extra slots for them. */
    if (parameter == 0U) {
        terminal_color_current = TERM_COLOR_DEFAULT;
    }
}

/* Put one character in the cell at the cursor, advancing it.
 *
 * Both the ASCII path and the decoded one come through here so that a cell
 * means the same thing either way: one character, with its colour, and the
 * line's length and terminator updated together. */
static void terminal_put_cell(char character)
{
    if (term_cursor_column >= TERMINAL_LOGICAL_COLS) {
        terminal_new_line(TERMINAL_FLAG_OUTPUT);
    }
    term_lines[term_cursor_line][term_cursor_column] = character;
    term_attrs[term_cursor_line][term_cursor_column] = terminal_color_current;
    term_cursor_column++;
    term_lengths[term_cursor_line] = (uint8_t)term_cursor_column;
    term_lines[term_cursor_line][term_cursor_column] = '\0';
    term_flags[term_cursor_line] |= TERMINAL_FLAG_OUTPUT;
}

/* Write a codepoint that has a glyph in the extended table.
 *
 * The box drawing characters are three bytes each in UTF-8, and the font only
 * covers printable ASCII, so a logo built from them drew nothing at all: not a
 * missing glyph but three bytes, none of which was a character.  A cell holds
 * one byte, so the sequence is decoded here and the cell stores the glyph's
 * index, which the font maps back.  A character with no glyph is dropped, as
 * every other character outside ASCII already was. */
static void terminal_put_extended(const char *text, int *index)
{
    uint32_t codepoint = font_utf8_next(text, index);
    int glyph = font_ext_index(codepoint);

    if (glyph < 0) {
        return;
    }
    terminal_put_cell((char)(0xF0 | (char)glyph));
}

static void terminal_putc_raw(char character)
{
    if (terminal_escape_state != 0) {
        if (character >= '0' && character <= '9') {
            terminal_escape_parameter =
                (uint8_t)(terminal_escape_parameter * 10U +
                          (uint8_t)(character - '0'));
            return;
        }
        if (character == 'm') {
            terminal_sgr(terminal_escape_parameter);
            terminal_escape_state = 0;
            terminal_escape_parameter = 0;
            return;
        }
        if (character == ';') {
            /* a second parameter: apply the first and start again.  Only the
             * first is honoured, which is enough for every sequence used
             * here and is not worth a parameter array. */
            terminal_sgr(terminal_escape_parameter);
            terminal_escape_parameter = 0;
            return;
        }
        if (character == '[') {
            return; /* the introducer after ESC */
        }
        /* anything else ends the sequence, and is dropped */
        terminal_escape_state = 0;
        terminal_escape_parameter = 0;
        return;
    }
    if (character == 0x1B) {
        terminal_escape_state = 1;
        terminal_escape_parameter = 0;
        return;
    }
    if (character == '\n') {
        terminal_new_line(TERMINAL_FLAG_OUTPUT);
        return;
    }
    if (character == '\r') {
        return;
    }
    if (character == '\b') {
        if (term_cursor_column > 0) {
            term_cursor_column--;
            term_lengths[term_cursor_line] = (uint8_t)term_cursor_column;
            term_lines[term_cursor_line][term_cursor_column] = '\0';
        }
        return;
    }
    if (character < ' ' && character != '\t') {
        return;
    }
    if (character > '~') {
        return;
    }
    if (term_cursor_column >= TERMINAL_LOGICAL_COLS) {
        terminal_new_line(TERMINAL_FLAG_OUTPUT);
    }
    term_lines[term_cursor_line][term_cursor_column] = character;
    term_attrs[term_cursor_line][term_cursor_column] = terminal_color_current;
    term_cursor_column++;
    term_lengths[term_cursor_line] = (uint8_t)term_cursor_column;
    term_lines[term_cursor_line][term_cursor_column] = '\0';
    term_flags[term_cursor_line] |= TERMINAL_FLAG_OUTPUT;
}

void terminal_set_color(uint8_t color)
{
    terminal_color_current = color < TERM_COLOR_COUNT ? color : TERM_COLOR_DEFAULT;
}

uint8_t terminal_color(void)
{
    return terminal_color_current;
}

void terminal_write(const char *text, int length)
{
    /* for text that is not NUL terminated, such as a slice out of a file */
    for (int index = 0; index < length; ++index) {
        if ((uint8_t)text[index] >= 0x80U) {
            /* count the whole sequence, so the loop does not also visit its
             * continuation bytes as if they were characters of their own */
            int after = index;
            int before = index;

            (void)font_utf8_next(text, &after);
            terminal_put_extended(text, &index);
            index = after > before ? after - 1 : index;
            continue;
        }
        terminal_putc_raw(text[index]);
    }
    term_scroll_offset = 0;
}

void terminal_puts(const char *text)
{
    for (int index = 0; text[index] != '\0';) {
        if ((uint8_t)text[index] >= 0x80U) {
            terminal_put_extended(text, &index);
            continue;
        }
        terminal_putc_raw(text[index]);
        index++;
    }
    term_scroll_offset = 0;
}

void terminal_error(void)
{
    if (term_count > 0) {
        term_flags[term_count - 1] |= TERMINAL_FLAG_ERROR;
    }
}

void terminal_printf_number(uint64_t value)
{
    char digits[24];
    int length = 0;

    if (value == 0U) {
        terminal_puts("0");
        return;
    }
    while (value != 0U && length < (int)sizeof(digits) - 1) {
        digits[length++] = (char)('0' + (int)(value % 10U));
        value /= 10U;
    }
    while (length > 0) {
        char single[2];

        single[0] = digits[--length];
        single[1] = '\0';
        terminal_puts(single);
    }
}

static void terminal_backspace(void)
{
    if (term_cursor_column > 0) {
        term_cursor_column--;
        term_lines[term_cursor_line][term_cursor_column] = '\0';
        term_lengths[term_cursor_line] = (uint8_t)term_cursor_column;
        return;
    }
    /* At column zero, backspace stops.
     *
     * It used to join the line above, which is what a raw byte stream does and
     * is wrong for this: the lines above the cursor are output.  Joining them
     * meant holding backspace walked backwards through the scrollback eating
     * the results of earlier commands, one whole line at a time, and there was
     * no way to get them back.  A terminal that has already printed a line does
     * not unprint it.
     *
     * The line being typed is the only thing backspace owns. */
}

static void terminal_submit(void)
{
    int length = term_lengths[term_cursor_line];
    int first = 0;
    int type_flags = term_flags[term_cursor_line];
    char command[TERMINAL_MAX_COLS];
    int span;

    if ((type_flags & TERMINAL_FLAG_PROMPT) != 0) {
        first = TERMINAL_PROMPT_COLUMNS;
        if (first > length) {
            first = length;
        }
    }
    span = length - first;
    if (span > TERMINAL_MAX_COLS - 1) {
        span = TERMINAL_MAX_COLS - 1;
    }
    __builtin_memcpy(command, term_lines[term_cursor_line] + first,
                     (size_t)span);
    command[span] = '\0';
    terminal_new_line((uint8_t)(TERMINAL_FLAG_COMMAND | type_flags));
    shell_execute(command);
    terminal_begin_prompt();
}

void terminal_handle_key(const struct key_event *event)
{
    char character;

    if (event->pressed == 0U) {
        return;
    }
    if (event->code == KEY_UP) {
        if (term_cursor_line > 0) {
            term_cursor_line--;
            term_cursor_column = term_lengths[term_cursor_line];
        }
        return;
    }
    if (event->code == KEY_DOWN) {
        if (term_cursor_line + 1 < term_count) {
            term_cursor_line++;
            term_cursor_column = term_lengths[term_cursor_line];
        }
        return;
    }
    if (event->code == KEY_PAGE_UP) {
        /* Scrolling back is bounded by the history, and the bound is the
         * number of lines above the bottom, which is one less than the count.
         * The old code clamped against term_count - 1, which is right in
         * principle, but a terminal holding only a prompt and an output line
         * clamps to a couple of lines and looks broken, and a terminal that
         * had scrolled back already got its offset reset to zero by the very
         * next keypress -- every other handler ends with
         * term_scroll_offset = 0, so a scrollback was dismissed by typing.
         *
         * Any new output snaps back to the bottom, which is what a terminal
         * does, but a key that is not output does not. */
        int limit = term_count > 0 ? term_count - 1 : 0;

        term_scroll_offset += 5;
        if (term_scroll_offset > limit) {
            term_scroll_offset = limit;
        }
        return;
    }
    if (event->code == KEY_PAGE_DOWN) {
        term_scroll_offset -= 5;
        if (term_scroll_offset < 0) {
            term_scroll_offset = 0;
        }
        return;
    }
    if (event->code == KEY_UP && (event->modifiers & MOD_SHIFT) != 0U) {
        int limit = term_count > 0 ? term_count - 1 : 0;

        if (term_scroll_offset < limit) {
            term_scroll_offset++;
        }
        return;
    }
    if (event->code == KEY_DOWN && (event->modifiers & MOD_SHIFT) != 0U) {
        if (term_scroll_offset > 0) {
            term_scroll_offset--;
        }
        return;
    }
    if (event->code == KEY_ESCAPE) {
        /* On an empty prompt, escape wipes the scrollback -- everything above,
         * not just the current line.  With text on the line it clears only the
         * line, so a stray keypress cannot throw away a half typed command.
         *
         * This is the whole way to clear the terminal.  Before, escape cleared
         * the current line and nothing else, and the only way to get rid of the
         * history was to hold backspace, which walked back line by line and
         * stopped dead at column zero of each one. */
        if (term_cursor_column == 0 && term_lengths[term_cursor_line] == 0) {
            terminal_clear();
            return;
        }
        term_cursor_column = 0;
        term_lengths[term_cursor_line] = 0;
        term_lines[term_cursor_line][0] = '\0';
        return;
    }
    if (event->code == 'l' && (event->modifiers & MOD_CTRL) != 0U) {
        /* Ctrl+L: the same thing, on a key that says what it means. */
        terminal_clear();
        return;
    }
    if (event->code == KEY_ENTER) {
        terminal_submit();
        return;
    }
    if (event->code == KEY_BACKSPACE) {
        terminal_backspace();
        return;
    }
    if (event->code == KEY_DELETE) {
        int length = term_lengths[term_cursor_line];

        if (term_cursor_column < length) {
            for (int index = term_cursor_column; index < length - 1; ++index) {
                term_lines[term_cursor_line][index] =
                    term_lines[term_cursor_line][index + 1];
            }
            term_lengths[term_cursor_line] = (uint8_t)(length - 1);
            term_lines[term_cursor_line][term_lengths[term_cursor_line]] = '\0';
        }
        return;
    }
    if (event->code == KEY_LEFT) {
        if (term_cursor_column > 0) {
            term_cursor_column--;
        }
        return;
    }
    if (event->code == KEY_RIGHT) {
        if (term_cursor_column < term_lengths[term_cursor_line]) {
            term_cursor_column++;
        }
        return;
    }
    if (event->code == KEY_HOME) {
        term_cursor_column = 0;
        return;
    }
    if (event->code == KEY_END) {
        term_cursor_column = term_lengths[term_cursor_line];
        return;
    }
    character = input_key_ascii(event);
    if (character == 0) {
        return;
    }
    if (character == '\t') {
        character = ' ';
    }
    if (term_cursor_column >= TERMINAL_LOGICAL_COLS - 1) {
        return;
    }
    term_lines[term_cursor_line][term_cursor_column] = character;
    /* Typed text takes the current colour, so a command can be shown in the
     * prompt colour by setting one before the line is submitted.  Without this
     * the cell keeps whatever the line held when it was created, which for a
     * line reused after a clear is a stale attribute rather than the default. */
    term_attrs[term_cursor_line][term_cursor_column] = terminal_color_current;
    term_cursor_column++;
    term_lengths[term_cursor_line] = (uint8_t)term_cursor_column;
    term_lines[term_cursor_line][term_cursor_column] = '\0';
    /* Typing does not dismiss a scrollback.  It used to: this reset meant that
     * after scrolling back with PageUp, the next character typed threw the
     * view to the bottom, so reading history and then typing a command lost the
     * history.  Only new output snaps back to the bottom, which is what a
     * terminal does. */
}

void terminal_scroll(int lines)
{
    int limit = term_count > 0 ? term_count - 1 : 0;

    /* Positive is back into the history, which is the direction a wheel
     * scrolls: pushing it up moves the view up, towards older lines.  Three
     * lines a notch, so one wheel click is a readable amount rather than one
     * line or half the screen. */
    term_scroll_offset += lines * 3;
    if (term_scroll_offset > limit) {
        term_scroll_offset = limit;
    }
    if (term_scroll_offset < 0) {
        term_scroll_offset = 0;
    }
}

int terminal_scroll_offset(void)
{
    return term_scroll_offset;
}

int terminal_line_count(void)
{
    return term_count;
}

int terminal_total_lines(void)
{
    return term_count;
}

int terminal_cursor_column(void)
{
    return term_cursor_column;
}

int terminal_is_prompt(void)
{
    return (term_flags[term_cursor_line] & TERMINAL_FLAG_PROMPT) != 0;
}

const char *terminal_line_text(int index)
{
    if (index < 0 || index >= term_count) {
        return "";
    }
    return term_lines[index];
}

int terminal_line_length(int index)
{
    if (index < 0 || index >= term_count) {
        return 0;
    }
    return term_lengths[index];
}

bool terminal_line_is_prompt(int index)
{
    if (index < 0 || index >= term_count) {
        return false;
    }
    return (term_flags[index] & (TERMINAL_FLAG_PROMPT |
                                 TERMINAL_FLAG_COMMAND)) != 0;
}

bool terminal_has_output(void)
{
    return term_count > 1;
}

static void terminal_draw_line(struct gfx_surface *surface, int x, int baseline,
                               int line_index, int columns);

void terminal_draw(struct gfx_surface *surface, int x, int y, int width,
                   int height)
{
    int rows;
    int columns;
    int first;
    bool blink;

    gfx_fill(surface, x, y, width, height, THEME_TERMINAL_BG);
    gfx_fill(surface, x, y, width, TERMINAL_HEADER, THEME_TERMINAL_BG_ALT);
    gfx_horizontal_line(surface, x, y + TERMINAL_HEADER - 1, width,
                        PIXEL_RGB(0x33, 0x33, 0x3C));
    app_draw_icon(surface, APP_TERMINAL, x + 8, y + 5, 12);
    font_draw(surface, x + 26, y + 15, "klye-sh",
              THEME_TERMINAL_DIM, 1);
    font_draw_right(surface, x + width - 10, y + 15, "zsh",
                    THEME_TERMINAL_DIM, 1);

    rows = (height - TERMINAL_HEADER - TERMINAL_PAD_Y * 2) /
           TERMINAL_LINE_HEIGHT;
    columns = (width - TERMINAL_PAD_X * 2) / FONT_ADVANCE;
    if (rows < 1) {
        rows = 1;
    }
    if (columns < 8) {
        columns = 8;
    }
    first = term_count - rows - term_scroll_offset;
    if (first < 0) {
        first = 0;
    }
    blink = (pit_ticks() % (CURSOR_BLINK_TICKS * 2U)) < CURSOR_BLINK_TICKS;
    for (int row = 0; row < rows; ++row) {
        int line_index = first + row;
        int baseline = y + TERMINAL_HEADER + TERMINAL_PAD_Y +
                       row * TERMINAL_LINE_HEIGHT + FONT_ASCENT;
        int length;

        if (line_index >= term_count) {
            break;
        }
        length = term_lengths[line_index];
        if (length > columns) {
            length = columns;
        }
        /* The colour of a character is whatever was in effect when it was
         * written, stored per cell.  A per-line colour cannot do this: a
         * prompt and its path and an error on the same line are one colour or
         * the other, and the line is the smallest thing that can be coloured. */
        if (line_index == term_cursor_line) {
            int cursor_x;
            int cursor_y;

            terminal_draw_line(surface, x + TERMINAL_PAD_X, baseline,
                               line_index, columns);
            if (!blink || length >= columns) {
                continue;
            }
            cursor_x = x + TERMINAL_PAD_X + term_cursor_column * FONT_ADVANCE;
            cursor_y = y + TERMINAL_HEADER + TERMINAL_PAD_Y +
                       row * TERMINAL_LINE_HEIGHT;
            gfx_fill(surface, cursor_x, cursor_y, FONT_ADVANCE,
                     TERMINAL_LINE_HEIGHT - 2, THEME_TERMINAL_CURSOR);
            if (term_cursor_column < term_lengths[line_index]) {
                char cell[2];

                cell[0] = term_lines[line_index][term_cursor_column];
                cell[1] = '\0';
                font_draw(surface, cursor_x, baseline, cell,
                          THEME_TERMINAL_BG, 1);
            }
            continue;
        }
        terminal_draw_line(surface, x + TERMINAL_PAD_X, baseline, line_index,
                           columns);
    }
}

/* Draw one line, a run of characters at a time in whatever colour they share.
 *
 * A character at a time is the obvious way and it is slow enough to show: a
 * full screen is over a thousand calls, and the font blitter is not cheap.
 * Consecutive characters with the same attribute almost always come in one
 * run, so the runs are what get drawn. */
static void terminal_draw_line(struct gfx_surface *surface, int x, int baseline,
                               int line_index, int columns)
{
    int length = term_lengths[line_index];

    if (length > columns) {
        length = columns;
    }
    for (int start = 0; start < length;) {
        uint8_t attribute = term_attrs[line_index][start];
        int run = start;

        while (run < length && term_attrs[line_index][run] == attribute) {
            run++;
        }
        {
            char piece[TERMINAL_MAX_COLS];

            for (int index = start; index < run; ++index) {
                piece[index - start] = term_lines[line_index][index];
            }
            piece[run - start] = '\0';
            font_draw(surface, x + start * FONT_ADVANCE, baseline, piece,
                      terminal_palette[attribute % TERM_COLOR_COUNT], 1);
        }
        start = run;
    }
}

void editor_reset(void)
{
    edit_count = 1;
    edit_lengths[0] = 0;
    edit_lines[0][0] = '\0';
    edit_cursor_line = 0;
    edit_cursor_column = 0;
    edit_top_line = 0;
    edit_modified = false;
    edit_saved = false;
    editor_load("untitled.txt", "");
}

void editor_load(const char *name, const char *body)
{
    int line = 0;
    int column = 0;
    int length = 0;

    while (name[length] != '\0' && length < EDITOR_NAME_MAX - 1) {
        length++;
    }
    for (int index = 0; index < length; ++index) {
        edit_name[index] = name[index];
    }
    edit_name[length] = '\0';
    edit_count = 1;
    edit_lengths[0] = 0;
    edit_lines[0][0] = '\0';
    if (body == 0) {
        body = "";
    }
    for (int index = 0; body[index] != '\0'; ++index) {
        char character = body[index];

        if (character == '\n') {
            if (line + 1 >= EDITOR_MAX_LINES) {
                break;
            }
            line++;
            column = 0;
            edit_lengths[line] = 0;
            edit_lines[line][0] = '\0';
            continue;
        }
        if (character == '\r') {
            continue;
        }
        if (column >= EDITOR_MAX_COLS - 1) {
            continue;
        }
        edit_lines[line][column] = character;
        column++;
        edit_lengths[line] = (uint8_t)column;
        edit_lines[line][column] = '\0';
    }
    edit_count = line + 1;
    edit_cursor_line = 0;
    edit_cursor_column = 0;
    edit_top_line = 0;
    edit_modified = false;
    edit_saved = true;
}

int editor_save(void)
{
    char buffer[VFS_BODY_MAX];
    int length = 0;
    int written;

    for (int line = 0; line < edit_count; ++line) {
        for (int column = 0; column < edit_lengths[line]; ++column) {
            if (length >= (int)sizeof(buffer) - 1) {
                break;
            }
            buffer[length++] = edit_lines[line][column];
        }
        if (length < (int)sizeof(buffer) - 1) {
            buffer[length++] = '\n';
        }
    }
    buffer[length] = '\0';
    written = vfs_write(edit_name, buffer, (uint32_t)length);
    if (written < 0) {
        edit_saved = false;
        return -1;
    }
    edit_modified = false;
    edit_saved = true;
    return (int)written;
}

const char *editor_name(void)
{
    return edit_name;
}

int editor_line_count(void)
{
    return edit_count;
}

int editor_cursor_line(void)
{
    return edit_cursor_line + 1;
}

int editor_cursor_column(void)
{
    return edit_cursor_column + 1;
}

bool editor_modified(void)
{
    return edit_modified;
}

bool editor_saved(void)
{
    return edit_saved;
}

int editor_word_count(void)
{
    int words = 0;
    bool in_word = false;

    for (int line = 0; line < edit_count; ++line) {
        for (int column = 0; column < edit_lengths[line]; ++column) {
            char character = edit_lines[line][column];

            if (character == ' ' || character == '\t') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
        in_word = false;
    }
    return words;
}

int editor_char_count(void)
{
    int total = 0;

    for (int line = 0; line < edit_count; ++line) {
        total += edit_lengths[line];
    }
    return total;
}

const char *editor_line_text(int index)
{
    if (index < 0 || index >= edit_count) {
        return "";
    }
    return edit_lines[index];
}

int editor_line_length(int index)
{
    if (index < 0 || index >= edit_count) {
        return 0;
    }
    return edit_lengths[index];
}

static void editor_insert(char character)
{
    int length = edit_lengths[edit_cursor_line];
    int column = edit_cursor_column;

    if (length >= EDITOR_MAX_COLS - 1) {
        return;
    }
    for (int index = length; index > column; --index) {
        edit_lines[edit_cursor_line][index] =
            edit_lines[edit_cursor_line][index - 1];
    }
    edit_lines[edit_cursor_line][column] = character;
    edit_lengths[edit_cursor_line] = (uint8_t)(length + 1);
    edit_lines[edit_cursor_line][length + 1] = '\0';
    edit_cursor_column++;
    edit_modified = true;
    edit_saved = false;
}

static void editor_newline(void)
{
    int length = edit_lengths[edit_cursor_line];
    int column = edit_cursor_column;
    int tail;

    if (edit_count >= EDITOR_MAX_LINES) {
        if (edit_cursor_line == 0) {
            return;
        }
        for (int line = 0; line < edit_count - 1; ++line) {
            __builtin_memcpy(edit_lines[line], edit_lines[line + 1],
                             EDITOR_MAX_COLS);
            edit_lengths[line] = edit_lengths[line + 1];
        }
        edit_count--;
        edit_cursor_line--;
        length = edit_lengths[edit_cursor_line];
        column = edit_cursor_column;
    }
    tail = length - column;
    for (int index = EDITOR_MAX_COLS - 1; index >= 0; --index) {
        if (index < tail) {
            edit_lines[edit_cursor_line + 1][index] =
                edit_lines[edit_cursor_line][column + index];
        }
    }
    for (int index = tail; index < EDITOR_MAX_COLS; ++index) {
        edit_lines[edit_cursor_line + 1][index] = '\0';
    }
    edit_lengths[edit_cursor_line + 1] = (uint8_t)tail;
    edit_lengths[edit_cursor_line] = (uint8_t)column;
    edit_lines[edit_cursor_line][column] = '\0';
    edit_count++;
    edit_cursor_line++;
    edit_cursor_column = 0;
    edit_modified = true;
    edit_saved = false;
}

static void editor_backspace(void)
{
    if (edit_cursor_column > 0) {
        int length = edit_lengths[edit_cursor_line];

        for (int index = edit_cursor_column - 1;
             index < length - 1; ++index) {
            edit_lines[edit_cursor_line][index] =
                edit_lines[edit_cursor_line][index + 1];
        }
        edit_lengths[edit_cursor_line] = (uint8_t)(length - 1);
        edit_lines[edit_cursor_line][edit_lengths[edit_cursor_line]] = '\0';
        edit_cursor_column--;
        edit_modified = true;
        edit_saved = false;
        return;
    }
    if (edit_cursor_line == 0) {
        return;
    }
    {
        int previous_length = edit_lengths[edit_cursor_line - 1];
        int current_length = edit_lengths[edit_cursor_line];

        for (int index = 0; index < current_length; ++index) {
            edit_lines[edit_cursor_line - 1][previous_length + index] =
                edit_lines[edit_cursor_line][index];
        }
        edit_lengths[edit_cursor_line - 1] =
            (uint8_t)(previous_length + current_length);
        edit_lines[edit_cursor_line - 1]
                  [edit_lengths[edit_cursor_line - 1]] = '\0';
        edit_cursor_line--;
        edit_cursor_column = previous_length;
        edit_modified = true;
        edit_saved = false;
    }
}

static void editor_delete(void)
{
    int length = edit_lengths[edit_cursor_line];

    if (edit_cursor_column < length) {
        for (int index = edit_cursor_column; index < length - 1; ++index) {
            edit_lines[edit_cursor_line][index] =
                edit_lines[edit_cursor_line][index + 1];
        }
        edit_lengths[edit_cursor_line] = (uint8_t)(length - 1);
        edit_lines[edit_cursor_line][edit_lengths[edit_cursor_line]] = '\0';
        edit_modified = true;
        edit_saved = false;
        return;
    }
    if (edit_cursor_line + 1 >= edit_count) {
        return;
    }
    {
        int current_length = edit_lengths[edit_cursor_line];
        int next_length = edit_lengths[edit_cursor_line + 1];

        for (int index = 0; index < next_length; ++index) {
            edit_lines[edit_cursor_line][current_length + index] =
                edit_lines[edit_cursor_line + 1][index];
        }
        edit_lengths[edit_cursor_line] =
            (uint8_t)(current_length + next_length);
        edit_lines[edit_cursor_line][edit_lengths[edit_cursor_line]] = '\0';
        for (int line = edit_cursor_line + 1; line < edit_count - 1; ++line) {
            __builtin_memcpy(edit_lines[line], edit_lines[line + 1],
                             EDITOR_MAX_COLS);
            edit_lengths[line] = edit_lengths[line + 1];
        }
        edit_count--;
        edit_modified = true;
        edit_saved = false;
    }
}

static void editor_move_vertical(int delta)
{
    int target = edit_cursor_line + delta;

    if (target < 0) {
        target = 0;
    }
    if (target >= edit_count) {
        target = edit_count - 1;
    }
    edit_cursor_line = target;
    if (edit_cursor_column > edit_lengths[target]) {
        edit_cursor_column = edit_lengths[target];
    }
}

void editor_handle_key(const struct key_event *event)
{
    char character;

    if (event->pressed == 0U) {
        return;
    }
    if (input_key_is_modifier(event->code)) {
        return;
    }
    if ((event->modifiers & MOD_CTRL) != 0U) {
        if (event->code == 's' || event->code == 'S') {
            editor_save();
        }
        if (event->code == KEY_HOME) {
            edit_cursor_line = 0;
            edit_cursor_column = 0;
            edit_top_line = 0;
        }
        return;
    }
    switch (event->code) {
    case KEY_ENTER:
        editor_newline();
        return;
    case KEY_BACKSPACE:
        editor_backspace();
        return;
    case KEY_DELETE:
        editor_delete();
        return;
    case KEY_LEFT:
        if (edit_cursor_column > 0) {
            edit_cursor_column--;
        } else if (edit_cursor_line > 0) {
            edit_cursor_line--;
            edit_cursor_column = edit_lengths[edit_cursor_line];
        }
        return;
    case KEY_RIGHT:
        if (edit_cursor_column < edit_lengths[edit_cursor_line]) {
            edit_cursor_column++;
        } else if (edit_cursor_line + 1 < edit_count) {
            edit_cursor_line++;
            edit_cursor_column = 0;
        }
        return;
    case KEY_UP:
        editor_move_vertical(-1);
        return;
    case KEY_DOWN:
        editor_move_vertical(1);
        return;
    case KEY_HOME:
        edit_cursor_column = 0;
        return;
    case KEY_END:
        edit_cursor_column = edit_lengths[edit_cursor_line];
        return;
    case KEY_PAGE_UP:
        editor_move_vertical(-10);
        return;
    case KEY_PAGE_DOWN:
        editor_move_vertical(10);
        return;
    default:
        break;
    }
    if (event->code == KEY_TAB) {
        for (int index = 0; index < EDITOR_TAB; ++index) {
            editor_insert(' ');
        }
        return;
    }
    character = input_key_ascii(event);
    if (character == 0) {
        return;
    }
    if (character == '\n' || character == '\t' || character == '\b') {
        return;
    }
    editor_insert(character);
}

void editor_draw(struct gfx_surface *surface, int x, int y, int width,
                 int height)
{
    int rows;
    int body_height;
    int text_x;
    int columns;
    bool blink;

    gfx_fill(surface, x, y, width, height, THEME_SURFACE);
    body_height = height - EDITOR_STATUS;
    gfx_fill(surface, x, y, EDITOR_GUTTER, body_height, THEME_SURFACE_SUNKEN);
    gfx_vertical_line(surface, x + EDITOR_GUTTER, y, body_height,
                      THEME_SEPARATOR);
    rows = (body_height - EDITOR_PAD_Y * 2) / EDITOR_LINE_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (edit_cursor_line < edit_top_line) {
        edit_top_line = edit_cursor_line;
    }
    if (edit_cursor_line >= edit_top_line + rows) {
        edit_top_line = edit_cursor_line - rows + 1;
    }
    if (edit_top_line > edit_count - 1) {
        edit_top_line = edit_count - 1;
    }
    if (edit_top_line < 0) {
        edit_top_line = 0;
    }
    text_x = x + EDITOR_GUTTER + 8;
    columns = (width - EDITOR_GUTTER - 16) / FONT_ADVANCE;
    if (columns < 8) {
        columns = 8;
    }
    blink = (pit_ticks() % (CURSOR_BLINK_TICKS * 2U)) < CURSOR_BLINK_TICKS;
    for (int row = 0; row < rows; ++row) {
        int line_index = edit_top_line + row;
        int top = y + EDITOR_PAD_Y + row * EDITOR_LINE_HEIGHT;
        int baseline = top + FONT_ASCENT;
        int length;

        if (line_index >= edit_count) {
            break;
        }
        if (line_index == edit_cursor_line) {
            gfx_fill(surface, x + EDITOR_GUTTER + 1, top,
                     width - EDITOR_GUTTER - 1, EDITOR_LINE_HEIGHT,
                     THEME_ACCENT_SOFT);
        }
        {
            char number[8];
            int index = 0;
            int value = line_index + 1;

            if (value == 0) {
                number[index++] = '0';
            }
            while (value > 0 && index < 7) {
                number[index++] = (char)('0' + value % 10);
                value /= 10;
            }
            number[index] = '\0';
            for (int left = 0, right = index - 1; left < right;
                 ++left, --right) {
                char swap = number[left];

                number[left] = number[right];
                number[right] = swap;
            }
            font_draw_right(surface, x + EDITOR_GUTTER - 6, baseline, number,
                            line_index == edit_cursor_line ? THEME_ACCENT
                                                            : THEME_TEXT_TERTIARY,
                            1);
        }
        length = edit_lengths[line_index];
        if (length > columns) {
            length = columns;
        }
        if (length > 0) {
            font_draw(surface, text_x, baseline, edit_lines[line_index],
                      THEME_TEXT_PRIMARY, 1);
        }
        if (line_index == edit_cursor_line && edit_cursor_column < columns &&
            blink) {
            int cursor_x = text_x + edit_cursor_column * FONT_ADVANCE;

            gfx_fill(surface, cursor_x, top + 1, 2, EDITOR_LINE_HEIGHT - 3,
                     THEME_ACCENT);
        }
    }
    gfx_fill(surface, x, y + body_height, width, EDITOR_STATUS,
             THEME_SURFACE_SUNKEN);
    gfx_horizontal_line(surface, x, y + body_height, width, THEME_SEPARATOR);
    {
        char status[EDITOR_NAME_MAX + 4];
        int offset = 0;
        int index = 0;

        while (edit_name[index] != '\0' && offset < EDITOR_NAME_MAX - 1) {
            status[offset++] = edit_name[index++];
        }
        status[offset++] = edit_modified ? '*' : ' ';
        status[offset++] = ' ';
        status[offset] = '\0';
        font_draw(surface, x + 10, y + body_height + 14, status,
                  THEME_TEXT_SECONDARY, 1);
    }
    {
        char position[40];
        int offset = 0;
        int line_value = edit_cursor_line + 1;
        int column_value = edit_cursor_column + 1;
        char line_digits[8];
        char column_digits[8];
        int line_length = 0;
        int column_length = 0;

        while (line_value > 0) {
            line_digits[line_length++] = (char)('0' + line_value % 10);
            line_value /= 10;
        }
        while (column_value > 0) {
            column_digits[column_length++] = (char)('0' + column_value % 10);
            column_value /= 10;
        }
        while (line_length > 0) {
            position[offset++] = line_digits[--line_length];
        }
        position[offset++] = ':';
        while (column_length > 0) {
            position[offset++] = column_digits[--column_length];
        }
        position[offset++] = ' ';
        position[offset] = '\0';
        font_draw_right(surface, x + width - 10, y + body_height + 14,
                        position, THEME_TEXT_SECONDARY, 1);
    }
}

void apps_init(void)
{
    for (int index = 0; index < APP_COUNT; ++index) {
        app_open_flags[index] = false;
    }
    terminal_reset();
    editor_reset();
}
