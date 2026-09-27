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
const char *lua_host_title(const struct lua_host *host);
const char *lua_host_error(const struct lua_host *host);

/* Steps the script for one frame and replays its draw list. */
void lua_host_service(struct lua_host *host);
void lua_host_draw(struct lua_host *host, struct gfx_surface *surface, int x,
                   int y, int width, int height);

void lua_host_set_mouse(struct lua_host *host, int x, int y, bool down);
void lua_host_push_key(struct lua_host *host, uint32_t code, bool pressed);

int lua_host_count(void);
struct lua_host *lua_host_at(int index);
struct lua_host *lua_host_find(const char *name);

/* Runs a chunk for its side effects only, used by the `lua` shell command. */
bool lua_host_run_once(const char *name, const char *source, int length,
                       char *error, int error_max);

#endif
