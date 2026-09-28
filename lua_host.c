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
#include "doom.h"
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
    /* Registry references to the script's optional callbacks, resolved once
     * at load time.  Looking them up by name every frame meant a hash lookup
     * per callback per frame for a function that never changes. */
    int update_ref;
    int paint_ref;
    struct lua_draw draw[LUA_DRAW_MAX];
    int draw_count;
    /* The previous frame's list, so a script that draws the same thing every
     * frame can be recognised and skipped.  Decoding a picture is per-pixel, so
     * a static window that repaints on every tick costs a full redraw for
     * nothing. */
    struct lua_draw previous[LUA_DRAW_MAX];
    int previous_count;
    bool list_changed;
    int mouse_x;
    int mouse_y;
    bool mouse_down;
    uint32_t keys[LUA_KEYS];
    int key_head;
    int key_tail;
    uint32_t frame;
    /* the client area size, refreshed when the compositor replays this host's
     * draw list.  A script cannot otherwise know how big its window is, and
     * anything that centres or scales needs to. */
    int width;
    int height;
    bool yielded;
    bool failed;
    bool used;
};

static struct lua_host hosts[LUA_HOST_LIMIT];

static void draw_push(struct lua_host *host, uint8_t op, int32_t a, int32_t b,
                      int32_t c, int32_t d, int32_t radius, uint32_t colour,
                      const char *text);
static void copy_out(const char *from, char *to, int max);
static void copy_error(struct lua_host *host, const char *text);

/* ------------------------------------------------------------ doom -------- */
/* Doom's own graphics, exposed to scripts so a title screen or a level view is
 * a few lines of Lua rather than a native app. */

static int doom_l_open(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (doom_open_wad(path) == false) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, doom_error());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int doom_l_lumps(lua_State *L)
{
    (void)L;
    lua_pushinteger(L, doom_wad_lumps());
    return 1;
}

static int doom_l_palettes(lua_State *L)
{
    (void)L;
    lua_pushinteger(L, doom_palette_count());
    return 1;
}

static int doom_l_palette(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 1);

    doom_set_palette(index);
    lua_pushboolean(L, 1);
    return 1;
}

static int doom_l_color(lua_State *L)
{
    uint32_t index = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t color = doom_color(index);

    /* returned as three bytes so a script can build a colour without knowing
     * how the packer is laid out */
    lua_pushinteger(L, (lua_Integer)((color >> 16) & 0xFFU));
    lua_pushinteger(L, (lua_Integer)((color >> 8) & 0xFFU));
    lua_pushinteger(L, (lua_Integer)(color & 0xFFU));
    return 3;
}

static int doom_l_size(lua_State *L)
{
    int width = 0;
    int height = 0;

    if (doom_patch_size(luaL_checkstring(L, 1), &width, &height) == false) {
        lua_pushnil(L);
        lua_pushstring(L, doom_error());
        return 2;
    }
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

static int doom_l_patch(lua_State *L)
{
    const char *lump = luaL_checkstring(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);

    if (doom_draw_patch(lump, gfx_backbuffer(), x, y) == false) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, doom_error());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg doom_functions[] = {
    { "open", doom_l_open },
    { "lumps", doom_l_lumps },
    { "palettes", doom_l_palettes },
    { "palette", doom_l_palette },
    { "color", doom_l_color },
    { "size", doom_l_size },
    { "patch", doom_l_patch },
    { 0, 0 },
};

static void register_doom(lua_State *L)
{
    lua_newtable(L);
    luaL_setfuncs(L, doom_functions, 0);
    lua_setglobal(L, "doom");
}

/* Painting hooks, so a script can queue a patch in the frame's display list
 * instead of drawing into the back buffer behind the compositor's back. */
static int doom_l_draw_patch(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *lump = luaL_checkstring(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);

    if (host == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    draw_push(host, 6U, x, y, 0, 0, 0, 0, lump);
    lua_pushboolean(L, 1);
    return 1;
}

static int doom_l_draw_scaled(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *lump = luaL_checkstring(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int width = (int)luaL_checkinteger(L, 4);

    if (host == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    draw_push(host, 7U, x, y, width, 0, 0, 0, lump);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg doom_paint_functions[] = {
    { "drawpatch", doom_l_draw_patch },
    { "drawscaled", doom_l_draw_scaled },
    { 0, 0 },
};

static void register_doom_paint(lua_State *L, struct lua_host *host)
{
    lua_newtable(L);
    lua_pushlightuserdata(L, host);
    luaL_setfuncs(L, doom_paint_functions, 1);
    lua_setglobal(L, "doompaint");
}

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

/* ctx members are invoked as methods (ctx:f(...)), so argument 1 is the ctx
 * table itself and the real arguments start at index 2.  Every arity check
 * below therefore counts the implicit self. */

/* Drawing coordinates are ordinary numbers.  Requiring an exact integer
 * would force every script to floor() the result of its own arithmetic, which
 * is a pointless trap for an API that ends up truncating to pixels anyway. */
static int32_t ctx_int(lua_State *L, int index)
{
    return (int32_t)luaL_checknumber(L, index);
}

static int host_clear(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 6) {
        return luaL_error(L, "clear expects x, y, w, h, colour");
    }
    draw_push(host, 1, ctx_int(L, 2), ctx_int(L, 3),
              ctx_int(L, 4), ctx_int(L, 5), 0,
              lua_colour(L, 6), 0);
    return 0;
}

static int host_fill(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 6) {
        return luaL_error(L, "fill expects x, y, w, h, colour");
    }
    draw_push(host, 1, ctx_int(L, 2), ctx_int(L, 3),
              ctx_int(L, 4), ctx_int(L, 5), 0,
              lua_colour(L, 6), 0);
    return 0;
}

static int host_rounded(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    int32_t radius = 6;

    if (lua_gettop(L) < 5) {
        return luaL_error(L, "rounded expects x, y, w, h [, radius [, colour]]");
    }
    if (lua_gettop(L) >= 6) {
        radius = ctx_int(L, 6);
    }
    draw_push(host, 2, ctx_int(L, 2), ctx_int(L, 3),
              ctx_int(L, 4), ctx_int(L, 5), radius,
              lua_gettop(L) >= 7 ? lua_colour(L, 7) : 0xFF6FD3FFU, 0);
    return 0;
}

static int host_text(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));
    const char *text;

    if (lua_gettop(L) < 4) {
        return luaL_error(L, "text expects x, y, string [, colour]");
    }
    text = luaL_checkstring(L, 4);
    draw_push(host, 3, ctx_int(L, 2), ctx_int(L, 3), 0, 0, 0,
              lua_gettop(L) >= 5 ? lua_colour(L, 5) : 0xFFE8EAF0U, text);
    return 0;
}

static int host_circle(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 4) {
        return luaL_error(L, "circle expects x, y, radius [, colour]");
    }
    draw_push(host, 4, ctx_int(L, 2), ctx_int(L, 3),
              ctx_int(L, 4), 0, 0,
              lua_gettop(L) >= 5 ? lua_colour(L, 5) : 0xFFE8EAF0U, 0);
    return 0;
}

static int host_line(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    if (lua_gettop(L) < 4) {
        return luaL_error(L, "line expects x, y, width [, colour]");
    }
    draw_push(host, 5, ctx_int(L, 2), ctx_int(L, 3),
              ctx_int(L, 4), 0, 0,
              lua_gettop(L) >= 5 ? lua_colour(L, 5) : 0xFF8B95A8U, 0);
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
static int host_width(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    lua_pushinteger(L, host != 0 ? host->width : 0);
    return 1;
}

static int host_height(lua_State *L)
{
    struct lua_host *host = (struct lua_host *)lua_touserdata(L, lua_upvalueindex(1));

    lua_pushinteger(L, host != 0 ? host->height : 0);
    return 1;
}

static void register_ctx(lua_State *L, struct lua_host *host)
{
    lua_newtable(L);

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_clear, 1);
    lua_setfield(L, -2, "clear");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_fill, 1);
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
    lua_pushcclosure(L, host_width, 1);
    lua_setfield(L, -2, "width");

    lua_pushlightuserdata(L, host);
    lua_pushcclosure(L, host_height, 1);
    lua_setfield(L, -2, "height");

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

    /* Keep it in the registry for passing to paint(ctx), and also publish it
     * as the global `ctx` so a script can use it from update() as well, which
     * is the usual shape for a game style callback pair. */
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "klye.ctx");
    lua_setglobal(L, "ctx");
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

int lua_host_index(const struct lua_host *host)
{
    for (int index = 0; index < LUA_HOST_LIMIT; ++index) {
        if (&hosts[index] == host) {
            return index;
        }
    }
    return -1;
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
    host->update_ref = 0;
    host->paint_ref = 0;
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
    register_doom(L);
    register_doom_paint(L, host);

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
    /* Resolve the optional callbacks once, so the per frame path never
     * hashes.  luaL_ref answers LUA_NOREF or LUA_REFNIL for a missing global,
     * and a raw get of either would index the registry out of bounds, so the
     * result is clamped to a non positive "not present". */
    lua_getglobal(L, "update");
    host->update_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (host->update_ref <= 0) {
        host->update_ref = 0;
    }
    lua_getglobal(L, "paint");
    host->paint_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (host->paint_ref <= 0) {
        host->paint_ref = 0;
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

uint32_t lua_host_frame(const struct lua_host *host)
{
    return host != 0 ? host->frame : 0U;
}

int lua_host_draw_count(const struct lua_host *host)
{
    return host != 0 ? host->draw_count : 0;
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

/* Calls one of the script's optional callbacks.
 *
 * Stack discipline is the whole subtlety here: lua_pcall removes the function
 * and its arguments itself, so the function must not be popped again on the
 * success path.  Getting that wrong underflows the Lua stack by one slot per
 * frame, which quietly corrupts the interpreter's internals instead of
 * failing loudly. */
static bool call_callback(struct lua_host *host, int reference,
                          const char *label, int extra)
{
    lua_State *L = host->state;
    bool ok = true;

    if (reference <= 0) {
        return true;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, reference);
    if (lua_isfunction(L, -1) == false) {
        lua_pop(L, 1);
        return true;
    }
    if (extra != 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, "klye.ctx");
    } else {
        lua_pushnumber(L, 1.0 / 60.0);
    }
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        copy_error(host, lua_tostring(L, -1));
        host->failed = true;
        ok = false;
    }
    /* pcall consumed the function and the argument; on failure it left the
     * error message behind, and either way we are back at the entry depth. */
    if (ok == false) {
        lua_pop(L, 1);
    }
    (void)label;
    return ok;
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
    /* the global ctx carries a live frame counter, so publish the new value */
    lua_getglobal(L, "ctx");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, (lua_Integer)host->frame);
        lua_setfield(L, -2, "frame_count");
    }
    lua_pop(L, 1);

    if (call_callback(host, host->update_ref, "update", 0) == false) {
        return;
    }
    call_callback(host, host->paint_ref, "paint", 1);

    /* A script that draws the same commands as last frame has not changed what
     * is on screen, so the window does not need repainting.  Compared field by
     * field rather than with memcmp, because the struct has padding that
     * memcmp would read. */
    {
        bool same = host->draw_count == host->previous_count;

        for (int index = 0; same && index < host->draw_count; ++index) {
            const struct lua_draw *now = &host->draw[index];
            const struct lua_draw *was = &host->previous[index];

            if (now->op != was->op || now->a != was->a || now->b != was->b ||
                now->c != was->c || now->d != was->d ||
                now->radius != was->radius || now->colour != was->colour) {
                same = false;
            } else {
                for (int at = 0; at < (int)sizeof(now->text); ++at) {
                    if (now->text[at] != was->text[at]) {
                        same = false;
                        break;
                    }
                }
            }
        }
        host->list_changed = !same;
        if (same) {
            return;
        }
        for (int index = 0; index < host->draw_count &&
                            index < LUA_DRAW_MAX; ++index) {
            host->previous[index] = host->draw[index];
        }
        host->previous_count = host->draw_count;
    }
}

bool lua_host_list_changed(const struct lua_host *host)
{
    return host != 0 && host->list_changed;
}

void lua_host_draw(struct lua_host *host, struct gfx_surface *surface, int x,
                   int y, int width, int height)
{
    if (host == 0 || host->used == false) {
        return;
    }
    host->width = width;
    host->height = height;
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
        case 6:
            /* a decoded WAD patch, at native size */
            (void)doom_draw_patch(cmd->text, surface, left, top);
            break;
        case 7:
            /* a decoded WAD patch, scaled to `c` pixels wide */
            (void)doom_draw_picture_scaled(cmd->text, surface, left, top, cmd->c);
            break;
        default:
            break;
        }
    }
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
