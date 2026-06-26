#include "frontend/runtime_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace snes::frontend {
namespace {

struct FilterOption {
    VideoFilterKind kind;
    const char *name;
    const char *description;
};

constexpr std::array<FilterOption, 10> filters{{
    {VideoFilterKind::Sharp, "NITIDO", "pixel cru, sem suavizacao"},
    {VideoFilterKind::Pretty, "BONITO", "2x suave, anti-serrilhado"},
    {VideoFilterKind::Smooth, "SUAVE", "interpolacao linear"},
    {VideoFilterKind::Fsr, "FSR", "sharpening forte sem blur linear"},
    {VideoFilterKind::Fsr3, "FSR3", "upscale suave + nitidez ajustavel"},
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

bool fsr_filter(VideoFilterKind kind) {
    return kind == VideoFilterKind::Fsr;
}

bool fsr3_filter(VideoFilterKind kind) {
    return kind == VideoFilterKind::Fsr3;
}

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

std::vector<uint32_t> fsr_pixels;

uint8_t clamp_u8(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

uint32_t pack_argb(Rgb color) {
    return 0xff000000U |
           (static_cast<uint32_t>(clamp_u8(color.r)) << 16U) |
           (static_cast<uint32_t>(clamp_u8(color.g)) << 8U) |
           static_cast<uint32_t>(clamp_u8(color.b));
}

Rgb unpack_pixel(const snes::VideoFrame &frame, int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(frame.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(frame.height) - 1);
    const uint8_t *row = frame.pixels.data() + y * frame.pitch;
    if (frame.format == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t pixel = 0;
        std::memcpy(&pixel, row + x * 4, sizeof(pixel));
        return Rgb{
            static_cast<float>((pixel >> 16U) & 0xffU),
            static_cast<float>((pixel >> 8U) & 0xffU),
            static_cast<float>(pixel & 0xffU),
        };
    }
    uint16_t pixel = 0;
    std::memcpy(&pixel, row + x * 2, sizeof(pixel));
    const float r = static_cast<float>((pixel >> 11U) & 0x1fU) * 255.0f / 31.0f;
    const float g = static_cast<float>((pixel >> 5U) & 0x3fU) * 255.0f / 63.0f;
    const float b = static_cast<float>(pixel & 0x1fU) * 255.0f / 31.0f;
    return Rgb{r, g, b};
}

float luma(Rgb color) {
    return color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
}

Rgb sharpen(Rgb center, Rgb left, Rgb right, Rgb up, Rgb down) {
    const Rgb blur{
        (left.r + right.r + up.r + down.r) * 0.25f,
        (left.g + right.g + up.g + down.g) * 0.25f,
        (left.b + right.b + up.b + down.b) * 0.25f,
    };
    const float contrast =
        std::min(1.0f, std::abs(luma(center) - luma(blur)) / 36.0f);
    const float strength = 0.34f + contrast * 0.46f;
    const auto sharpen_channel = [&](float value, float blurred) {
        const float detail = std::clamp(value - blurred, -42.0f, 42.0f);
        return value + detail * strength;
    };
    return Rgb{
        sharpen_channel(center.r, blur.r),
        sharpen_channel(center.g, blur.g),
        sharpen_channel(center.b, blur.b),
    };
}

Rgb mix(Rgb a, Rgb b, float amount) {
    return Rgb{
        a.r + (b.r - a.r) * amount,
        a.g + (b.g - a.g) * amount,
        a.b + (b.b - a.b) * amount,
    };
}

float color_distance(Rgb a, Rgb b) {
    const float dr = a.r - b.r;
    const float dg = a.g - b.g;
    const float db = a.b - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

Rgb sample_bilinear(const snes::VideoFrame &frame, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const Rgb top = mix(unpack_pixel(frame, x0, y0),
                        unpack_pixel(frame, x0 + 1, y0), tx);
    const Rgb bottom = mix(unpack_pixel(frame, x0, y0 + 1),
                           unpack_pixel(frame, x0 + 1, y0 + 1), tx);
    return mix(top, bottom, ty);
}

float smootherstep(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

Rgb sample_smooth(const snes::VideoFrame &frame, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float tx = smootherstep(x - static_cast<float>(x0));
    const float ty = smootherstep(y - static_cast<float>(y0));
    const Rgb top = mix(unpack_pixel(frame, x0, y0),
                        unpack_pixel(frame, x0 + 1, y0), tx);
    const Rgb bottom = mix(unpack_pixel(frame, x0, y0 + 1),
                           unpack_pixel(frame, x0 + 1, y0 + 1), tx);
    return mix(top, bottom, ty);
}

Rgb sharpen_upscaled_pixel(const std::vector<Rgb> &source, unsigned width,
                           unsigned height, int x, int y, float amount) {
    const auto at = [&](int px, int py) {
        px = std::clamp(px, 0, static_cast<int>(width) - 1);
        py = std::clamp(py, 0, static_cast<int>(height) - 1);
        return source[static_cast<size_t>(py) * width + px];
    };
    const Rgb center = at(x, y);
    const Rgb blur{
        (at(x - 1, y).r + at(x + 1, y).r + at(x, y - 1).r + at(x, y + 1).r) *
            0.25f,
        (at(x - 1, y).g + at(x + 1, y).g + at(x, y - 1).g + at(x, y + 1).g) *
            0.25f,
        (at(x - 1, y).b + at(x + 1, y).b + at(x, y - 1).b + at(x, y + 1).b) *
            0.25f,
    };
    const auto apply = [&](float value, float blurred) {
        const float detail = std::clamp(value - blurred, -28.0f, 28.0f);
        return value + detail * amount;
    };
    return Rgb{
        apply(center.r, blur.r),
        apply(center.g, blur.g),
        apply(center.b, blur.b),
    };
}

Rgb enhance_color(Rgb color) {
    constexpr float saturation = 1.08f;
    constexpr float contrast = 1.04f;
    constexpr float brightness = 1.0f;
    const float gray = luma(color);
    color.r = gray + (color.r - gray) * saturation;
    color.g = gray + (color.g - gray) * saturation;
    color.b = gray + (color.b - gray) * saturation;
    color.r = (color.r - 128.0f) * contrast + 128.0f + brightness;
    color.g = (color.g - 128.0f) * contrast + 128.0f + brightness;
    color.b = (color.b - 128.0f) * contrast + 128.0f + brightness;
    return color;
}

bool ensure_texture(Uint32 format, unsigned width, unsigned height,
                    SDL_ScaleMode scale_mode) {
    if (!app.texture || app.texture_width != width ||
        app.texture_height != height) {
        SDL_DestroyTexture(app.texture);
        app.texture = SDL_CreateTexture(
            app.renderer, format, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(width), static_cast<int>(height));
        if (!app.texture) {
            std::cerr << "Falha ao criar textura: " << SDL_GetError() << '\n';
            app.running = false;
            return false;
        }
        app.texture_width = width;
        app.texture_height = height;
    }
    SDL_SetTextureScaleMode(app.texture, scale_mode);
    return true;
}

bool update_fsr_texture(const snes::VideoFrame &frame) {
    if (!ensure_texture(SDL_PIXELFORMAT_ARGB8888, frame.width, frame.height,
                        SDL_ScaleModeNearest)) {
        return false;
    }
    fsr_pixels.resize(static_cast<size_t>(frame.width) * frame.height);

    for (int y = 0; y < static_cast<int>(frame.height); ++y) {
        for (int x = 0; x < static_cast<int>(frame.width); ++x) {
            const Rgb center = unpack_pixel(frame, x, y);
            const Rgb color = sharpen(
                center,
                unpack_pixel(frame, x - 1, y),
                unpack_pixel(frame, x + 1, y),
                unpack_pixel(frame, x, y - 1),
                unpack_pixel(frame, x, y + 1));
            fsr_pixels[static_cast<size_t>(y) * frame.width + x] =
                pack_argb(color);
        }
    }

    SDL_UpdateTexture(app.texture, nullptr, fsr_pixels.data(),
                      static_cast<int>(frame.width * sizeof(uint32_t)));
    return true;
}

bool update_pretty_texture(const snes::VideoFrame &frame) {
    const unsigned output_width = std::max(1U, frame.width * 2U);
    const unsigned output_height = std::max(1U, frame.height * 2U);
    if (!ensure_texture(SDL_PIXELFORMAT_ARGB8888, output_width, output_height,
                        SDL_ScaleModeLinear)) {
        return false;
    }
    fsr_pixels.resize(static_cast<size_t>(output_width) * output_height);

    for (int y = 0; y < static_cast<int>(output_height); ++y) {
        const float source_y = (static_cast<float>(y) + 0.5f) * 0.5f - 0.5f;
        for (int x = 0; x < static_cast<int>(output_width); ++x) {
            const float source_x = (static_cast<float>(x) + 0.5f) * 0.5f - 0.5f;
            const Rgb color = enhance_color(sample_bilinear(frame, source_x, source_y));
            fsr_pixels[static_cast<size_t>(y) * output_width + x] =
                pack_argb(color);
        }
    }

    SDL_UpdateTexture(app.texture, nullptr, fsr_pixels.data(),
                      static_cast<int>(output_width * sizeof(uint32_t)));
    return true;
}

bool update_fsr3_texture(const snes::VideoFrame &frame) {
    const unsigned scale = 3U;
    const unsigned output_width = std::max(1U, frame.width * scale);
    const unsigned output_height = std::max(1U, frame.height * scale);
    if (!ensure_texture(SDL_PIXELFORMAT_ARGB8888, output_width, output_height,
                        SDL_ScaleModeLinear)) {
        return false;
    }

    static std::vector<Rgb> smooth_pixels;
    static std::vector<Rgb> temporal_pixels;
    static unsigned temporal_width = 0;
    static unsigned temporal_height = 0;
    smooth_pixels.resize(static_cast<size_t>(output_width) * output_height);
    fsr_pixels.resize(smooth_pixels.size());
    const bool temporal_valid = temporal_width == output_width &&
                                temporal_height == output_height &&
                                temporal_pixels.size() == smooth_pixels.size();
    if (!temporal_valid) {
        temporal_pixels.assign(smooth_pixels.size(), {});
        temporal_width = output_width;
        temporal_height = output_height;
    }

    for (int y = 0; y < static_cast<int>(output_height); ++y) {
        const float source_y =
            (static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f;
        for (int x = 0; x < static_cast<int>(output_width); ++x) {
            const float source_x =
                (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) -
                0.5f;
            smooth_pixels[static_cast<size_t>(y) * output_width + x] =
                enhance_color(sample_smooth(frame, source_x, source_y));
        }
    }

    if (temporal_valid) {
        for (size_t index = 0; index < smooth_pixels.size(); ++index) {
            const float difference =
                color_distance(smooth_pixels[index], temporal_pixels[index]);
            const float stable =
                1.0f - std::clamp(difference / 52.0f, 0.0f, 1.0f);
            const float history_weight = 0.42f * stable;
            smooth_pixels[index] =
                mix(smooth_pixels[index], temporal_pixels[index], history_weight);
        }
    }
    temporal_pixels = smooth_pixels;

    const float amount =
        std::clamp(app.video_filter.sharpness, 0, 100) / 100.0f * 0.85f;
    for (int y = 0; y < static_cast<int>(output_height); ++y) {
        for (int x = 0; x < static_cast<int>(output_width); ++x) {
            const Rgb color = amount > 0.001f
                                  ? sharpen_upscaled_pixel(
                                        smooth_pixels, output_width,
                                        output_height, x, y, amount)
                                  : smooth_pixels[static_cast<size_t>(y) *
                                                  output_width + x];
            fsr_pixels[static_cast<size_t>(y) * output_width + x] =
                pack_argb(color);
        }
    }

    SDL_UpdateTexture(app.texture, nullptr, fsr_pixels.data(),
                      static_cast<int>(output_width * sizeof(uint32_t)));
    return true;
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
    case VideoFilterKind::Pretty:
    case VideoFilterKind::Smooth:
    case VideoFilterKind::Fsr:
    case VideoFilterKind::Fsr3:
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
    if (key == SDLK_LEFTBRACKET) {
        adjust_video_filter_sharpness(-5);
        return true;
    }
    if (key == SDLK_RIGHTBRACKET) {
        adjust_video_filter_sharpness(5);
        return true;
    }
    if (key >= SDLK_1 && key <= SDLK_9) {
        index = static_cast<size_t>(key - SDLK_1);
        if (index < filters.size()) {
            state.selected = filters[index].kind;
            state.current = state.selected;
            state.menu_active = false;
            apply_video_filter_to_texture();
        }
        return true;
    }
    if (key == SDLK_0 && filters.size() >= 10) {
        state.selected = filters[9].kind;
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

bool adjust_video_filter_sharpness(int delta) {
    const int previous = app.video_filter.sharpness;
    app.video_filter.sharpness =
        std::clamp(app.video_filter.sharpness + delta, 0, 100);
    return app.video_filter.sharpness != previous;
}

void draw_video_filter_menu() {
    const auto &state = app.video_filter;
    if (!state.menu_active) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    const SDL_Rect box{250, 108, 524, 548};
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 238);
    SDL_RenderFillRect(app.renderer, &box);
    SDL_SetRenderDrawColor(app.renderer, 120, 230, 255, 225);
    SDL_RenderDrawRect(app.renderer, &box);

    draw_text(box.x + 22, box.y + 22, "FILTROS DE VIDEO",
              SDL_Color{120, 230, 255, 255}, 2);
    draw_text(box.x + 22, box.y + 58,
              "SETAS NAVEGAM  [ ] NITIDEZ  ENTER APLICA",
              SDL_Color{190, 210, 225, 255}, 1);
    char sharpness_line[48];
    std::snprintf(sharpness_line, sizeof(sharpness_line), "NITIDEZ %3d",
                  app.video_filter.sharpness);
    draw_text(box.x + 392, box.y + 58, sharpness_line,
              SDL_Color{255, 220, 90, 255}, 1);

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
        std::snprintf(line, sizeof(line), "%c  %s%s",
                      i == 9 ? '0' : static_cast<char>('1' + i),
                      filter.name, active ? "  *" : "");
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

bool update_video_filter_texture(const snes::VideoFrame &frame) {
    if (fsr3_filter(app.video_filter.current)) {
        return update_fsr3_texture(frame);
    }
    if (app.video_filter.current == VideoFilterKind::Pretty) {
        return update_pretty_texture(frame);
    }
    if (fsr_filter(app.video_filter.current)) {
        return update_fsr_texture(frame);
    }

    const Uint32 format = frame.format == RETRO_PIXEL_FORMAT_XRGB8888
                              ? SDL_PIXELFORMAT_ARGB8888
                              : SDL_PIXELFORMAT_RGB565;
    if (!ensure_texture(
            format, frame.width, frame.height,
            smooth_filter(app.video_filter.current) ? SDL_ScaleModeLinear
                                                    : SDL_ScaleModeNearest)) {
        return false;
    }
    SDL_UpdateTexture(app.texture, nullptr, frame.pixels.data(),
                      static_cast<int>(frame.pitch));
    return true;
}

} // namespace snes::frontend
