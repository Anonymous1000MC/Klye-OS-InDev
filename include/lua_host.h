#ifndef KLYE_LUA_HOST_H
#define KLYE_LUA_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

/* Hosts a Lua 5.4 interpreter inside the kernel and gives scripts a small
 * `ctx` table for painting, input and frame pacing.
 *
 * A script is expected to define:
 *
 *   function update(dt)   optional, called once per frame
 *   function paint(ctx)   optional, called once per frame
 *   return app            optional table, kept for the window title
 *
 * and may call ctx:frame() to yield for the rest of the frame.
 */

#define LUA_HOST_LIMIT 4
#define LUA_ERROR_MAX 256

struct lua_host;

/* Loads source and returns a host, or 0 with a reason in *error. */
struct lua_host *lua_host_load(const char *name, const char *source,
                               int length, char *error, int error_max);
void lua_host_unload(struct lua_host *host);

bool lua_host_loaded(const struct lua_host *host);
uint32_t lua_host_frame(const struct lua_host *host);
int lua_host_draw_count(const struct lua_host *host);
const char *lua_host_title(const struct lua_host *host);
const char *lua_host_error(const struct lua_host *host);

/* Steps the script for one frame and replays its draw list. */
void lua_host_service(struct lua_host *host);

/* True when the last service produced a different display list than the one
 * before it, meaning the window genuinely needs repainting. */
bool lua_host_list_changed(const struct lua_host *host);

/* The bounding box of what the script changed since the last frame, in window
 * coordinates, or false when there is none and the whole window has to be
 * repainted.  This is a bound on where pixels can differ, so it must never be
 * smaller than the commands it came from. */
bool lua_host_damage_rect(const struct lua_host *host, struct gfx_rect *out);
void lua_host_draw(struct lua_host *host, struct gfx_surface *surface, int x,
                   int y, int width, int height);

void lua_host_set_mouse(struct lua_host *host, int x, int y, bool down);
void lua_host_push_key(struct lua_host *host, uint32_t code, bool pressed);

int lua_host_count(void);
struct lua_host *lua_host_at(int index);
/* The reverse of lua_host_at, so a host can be handed to the window manager. */
int lua_host_index(const struct lua_host *host);
struct lua_host *lua_host_find(const char *name);

/* Runs a chunk for its side effects only, used by the `lua` shell command. */
bool lua_host_run_once(const char *name, const char *source, int length,
                       char *error, int error_max);

#endif
