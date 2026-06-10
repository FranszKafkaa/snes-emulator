-- Exemplo simples de automacao frame a frame.
-- Rode com:
-- ./build/snes mario.sfc --script scripts/exemplo-bot.lua

function on_frame(frame)
    snes.clear_input()

    -- Anda para a direita quase sempre.
    snes.press("right")

    -- Pula em ciclos curtos.
    if frame % 90 < 18 then
        snes.press("b")
    end

    -- Exemplo de leitura: powerup do Mario no Super Mario World.
    local powerup = snes.read8(0x7E0019)
    if frame % 60 == 0 then
        snes.log("frame", frame, "powerup", powerup)
    end
end
