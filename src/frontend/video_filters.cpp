#include "frontend/runtime_context.h"

#include <array>
#include <cstdio>

namespace snes::frontend {
namespace {

struct FilterOption {
    VideoFilterKind kind;
    const char *name;
    const char *description;
};

constexpr std::array<FilterOption, 7> filters{{
    {VideoFilterKind::Sharp, "NITIDO", "pixel cru, sem suavizacao"},
    {VideoFilterKind::Smooth, "SUAVE", "interpolacao linear"},
    {VideoFilterKind::Scanlines, "SCANLINES", "linhas de CRT leve"},
    {VideoFilterKind::Crt, "CRT", "scanline, grade e vinheta"},
    {VideoFilterKind::LcdGrid, "LCD GRID", "mascara de subpixel"},
    {VideoFilterKind::Warm, "QUENTE", "tint amarelado"},
    {VideoFilterKind::Green, "MONO VERDE", "monitor fosforo verde"},
}};

size_t filter_index(VideoFilterKind kind) {
    for (size_t index = 0; index < filters.size(); ++index) {
        if (filters[index].kind == kind) {
            return index;
        }
    }
    return 0;
}

bool smooth_filter(VideoFilterKind kind) {
    return kind == VideoFilterKind::Smooth;
}

void fill_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    const SDL_Rect rect{x, y, w, h};
    SDL_SetRenderDrawColor(app.renderer, r, g, b, a);
    SDL_RenderFillRect(app.renderer, &rect);
}

void draw_scanlines(Uint8 alpha, int step = 4, int thickness = 1) {
    for (int y = 1; y < game_height; y += step) {
        fill_rect(0, y, game_width, thickness, 0, 0, 0, alpha);
    }
}

void draw_vertical_mask(Uint8 alpha, int step = 6) {
    for (int x = 0; x < game_width; x += step) {
        fill_rect(x, 0, 1, game_height, 255, 40, 40, alpha);
        fill_rect(x + 2, 0, 1, game_height, 40, 255, 80, alpha);
        fill_rect(x + 4, 0, 1, game_height, 70, 130, 255, alpha);
    }
}

void draw_vignette() {
    constexpr int bands = 12;
    for (int i = 0; i < bands; ++i) {
        const Uint8 alpha = static_cast<Uint8>(18 - i);
        fill_rect(i * 6, i * 4, game_width - i * 12, 4, 0, 0, 0, alpha);
        fill_rect(i * 6, game_height - (i + 1) * 4,
                  game_width - i * 12, 4, 0, 0, 0, alpha);
        fill_rect(i * 6, i * 4, 6, game_height - i * 8, 0, 0, 0, alpha);
        fill_rect(game_width - (i + 1) * 6, i * 4, 6,
                  game_height - i * 8, 0, 0, 0, alpha);
    }
}

} // namespace

const char *video_filter_name(VideoFilterKind filter) {
    return filters[filter_index(filter)].name;
}

void apply_video_filter_to_texture() {
    if (!app.texture) {
        return;
    }
    SDL_SetTextureScaleMode(
        app.texture,
        smooth_filter(app.video_filter.current) ? SDL_ScaleModeLinear
                                                : SDL_ScaleModeNearest);
}

void draw_video_filter_overlay() {
    if (!app.renderer) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    switch (app.video_filter.current) {
    case VideoFilterKind::Sharp:
    case VideoFilterKind::Smooth:
        break;
    case VideoFilterKind::Scanlines:
        draw_scanlines(58);
        break;
    case VideoFilterKind::Crt:
        draw_scanlines(72);
        draw_vertical_mask(18);
        fill_rect(0, 0, game_width, game_height, 255, 244, 218, 12);
        draw_vignette();
        break;
    case VideoFilterKind::LcdGrid:
        draw_vertical_mask(34);
        draw_scanlines(24, 3, 1);
        break;
    case VideoFilterKind::Warm:
        fill_rect(0, 0, game_width, game_height, 255, 214, 150, 34);
        draw_scanlines(22);
        break;
    case VideoFilterKind::Green:
        fill_rect(0, 0, game_width, game_height, 20, 255, 90, 82);
        draw_scanlines(44);
        break;
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void open_video_filter_menu() {
    app.video_filter.menu_active = true;
    app.video_filter.selected = app.video_filter.current;
}

bool handle_video_filter_menu_key(SDL_Keycode key) {
    auto &state = app.video_filter;
    if (!state.menu_active) {
        return false;
    }

    if (key == SDLK_ESCAPE || key == SDLK_v) {
        state.menu_active = false;
        return true;
    }

    size_t index = filter_index(state.selected);
    if (key == SDLK_UP || key == SDLK_LEFT) {
        index = index == 0 ? filters.size() - 1 : index - 1;
        state.selected = filters[index].kind;
        return true;
    }
    if (key == SDLK_DOWN || key == SDLK_RIGHT) {
        index = (index + 1) % filters.size();
        state.selected = filters[index].kind;
        return true;
    }
    if (key >= SDLK_1 && key <= SDLK_7) {
        index = static_cast<size_t>(key - SDLK_1);
        state.selected = filters[index].kind;
        state.current = state.selected;
        state.menu_active = false;
        apply_video_filter_to_texture();
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
        state.current = state.selected;
        state.menu_active = false;
        apply_video_filter_to_texture();
        return true;
    }

    return true;
}

void draw_video_filter_menu() {
    const auto &state = app.video_filter;
    if (!state.menu_active) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    const SDL_Rect box{250, 132, 524, 464};
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 238);
    SDL_RenderFillRect(app.renderer, &box);
    SDL_SetRenderDrawColor(app.renderer, 120, 230, 255, 225);
    SDL_RenderDrawRect(app.renderer, &box);

    draw_text(box.x + 22, box.y + 22, "FILTROS DE VIDEO",
              SDL_Color{120, 230, 255, 255}, 2);
    draw_text(box.x + 22, box.y + 58,
              "SETAS NAVEGAM  ENTER APLICA  V/ESC FECHA",
              SDL_Color{190, 210, 225, 255}, 1);

    for (size_t i = 0; i < filters.size(); ++i) {
        const auto &filter = filters[i];
        const bool selected = filter.kind == state.selected;
        const bool active = filter.kind == state.current;
        const int y = box.y + 94 + static_cast<int>(i) * 46;

        if (selected) {
            const SDL_Rect row{box.x + 16, y - 8, box.w - 32, 38};
            SDL_SetRenderDrawColor(app.renderer, 255, 220, 90, 215);
            SDL_RenderFillRect(app.renderer, &row);
        }

        char line[96];
        std::snprintf(line, sizeof(line), "%zu  %s%s",
                      i + 1, filter.name, active ? "  *" : "");
        draw_text(box.x + 30, y, line,
                  selected ? SDL_Color{5, 8, 15, 255}
                           : SDL_Color{235, 240, 246, 255},
                  1);
        draw_text(box.x + 210, y, filter.description,
                  selected ? SDL_Color{30, 36, 44, 255}
                           : SDL_Color{145, 170, 195, 255},
                  1);
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

} // namespace snes::frontend
