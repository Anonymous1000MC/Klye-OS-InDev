-- clock.lua - a windowed Lua app, written the way anyone would expect
--
-- paint(ctx) runs once per frame with the drawing API.  The host also
-- publishes it as the global `ctx`, so update() can read input from it.
--
--   function update(dt)   optional, called once per frame
--   function paint(ctx)   optional, called once per frame
--   return {title = "..."}  optional

local BG    = 0x232B3D
local CHIP  = 0x6FD3FF
local INK   = 0x10202C
local TEXT  = 0xE8EAF0
local MUTED = 0x8B95A8

local state = {
  frames = 0,
  presses = 0,
  mx = 0,
  my = 0,
  held = false,
}

function update(dt)
  state.frames = state.frames + 1
  state.mx = ctx:mouse_x()
  state.my = ctx:mouse_y()
  state.held = ctx:mouse_down()
  if ctx:key() then
    state.presses = state.presses + 1
  end
end

function paint(c)
  c:clear(0, 0, 520, 380, BG)

  -- header chip
  c:rounded(24, 20, 150, 34, 6, CHIP)
  c:text(44, 42, "lua app", INK)

  c:text(24, 90, "frames", MUTED)
  c:text(24, 112, tostring(state.frames), TEXT)

  -- a bar that grows with the frame count, wrapping at 400
  c:fill(24, 136, state.frames % 400, 10, CHIP)
  c:line(24, 161, 400, MUTED)

  c:text(24, 196, "mouse", MUTED)
  c:text(24, 218, state.mx .. ", " .. state.my, TEXT)
  c:text(24, 244, "key presses: " .. state.presses, TEXT)

  if state.held then
    c:text(24, 330, "button down", CHIP)
  else
    c:text(24, 330, "click me", MUTED)
  end
end

return { title = "Clock" }
