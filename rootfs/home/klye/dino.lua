-- dino.lua - stylized dino runner
--
local BG     = 0x181A1F
local DINO   = 0x6FD3FF
local OBST   = 0x51CF66  -- Green cactus
local GROUND = 0x3A4454
local TEXT   = 0xE8EAF0
local MUTED  = 0x8B95A8

local game = {
    score = 0,
    high_score = 0,
    speed = 4,
    over = false,
    dino = {
        x = 40,
        y = 260,
        w = 26,
        h = 32,
        vy = 0,
        grounded = true,
    },
    obs = {
        x = 520,
        y = 262,
        w = 16,
        h = 30,
    }
}

function update(dt)
if game.over then
    if ctx:key() or ctx:mouse_down() then
        game.score = 0
        game.speed = 4
        game.over = false
        game.obs.x = 520
        game.dino.y = 260
        game.dino.vy = 0
        game.dino.grounded = true
        end
        return
        end

        -- Jump input
        if (ctx:key() == " " or ctx:mouse_down()) and game.dino.grounded then
            game.dino.vy = -10
            game.dino.grounded = false
            end

            -- Gravity
            game.dino.vy = game.dino.vy + 0.6
            game.dino.y = game.dino.y + game.dino.vy

            -- Floor collision
            if game.dino.y >= 260 then
                game.dino.y = 260
                game.dino.vy = 0
                game.dino.grounded = true
                end

                -- Move obstacle
                game.obs.x = game.obs.x - game.speed
                if game.obs.x < -25 then
                    game.obs.x = 520
                    game.score = game.score + 1
                    game.speed = 4 + math.floor(game.score / 5)
                    end

                    -- Collision Detection
                    if game.dino.x < game.obs.x + game.obs.w and
                        game.dino.x + game.dino.w > game.obs.x and
                        game.dino.y < game.obs.y + game.obs.h and
                        game.dino.y + game.dino.h > game.obs.y then
                        game.over = true
                        if game.score > game.high_score then
                            game.high_score = game.score
                            end
                            end
                            end

                            function paint(c)
                            c:clear(0, 0, 520, 380, BG)

                            -- Ground line
                            c:line(0, 292, 520, GROUND)

                            -- Draw Stylized Dino (composed of smaller blocks for head, body, tail, legs)
                            local dx, dy = game.dino.x, game.dino.y
                            -- Body
                            c:fill(dx + 4, dy + 10, 18, 16, DINO)
                            -- Head & Snout
                            c:fill(dx + 12, dy, 12, 12, DINO)
                            -- Tail
                            c:fill(dx, dy + 12, 6, 8, DINO)
                            -- Legs
                            c:fill(dx + 6, dy + 26, 4, 6, DINO)
                            c:fill(dx + 16, dy + 26, 4, 6, DINO)

                            -- Draw Stylized Cactus Obstacle
                            local ox, oy = game.obs.x, game.obs.y
                            -- Main trunk
                            c:fill(ox + 4, oy, 8, 30, OBST)
                            -- Left arm
                            c:fill(ox, oy + 8, 4, 10, OBST)
                            c:fill(ox, oy + 8, 8, 4, OBST)
                            -- Right arm
                            c:fill(ox + 12, oy + 14, 4, 10, OBST)
                            c:fill(ox + 8, oy + 14, 8, 4, OBST)

                            -- UI / Scores
                            c:text(24, 30, "SCORE: " .. game.score, TEXT)
                            c:text(380, 30, "HIGH: " .. game.high_score, MUTED)

                            if game.over then
                                c:text(180, 160, "GAME OVER", 0xFF6B6B)
                                c:text(140, 190, "Press any key / click to restart", MUTED)
                                else
                                    c:text(24, 350, "Press Space or Click to Jump", MUTED)
                                    end
                                    end

                                    return { title = "Dino Jump" }
