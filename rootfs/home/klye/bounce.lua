-- bounce.lua - a second sample, showing animation from update()
--
-- Demonstrates state kept between frames, a moving circle and a trail.

local BG    = 0x1B2436
local BALL  = 0xFF7A5C
local TRAIL = 0x2E3A52
local TEXT  = 0xE8EAF0
local MUTED = 0x8B95A8

local W, H = 520, 380
local ball = { x = 60, y = 90, vx = 2.2, vy = 1.7, r = 14 }
local trail = {}

function update(dt)
  ball.x = ball.x + ball.vx
  ball.y = ball.y + ball.vy
  if ball.x - ball.r < 0 or ball.x + ball.r > W then ball.vx = -ball.vx end
  if ball.y - ball.r < 70 or ball.y + ball.r > H - 20 then ball.vy = -ball.vy end

  table.insert(trail, 1, { x = ball.x, y = ball.y })
  if #trail > 14 then table.remove(trail) end

  if ctx:key() then
    ball.vx = ball.vx * 1.1
    ball.vy = ball.vy * 1.1
  end
end

function paint(c)
  c:clear(0, 0, W, H, BG)

  c:text(24, 34, "bounce", TEXT)
  c:text(24, 54, "space speeds it up", MUTED)

  -- walls
  c:line(0, 69, W, TRAIL)
  c:line(0, H - 20, W, TRAIL)

  for i, dot in ipairs(trail) do
    local fade = (#trail - i + 1) / (#trail + 1)
    local r = dot.r or math.floor(ball.r * fade)
    c:circle(dot.x, dot.y, r, TRAIL)
  end

  c:circle(ball.x, ball.y, ball.r, BALL)
  c:text(24, H - 8, "frame " .. ctx.frame_count, MUTED)
end

return { title = "Bounce" }
