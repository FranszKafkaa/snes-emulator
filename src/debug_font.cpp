#include "debug_font.h"

#include <array>
#include <cctype>

namespace snes {
namespace {

std::array<uint8_t, 7> glyph(char character) {
    switch (character) {
    case '0': return {14, 17, 19, 21, 25, 17, 14};
    case '1': return {4, 12, 4, 4, 4, 4, 14};
    case '2': return {14, 17, 1, 2, 4, 8, 31};
    case '3': return {30, 1, 1, 14, 1, 1, 30};
    case '4': return {2, 6, 10, 18, 31, 2, 2};
    case '5': return {31, 16, 16, 30, 1, 1, 30};
    case '6': return {14, 16, 16, 30, 17, 17, 14};
    case '7': return {31, 1, 2, 4, 8, 8, 8};
    case '8': return {14, 17, 17, 14, 17, 17, 14};
    case '9': return {14, 17, 17, 15, 1, 1, 14};
    case 'A': return {14, 17, 17, 31, 17, 17, 17};
    case 'B': return {30, 17, 17, 30, 17, 17, 30};
    case 'C': return {14, 17, 16, 16, 16, 17, 14};
    case 'D': return {30, 17, 17, 17, 17, 17, 30};
    case 'E': return {31, 16, 16, 30, 16, 16, 31};
    case 'F': return {31, 16, 16, 30, 16, 16, 16};
    case 'G': return {14, 17, 16, 23, 17, 17, 14};
    case 'H': return {17, 17, 17, 31, 17, 17, 17};
    case 'I': return {14, 4, 4, 4, 4, 4, 14};
    case 'J': return {7, 2, 2, 2, 2, 18, 12};
    case 'K': return {17, 18, 20, 24, 20, 18, 17};
    case 'L': return {16, 16, 16, 16, 16, 16, 31};
    case 'M': return {17, 27, 21, 21, 17, 17, 17};
    case 'N': return {17, 25, 21, 19, 17, 17, 17};
    case 'O': return {14, 17, 17, 17, 17, 17, 14};
    case 'P': return {30, 17, 17, 30, 16, 16, 16};
    case 'Q': return {14, 17, 17, 17, 21, 18, 13};
    case 'R': return {30, 17, 17, 30, 20, 18, 17};
    case 'S': return {15, 16, 16, 14, 1, 1, 30};
    case 'T': return {31, 4, 4, 4, 4, 4, 4};
    case 'U': return {17, 17, 17, 17, 17, 17, 14};
    case 'V': return {17, 17, 17, 17, 17, 10, 4};
    case 'W': return {17, 17, 17, 21, 21, 21, 10};
    case 'X': return {17, 17, 10, 4, 10, 17, 17};
    case 'Y': return {17, 17, 10, 4, 4, 4, 4};
    case 'Z': return {31, 1, 2, 4, 8, 16, 31};
    case ':': return {0, 4, 4, 0, 4, 4, 0};
    case ';': return {0, 4, 4, 0, 4, 4, 8};
    case '-': return {0, 0, 0, 31, 0, 0, 0};
    case '_': return {0, 0, 0, 0, 0, 0, 31};
    case '.': return {0, 0, 0, 0, 0, 4, 4};
    case ',': return {0, 0, 0, 0, 0, 4, 8};
    case '/': return {1, 2, 2, 4, 8, 8, 16};
    case '+': return {0, 4, 4, 31, 4, 4, 0};
    case '*': return {0, 21, 14, 31, 14, 21, 0};
    case '=': return {0, 0, 31, 0, 31, 0, 0};
    case '$': return {4, 15, 20, 14, 5, 30, 4};
    case '#': return {10, 31, 10, 10, 31, 10, 0};
    case '%': return {24, 25, 2, 4, 8, 19, 3};
    case '(': return {2, 4, 8, 8, 8, 4, 2};
    case ')': return {8, 4, 2, 2, 2, 4, 8};
    case '[': return {14, 8, 8, 8, 8, 8, 14};
    case ']': return {14, 2, 2, 2, 2, 2, 14};
    case '{': return {2, 4, 4, 8, 4, 4, 2};
    case '}': return {8, 4, 4, 2, 4, 4, 8};
    case '<': return {2, 4, 8, 16, 8, 4, 2};
    case '>': return {8, 4, 2, 1, 2, 4, 8};
    case '\'': return {4, 4, 8, 0, 0, 0, 0};
    case '"': return {10, 10, 20, 0, 0, 0, 0};
    case '!': return {4, 4, 4, 4, 4, 0, 4};
    case '?': return {14, 17, 1, 2, 4, 0, 4};
    default: return {};
    }
}

} // namespace

void draw_debug_text(SDL_Renderer *renderer, int x, int y,
                     std::string_view text, SDL_Color color, int scale) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const int start_x = x;
    for (char character : text) {
        if (character == '\n') {
            x = start_x;
            y += 9 * scale;
            continue;
        }
        const auto rows = glyph(static_cast<char>(
            std::toupper(static_cast<unsigned char>(character))));
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (rows[row] & (1 << (4 - column))) {
                    SDL_Rect pixel{x + column * scale, y + row * scale,
                                   scale, scale};
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
        x += 6 * scale;
    }
}

} // namespace snes
