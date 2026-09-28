-- title.lua - show a real WAD picture, decoded with Doom's own patch format
-- and Doom's own palette.
--
--   kpm build title.lua
--   open title
--
-- The WAD is read straight off the disk image, so this wants a DOOM1.WAD at
-- doom/DOOM1.WAD on the image.  See tools/mkdisk.py.

local wad = "doom/DOOM1.WAD"
local lump = "TITLEPIC"

local ready = false
local problem = nil

local ok, err = doom.open(wad)
if ok then
  ready = true
else
  problem = err or "no wad"
end

function paint(ctx)
  local w = ctx:width()
  local h = ctx:height()

  ctx:clear(0, 0, w, h, 0x000000)

  if not ready then
    ctx:text(8, 16, "doom: " .. tostring(problem), 0xff4040)
    ctx:text(8, 32, "put DOOM1.WAD at doom/ on the disk image", 0x808080)
    return
  end

  local pw, ph = doom.size(lump)
  if pw == nil then
    ctx:text(8, 16, "doom: " .. tostring(ph), 0xff4040)
    return
  end

  -- scale by a whole number so the pixels stay square, then centre
  local factor = math.floor(math.min(w / pw, h / ph))
  if factor < 1 then factor = 1 end
  local dw = pw * factor
  local dh = ph * factor
  local x = math.floor((w - dw) / 2)
  local y = math.floor((h - dh) / 2)

  doompaint.drawscaled(lump, x, y, dw)

  -- caption bar, using a colour straight out of Doom's palette
  local r, g, b = doom.color(4)
  ctx:fill(0, h - 14, w, 14, (r << 16) + (g << 8) + b)
  ctx:text(5, h - 4, "DOOM1.WAD  " .. doom.lumps() .. " lumps  " ..
           factor .. "x", 0xffffff)
end
