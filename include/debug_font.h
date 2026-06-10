#pragma once

#include <SDL.h>

#include <string_view>

namespace snes {

void draw_debug_text(SDL_Renderer *renderer, int x, int y,
                     std::string_view text, SDL_Color color, int scale = 2);

} // namespace snes
