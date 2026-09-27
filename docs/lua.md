# Writing Klye apps in Lua

Klye OS hosts Lua 5.4.7 in the kernel. You write ordinary Lua; the system
gives it a window, a drawing API, input, and a filesystem.

Two sample apps ship in the image and can be opened from the dock: **Clock**
and **Bounce**. Their sources are in `rootfs/home/klye/lua/`.

## The shape of an app

An app is a chunk that may define two functions and optionally return a table:

```lua
function update(dt)      -- optional, called once per frame
end

function paint(c)        -- optional, called once per frame
end

return { title = "Clock" }
```

`update` runs first, then `paint`. `dt` is `1/60` — a constant, not a measured
delta, because the compositor is fixed rate.

Anything the chunk defines at the top level stays alive for as long as the
window is open: locals become upvalues captured by the functions, so state
persists across frames. A `local t = {}` above `update` is exactly the state
you want.

If the window is closed, the `lua_State` is closed and everything the script
allocated is freed.

## The drawing API

`paint(c)` receives the drawing table, and the same table is also available as
the global `ctx` so `update` can use it. Colours are plain `0xRRGGBB` numbers.

| call | what it draws |
|---|---|
| `c:clear(x, y, w, h, colour)` | a filled rectangle |
| `c:fill(x, y, w, h, colour)` | a filled rectangle (same as clear) |
| `c:rounded(x, y, w, h [, radius [, colour]])` | rounded rectangle, radius defaults to 6 |
| `c:circle(x, y, radius [, colour])` | filled circle |
| `c:line(x, y, width [, colour])` | a horizontal line |
| `c:text(x, y, string [, colour])` | text, drawn on the baseline at `y` |

Coordinates are **window-relative, in pixels**, with the origin at the top left
of the client area — the title bar is not included. They are plain numbers, not
integers, so you can pass the result of arithmetic without flooring it first.

Note that `x`/`y` for `text` is the **baseline**, not the top of the glyphs.
Text drawn at `y = 40` occupies roughly `y = 31..40`.

There is no alpha and no clipping. Colours are opaque, which keeps the
compositor cheap.

## Input

| call | returns |
|---|---|
| `ctx:mouse_x()` | pointer x, window relative |
| `ctx:mouse_y()` | pointer y, window relative |
| `ctx:mouse_down()` | `true` while the left button is held |
| `ctx:key()` | a key code, or `nil` if nothing is queued |
| `ctx:frame()` | the frame counter, as an integer |
| `ctx.runtime` | the string `"klye-lua"` |

`ctx:key()` returns the raw key code from `include/input.h`; it does not return
a character. There is no printable-character helper yet, so a text field would
need a key-code-to-character table. If you want one, that is a good first
contribution.

Keys are queued while the window has focus and the queue holds 16, so a script
that polls once per frame will not drop ordinary typing but can drop a fast
burst.

## Pacing

The compositor steps every Lua host once per composited frame, not once per
service call, so an animating script does not slow the whole desktop down.

There is no frame limiter inside the VM: a script that does unbounded work in
`paint` will simply make the desktop stutter. Keep the work per frame small.

## Installed standard library

The base, `string`, `table`, `math` and `utf8` libraries are all available,
including `string.format`, closures, metatables, `pcall` and the garbage
collector.

Deliberately **not** available: `io`, `os`, `package`, `require`, `debug` and
coroutines. Those need stdio, `dlopen`, signals and their own stacks, none of
which a kernel has. If you need to read a file, that is the next thing to add.

Numbers are doubles. There is no integer division operator; use `math.floor`.

## Running a script

Three ways:

```bash
lua hello.lua            # run for its output, print() goes to the terminal
luastep lua/clock.lua 300   # step it 300 times and report, no window
open clock               # run it in a window
```

`print` is replaced with one that writes to the terminal and the serial log,
since the standard one targets a stdout this kernel does not have.

## Installing an app

A Lua app is a launcher record plus the script. `kpm build` writes both into
`/bin`, after which it shows up in the dock:

```bash
kpm build myapp.lua      # copies /home/klye/myapp.lua to /bin + a record
kpm list
open myapp
kpm remove myapp
```

The record it writes is:

```
title=myapp
kind=lua
entry=myapp.lua
icon=-1
```

`icon` is a native app id, or `-1` for none — in which case the dock draws the
app's initial letter on a grey tile.

## Things that will bite you

**A script error stops the app, it does not restart it.** The error is shown
in the terminal when you `open` it, and the window keeps its last good frame.
There is no hot reload; fix the file and reopen.

**Scripts run in ring 0.** There is no memory isolation. A bug in your script
can take the kernel down. `pcall` catches ordinary Lua errors, which covers
most mistakes, but not a wild pointer or unbounded recursion in a C function.

**Installed apps do not survive a reboot.** The filesystem is in RAM and is
rebuilt from `rootfs/` on every boot. Persistence needs a block device, which
is not written yet.

**The filesystem is small**: 128 nodes and 12 KiB per file. A `.lua` source
file is fine; anything larger will not fit.

## A worked example

`rootfs/home/klye/lua/clock.lua`, annotated:

```lua
local state = { frames = 0, presses = 0, mx = 0, my = 0, held = false }

function update(dt)
  state.frames = state.frames + 1
  state.mx = ctx:mouse_x()        -- ctx is a global
  state.my = ctx:mouse_y()
  state.held = ctx:mouse_down()
  if ctx:key() then
    state.presses = state.presses + 1
  end
end

function paint(c)
  c:clear(0, 0, 520, 380, 0x232B3D)     -- fill the whole client area
  c:rounded(24, 20, 150, 34, 6, 0x6FD3FF)
  c:text(44, 42, "lua app", 0x10202C)    -- baseline at y=42
  c:text(24, 112, tostring(state.frames), 0xE8EAF0)

  -- a bar that grows with the frame count, wrapping at 400
  c:fill(24, 136, state.frames % 400, 10, 0x6FD3FF)
  c:line(24, 161, 400, 0x8B95A8)

  if state.held then
    c:text(24, 330, "button down", 0x6FD3FF)
  else
    c:text(24, 330, "click me", 0x8B95A8)
  end
end

return { title = "Clock" }
```

`state` is a local at chunk scope, so `update` and `paint` both close over it
and it survives between frames.
