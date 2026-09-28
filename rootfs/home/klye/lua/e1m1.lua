-- e1m1.lua - walk around E1M1.
--
--   kpm build e1m1.lua
--   open e1m1
--
-- Arrow keys or WASD to turn and walk.  The view is flat shaded rather than
-- textured, because the texture layout in DOOM1.WAD is not the vanilla one and
-- has not been worked out yet; the geometry is the real level, decoded from
-- the real segs and sectors, so what is on screen is the actual shape of E1M1.
--
-- Needs DOOM1.WAD at doom/DOOM1.WAD on the disk image.  See tools/mkfatdisk.py.

-- E1M1's player 1 start, which is the first thing of type 1 in THINGS.
local start_x, start_y, start_angle = 1056, -3616, 0

local loaded = false
local problem = nil

local ok, err = doom.open("doom/DOOM1.WAD")
if not ok then
  problem = err or "no wad"
else
  local level_ok, level_err = doom3d.level("E1M1")
  if level_ok then
    doom3d.view(start_x, start_y, start_angle, 0)
    loaded = true
  else
    problem = level_err or "no level"
  end
end

-- A step in map units per frame at full speed.  Doom's player is 56 tall and
-- a corridor is a couple of hundred units across, so a few units a frame walks
-- a room in about a second without tunnelling through a wall.
local speed = 4
local turn = 5

-- update is handed the frame delta, not the drawing context; `ctx` is a
-- global, so input is read from it directly.
function update(dt)
  if not loaded then
    return
  end

  -- Printable keys arrive as their ASCII code, arrows as 128 and up, so the
  -- arrow keys are 132 to 135 rather than anything that looks like a scancode.
  local turn_left = ctx:key_held(132) or ctx:key_held(65)   -- left, A
  local turn_right = ctx:key_held(133) or ctx:key_held(68)  -- right, D
  local forward = ctx:key_held(134) or ctx:key_held(87)     -- up, W
  local back = ctx:key_held(135) or ctx:key_held(83)       -- down, S

  if turn_left then doom3d.turn(-turn) end
  if turn_right then doom3d.turn(turn) end

  -- walk() moves along the way the player faces, so the app needs no angles
  if forward then doom3d.walk(speed) end
  if back then doom3d.walk(-speed) end
end

function paint(ctx)
  local w = ctx:width()
  local h = ctx:height()

  if not loaded then
    ctx:clear(0, 0, w, h, 0x101010)
    ctx:text(8, 16, "e1m1: " .. tostring(problem), 0xff4040)
    ctx:text(8, 32, "put DOOM1.WAD at doom/ on the disk image", 0x808080)
    return
  end

  ctx:clear(0, 0, w, h, 0x203040)
  -- 0x000000 is the void colour: columns whose ray found no geometry at all.
  doom3d.render(0x000000)

  -- A status bar, so the movement and the view are both visible at once.
  ctx:fill(0, h - 14, w, 14, 0x202020)
  local x, y, angle, floor = doom3d.where()
  ctx:text(5, h - 4, "E1M1  x=" .. x .. "  y=" .. y ..
           "  ang=" .. angle .. "  z=" .. floor, 0xffffff)
end
