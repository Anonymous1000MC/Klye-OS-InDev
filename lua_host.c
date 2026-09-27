/* lua_host.c - a Lua 5.4 interpreter running inside the kernel.
 *
 * Three jobs:
 *   1. give lua_State memory from the frame heap,
 *   2. open the subset of the standard library that makes sense without stdio
 *      (base, string, table, math, utf8) and nothing else,
 *   3. expose a `ctx` table so a script can paint, read input and yield.
 *
 * Scripts run in ring 0 for now.  There is no isolation, so a mistake in a
 * script can take the kernel with it; pcall catches Lua level errors, which
 * covers the realistic cases.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "font.h"
#include "gfx.h"
#include "heap.h"
#include "lua_host.h"
#include "theme.h"

extern void serial_write(const char *text);

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LUA_DRAW_MAX 96
#define LUA_KEYS 16

struct lua_draw {
    uint8_t op;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    int32_t radius;
    uint32_t colour;
    char text[48];
};

struct lua_host {
    char name[48];
    char title[48];
    char error[LUA_ERROR_MAX];
    lua_State *state;
    struct lua_draw draw[LUA_DRAW_MAX];
    int draw_count;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    uint32_t keys[LUA_KEYS];
    int key_head;
    int key_tail;
    uint32_t frame;
    bool yielded;
    bool failed;
    bool used;
};

static struct lua_host hosts[LUA_HOST_LIMIT];

static void copy_out(const char *from, char *to, int max);
static void copy_error(struct lua_host *host, const char *text);

/* ------------------------------------------------------------ allocator ---- */

static void *host_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0U) {
        heap_free(ptr);
        return 0;
    }
    if (ptr == 0) {
        return heap_malloc(nsize);
    }
    return heap_realloc(ptr, nsize);
}

/* -------------------------------------------------------------- drawing ---- */

static void draw_push(struct lua_host *host, uint8_t op, int32_t a, int32_t b,
                      int32_t c, int32_t d, int32_t radius, uint32_t colour,
                      const char *text)
{
    struct lua_draw *cmd;

    if (host->draw_count >= LUA_DRAW_MAX) {
        return;
    }
    cmd = &host->draw[host->draw_count++];
    cmd->op = op;
    cmd->a = a;
    cmd->b = b;
    cmd->c = c;
    cmd->d = d;
    cmd->radius = radius;
    cmd->colour = colour;
    if (text != 0) {
        int at = 0;

        while (text[at] != 0 && at < (int)sizeof(cmd->text) - 1) {
            cmd->text[at] = text[at];
            ++at;
        }
        cmd->text[at] = 0;
    } else {
        cmd->text[0] = 0;
    }
}

/* Colours are plain 0xRRGGBB numbers, so a script can write 0x232B3D the same
 * way it would in C.  Out of range values fall back to a visible magenta
 * rather than being silently clamped. */
static uint32_t lua_colour(lua_State *L, int index)
{
    lua_Number value = luaL_checknumber(L, index);
    long packed;

    if (value != value || value < 0.0 || value > 0xFFFFFF) {
        return PIXEL_RGB(0xFF, 0x00, 0xFF);
    }
    packed = (long)value;
    return (uint32_t)(0xFF000000U | ((uint32_t)packed & 0x00FFFFFFU));
}

static int host_rect(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 4) {
        return luaL_error(L, "rect expects x, y, w, h");
    }
    draw_push(host, 1, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2),
              (int32_t)luaL_checkinteger(L, 3),
              (int32_t)luaL_checkinteger(L, 4), 0, 0xFF232B3DU, 0);
    return 0;
}

static int host_colour_rect(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 5) {
        return luaL_error(L, "fill expects x, y, w, h, colour");
    }
    draw_push(host, 1, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2),
              (int32_t)luaL_checkinteger(L, 3),
              (int32_t)luaL_checkinteger(L, 4), 0, lua_colour(L, 5), 0);
    return 0;
}

static int host_rounded(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    int32_t radius = 6;

    if (lua_gettop(L) < 4) {
        return luaL_error(L, "rounded expects x, y, w, h [, radius]");
    }
    if (lua_gettop(L) >= 5) {
        radius = (int32_t)luaL_checkinteger(L, 5);
    }
    draw_push(host, 2, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2),
              (int32_t)luaL_checkinteger(L, 3),
              (int32_t)luaL_checkinteger(L, 4), radius,
              lua_gettop(L) >= 6 ? lua_colour(L, 6) : 0xFF6FD3FFU, 0);
    return 0;
}

static int host_text(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *text = luaL_checkstring(L, 3);

    if (lua_gettop(L) < 3) {
        return luaL_error(L, "text expects x, y, string [, colour]");
    }
    draw_push(host, 3, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2), 0, 0, 0,
              lua_gettop(L) >= 4 ? lua_colour(L, 4) : 0xFFE8EAF0U, text);
    return 0;
}

static int host_circle(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    int32_t radius;

    if (lua_gettop(L) < 3) {
        return luaL_error(L, "circle expects x, y, radius [, colour]");
    }
    radius = (int32_t)luaL_checkinteger(L, 3);
    draw_push(host, 4, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2), radius, 0, 0,
              lua_gettop(L) >= 4 ? lua_colour(L, 4) : 0xFFE8EAF0U, 0);
    return 0;
}

static int host_line(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 3) {
        return luaL_error(L, "line expects x, y, width [, colour]");
    }
    draw_push(host, 5, (int32_t)luaL_checkinteger(L, 1),
              (int32_t)luaL_checkinteger(L, 2),
              (int32_t)luaL_checkinteger(L, 3), 0, 0,
              lua_gettop(L) >= 4 ? lua_colour(L, 4) : 0xFF8B95A8U, 0);
    return 0;
}

static int host_mouse_x(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    lua_pushinteger(L, host->mouse_x);
    return 1;
}

static int host_mouse_y(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    lua_pushinteger(L, host->mouse_y);
    return 1;
}

static int host_mouse_down(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    lua_pushboolean(L, host->mouse_down);
    return 1;
}

static int host_key(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (host->key_head == host->key_tail) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, (lua_Integer)host->keys[host->key_head]);
        host->key_head = (host->key_head + 1) % LUA_KEYS;
    }
    return 1;
}

static int host_frame(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    host->yielded = true;
    (void)L;
    return 0;
}

/* Lua's own print writes to stdout, which this kernel stubs out, so the
 * global is replaced with one that reaches the terminal and the serial log. */
static int host_print(lua_State *L)
{
    int count = lua_gettop(L);

    for (int index = 1; index <= count; ++index) {
        /* Lua separates print arguments with a tab */
        if (index > 1) {
            terminal_puts("\t");
            serial_write("\t");
        }
        if (lua_isstring(L, index)) {
            const char *text = lua_tostring(L, index);

            if (text != 0) {
                terminal_puts(text);
                serial_write(text);
            }
        }
    }
    terminal_puts("\n");
    serial_write("\n");
    return 0;
}

/* Builds the ctx table for one host and stores it in the registry. */
static void register_ctx(lua_State *L, struct lua_host *host)
{
    lua_newtable(L);

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_rect, 1);
    lua_setfield(L, -2, "clear");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_colour_rect, 1);
    lua_setfield(L, -2, "fill");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_rounded, 1);
    lua_setfield(L, -2, "rounded");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_text, 1);
    lua_setfield(L, -2, "text");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_circle, 1);
    lua_setfield(L, -2, "circle");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_line, 1);
    lua_setfield(L, -2, "line");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_mouse_x, 1);
    lua_setfield(L, -2, "mouse_x");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_mouse_y, 1);
    lua_setfield(L, -2, "mouse_y");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_mouse_down, 1);
    lua_setfield(L, -2, "mouse_down");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_key, 1);
    lua_setfield(L, -2, "key");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_frame, 1);
    lua_setfield(L, -2, "frame");

    lua_pushinteger(L, (lua_Integer)host->frame);
    lua_setfield(L, -2, "frame_count");

    lua_pushstring(L, "klye-lua");
    lua_setfield(L, -2, "runtime");

    lua_setfield(L, LUA_REGISTRYINDEX, "klye.ctx");
}

/* --------------------------------------------------------------- errors ---- */

static void copy_out(const char *from, char *to, int max)
{
    int at = 0;

    if (to == 0 || max <= 0) {
        return;
    }
    while (at < max - 1 && from != 0 && from[at] != 0) {
        to[at] = from[at];
        ++at;
    }
    to[at] = 0;
}

static void copy_error(struct lua_host *host, const char *text)
{
    copy_out(text, host->error, LUA_ERROR_MAX);
}

/* --------------------------------------------------------------- hosts ---- */

int lua_host_count(void)
{
    return LUA_HOST_LIMIT;
}

struct lua_host *lua_host_at(int index)
{
    if (index < 0 || index >= LUA_HOST_LIMIT || hosts[index].used == false) {
        return 0;
    }
    return &hosts[index];
}

struct lua_host *lua_host_find(const char *name)
{
    for (int index = 0; index < LUA_HOST_LIMIT; ++index) {
        if (hosts[index].used != 0 && hosts[index].name[0] != 0) {
            bool same = true;

            for (int at = 0; at < 48; ++at) {
                if (hosts[index].name[at] != name[at]) {
                    same = false;
                    break;
                }
                if (name[at] == 0) {
                    break;
                }
            }
            if (same) {
                return &hosts[index];
            }
        }
    }
    return 0;
}

static struct lua_host *host_allocate(const char *name)
{
    for (int index = 0; index < LUA_HOST_LIMIT; ++index) {
        if (hosts[index].used == false) {
            __builtin_memset(&hosts[index], 0, sizeof(hosts[index]));
            hosts[index].used = true;
            for (int at = 0; at < 47 && name[at] != 0; ++at) {
                hosts[index].name[at] = name[at];
            }
            hosts[index].name[47] = 0;
            return &hosts[index];
        }
    }
    return 0;
}

static bool host_open_libs(lua_State *L)
{
    static const luaL_Reg libs[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_IOLIBNAME, 0 },
        { LUA_OSLIBNAME, 0 },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_DBLIBNAME, 0 },
        { LUA_LOADLIBNAME, 0 },
    };

    for (int index = 0; index < (int)(sizeof(libs) / sizeof(libs[0])); ++index) {
        if (libs[index].func == 0) {
            continue;
        }
        luaL_requiref(L, libs[index].name, libs[index].func, 1);
        lua_pop(L, 1);
    }
    return true;
}

struct lua_host *lua_host_load(const char *name, const char *source, int length,
                              char *error, int error_max)
{
    struct lua_host *host = host_allocate(name);
    lua_State *L;
    int base;

    if (host == 0) {
        if (error != 0 && error_max > 0) {
            error[0] = 0;
        }
        return 0;
    }
    host->error[0] = 0;
    L = lua_newstate(host_alloc, 0);
    if (L == 0) {
        host->used = false;
        if (error != 0 && error_max > 0) {
            copy_out("out of memory", error, error_max);
        }
        return 0;
    }
    host->state = L;
    host_open_libs(L);
    lua_pushcfunction(L, host_print);
    lua_setglobal(L, "print");
    register_ctx(L, host);

    (void)base;
    if (luaL_loadbuffer(L, source, (size_t)length, name) != LUA_OK) {
        copy_error(host, lua_tostring(L, -1));
        host->failed = true;
        lua_close(L);
        host->state = 0;
        if (error != 0 && error_max > 0) {
            copy_out(host->error, error, error_max);
        }
        return 0;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        copy_error(host, lua_tostring(L, -1));
        host->failed = true;
        if (error != 0 && error_max > 0) {
            copy_out(host->error, error, error_max);
        }
        return host;
    }
    /* An optional returned table may carry a title. */
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "title");
        if (lua_isstring(L, -1)) {
            const char *text = lua_tostring(L, -1);
            int at = 0;

            while (at < 47 && text[at] != 0) {
                host->title[at] = text[at];
                ++at;
            }
            host->title[at] = 0;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (host->title[0] == 0) {
        for (int at = 0; at < 47 && name[at] != 0; ++at) {
            host->title[at] = name[at];
        }
        host->title[47] = 0;
    }
    return host;
}

void lua_host_unload(struct lua_host *host)
{
    if (host == 0 || host->used == false) {
        return;
    }
    if (host->state != 0) {
        lua_close(host->state);
    }
    __builtin_memset(host, 0, sizeof(*host));
}

bool lua_host_loaded(const struct lua_host *host)
{
    return host != 0 && host->used != 0 && host->failed == false;
}

const char *lua_host_title(const struct lua_host *host)
{
    if (host == 0 || host->title[0] == 0) {
        return "Lua";
    }
    return host->title;
}

const char *lua_host_error(const struct lua_host *host)
{
    if (host == 0 || host->error[0] == 0) {
        return 0;
    }
    return host->error;
}

void lua_host_set_mouse(struct lua_host *host, int x, int y, bool down)
{
    if (host == 0) {
        return;
    }
    host->mouse_x = x;
    host->mouse_y = y;
    host->mouse_down = down;
}

void lua_host_push_key(struct lua_host *host, uint32_t code, bool pressed)
{
    int next;

    if (host == 0 || pressed == false) {
        return;
    }
    next = (host->key_tail + 1) % LUA_KEYS;
    if (next == host->key_head) {
        return;
    }
    host->keys[host->key_tail] = code;
    host->key_tail = next;
}

void lua_host_service(struct lua_host *host)
{
    lua_State *L;

    if (host == 0 || host->used == false || host->failed != 0) {
        return;
    }
    L = host->state;
    host->draw_count = 0;
    host->yielded = false;
    ++host->frame;

    /* update(dt) first, then paint(ctx), both optional. */
    lua_getglobal(L, "update");
    if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, 1.0 / 60.0);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            copy_error(host, lua_tostring(L, -1));
            host->failed = true;
            lua_pop(L, 1);
            return;
        }
    }
    lua_pop(L, 1);

    lua_getglobal(L, "paint");
    if (lua_isfunction(L, -1)) {
        lua_getfield(L, LUA_REGISTRYINDEX, "klye.ctx");
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            copy_error(host, lua_tostring(L, -1));
            host->failed = true;
            lua_pop(L, 1);
            return;
        }
    }
    lua_pop(L, 1);
}

void lua_host_draw(struct lua_host *host, struct gfx_surface *surface, int x,
                   int y, int width, int height)
{
    if (host == 0 || host->used == false) {
        return;
    }
    for (int index = 0; index < host->draw_count; ++index) {
        const struct lua_draw *cmd = &host->draw[index];
        int left = x + cmd->a;
        int top = y + cmd->b;

        switch (cmd->op) {
        case 1:
            gfx_fill(surface, left, top, cmd->c, cmd->d, cmd->colour);
            break;
        case 2:
            gfx_rounded_rect(surface, left, top, cmd->c, cmd->d,
                             cmd->radius > 0 ? cmd->radius : 6, cmd->colour);
            break;
        case 3:
            font_draw(surface, left, top, cmd->text, cmd->colour, 1);
            break;
        case 4:
            gfx_circle(surface, left, top, cmd->c, cmd->colour);
            break;
        case 5:
            gfx_horizontal_line(surface, left, top, cmd->c, cmd->colour);
            break;
        default:
            break;
        }
    }
    (void)width;
    (void)height;
}

bool lua_host_run_once(const char *name, const char *source, int length,
                       char *error, int error_max)
{
    struct lua_host *host = lua_host_load(name, source, length, error, error_max);
    bool ok = host != 0 && host->failed == false;

    if (host != 0) {
        lua_host_unload(host);
    }
    return ok;
}
