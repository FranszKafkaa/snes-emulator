#include "frontend/runtime_context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>

#include "debug_font.h"

extern char **environ;

namespace snes::frontend {

void core_log(enum retro_log_level level, const char *format, ...) {
    const char *prefix = level == RETRO_LOG_ERROR ? "erro" :
                         level == RETRO_LOG_WARN ? "aviso" : "core";
    std::fprintf(stderr, "[%s] ", prefix);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

bool environment(unsigned command, void *data) {
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto format = *static_cast<enum retro_pixel_format *>(data);
        if (format != RETRO_PIXEL_FORMAT_RGB565 &&
            format != RETRO_PIXEL_FORMAT_XRGB8888) {
            return false;
        }
        app.pixel_format = format;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char **>(data) = app.data_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback *>(data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool *>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return false;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto *message = static_cast<const retro_message *>(data);
        std::cerr << "[mensagem] " << message->msg << '\n';
        return true;
    }
    default:
        return false;
    }
}

void update_hash(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (!data) {
        return;
    }
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t row_bytes = 0;
    size_t frame_size = 0;
    if (!frame_layout(width, height, pitch, row_bytes, frame_size)) {
        std::cerr << "Frame ignorado: layout invalido para hash.\n";
        return;
    }
    for (unsigned y = 0; y < height; ++y) {
        for (size_t x = 0; x < row_bytes; ++x) {
            app.frame_hash ^= bytes[y * pitch + x];
            app.frame_hash *= 1099511628211ULL;
        }
    }
}

void start_media_workers() {
    app.video_pipeline = std::make_unique<snes::VideoPipeline>(
        [](const snes::VideoFrame &frame) {
            update_hash(frame.pixels.data(), frame.width, frame.height,
                        frame.pitch);
        });
    app.video_pipeline->start();
    if (app.audio) {
        app.audio_pipeline =
            std::make_unique<snes::AudioPipeline>(app.audio);
        app.audio_pipeline->start();
    }
}

void stop_media_workers() {
    if (app.video_pipeline) app.video_pipeline->stop();
    if (app.audio_pipeline) app.audio_pipeline->stop();
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    track_memory_activity(data, width, height, pitch);
    if (!data) {
        return;
    }
    size_t packed_pitch = 0;
    size_t frame_size = 0;
    if (!frame_layout(width, height, pitch, packed_pitch, frame_size)) {
        std::cerr << "Frame ignorado: layout invalido.\n";
        return;
    }
    if (app.headless) {
        update_hash(data, width, height, pitch);
        return;
    }

    auto frame = std::make_shared<snes::VideoFrame>();
    frame->pixels.resize(frame_size);
    frame->width = width;
    frame->height = height;
    frame->pitch = packed_pitch;
    frame->format = app.pixel_format;
    const auto *source = static_cast<const uint8_t *>(data);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(frame->pixels.data() + y * packed_pitch,
                    source + y * pitch, packed_pitch);
    }
    if (app.video_pipeline) {
        app.video_pipeline->submit(std::move(frame));
    }
}

void draw_lua_overlay() {
    if (app.lua.draw_commands.empty()) {
        return;
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    for (const auto &command : app.lua.draw_commands) {
        SDL_SetRenderDrawColor(app.renderer, command.color.r, command.color.g,
                               command.color.b, command.color.a);
        switch (command.kind) {
        case LuaRuntime::DrawKind::Text:
            draw_text(command.x, command.y, command.text, command.color,
                      command.scale);
            break;
        case LuaRuntime::DrawKind::Rect: {
            const SDL_Rect rect{command.x, command.y, command.w, command.h};
            if (command.filled) {
                SDL_RenderFillRect(app.renderer, &rect);
            } else {
                SDL_RenderDrawRect(app.renderer, &rect);
            }
            break;
        }
        case LuaRuntime::DrawKind::Line:
            SDL_RenderDrawLine(app.renderer, command.x, command.y,
                               command.x2, command.y2);
            break;
        }
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void draw_frontend_status() {
    const int speed = effective_speed_multiplier();
    if (speed <= 1 && !app.paused) {
        return;
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 7, 12, 20, 210);
    const SDL_Rect box{game_width - 168, 16, 146, 34};
    SDL_RenderFillRect(app.renderer, &box);
    SDL_SetRenderDrawColor(app.renderer, 120, 230, 255, 210);
    SDL_RenderDrawRect(app.renderer, &box);
    char line[64];
    std::snprintf(line, sizeof(line), app.paused ? "PAUSADO  TAB %dx" : "TURBO TAB %dx",
                  speed);
    draw_text(box.x + 10, box.y + 11, line,
              app.paused ? SDL_Color{255, 220, 90, 255}
                         : SDL_Color{120, 230, 255, 255},
              1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void present_latest_frame() {
    const auto frame =
        app.video_pipeline ? app.video_pipeline->latest() : nullptr;
    if (!frame || frame->serial == app.presented_serial) {
        return;
    }
    apply_stretched_render_scale();

    if (!app.texture || frame->width != app.texture_width ||
        frame->height != app.texture_height) {
        SDL_DestroyTexture(app.texture);
        const Uint32 format = frame->format == RETRO_PIXEL_FORMAT_XRGB8888
                                  ? SDL_PIXELFORMAT_ARGB8888
                                  : SDL_PIXELFORMAT_RGB565;
        app.texture = SDL_CreateTexture(
            app.renderer, format, SDL_TEXTUREACCESS_STREAMING,
            frame->width, frame->height);
        if (!app.texture) {
            std::cerr << "Falha ao criar textura: " << SDL_GetError() << '\n';
            app.running = false;
            return;
        }
        app.texture_width = frame->width;
        app.texture_height = frame->height;
    }

    SDL_UpdateTexture(app.texture, nullptr, frame->pixels.data(),
                      static_cast<int>(frame->pitch));
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);
    const SDL_Rect game_area{0, 0, game_width, game_height};
    SDL_RenderCopy(app.renderer, app.texture, nullptr, &game_area);
    draw_lua_overlay();
    draw_frontend_status();
    draw_lua_script_picker();
    if (app.memory_debug && !window_fullscreen()) {
        draw_screen_memory_marker();
        draw_memory_debugger();
    }
    SDL_RenderPresent(app.renderer);
    app.presented_serial = frame->serial;
}

size_t audio_batch(const int16_t *data, size_t frames);

void audio_sample(int16_t left, int16_t right) {
    const int16_t samples[] = {left, right};
    audio_batch(samples, 1);
}

size_t audio_batch(const int16_t *data, size_t frames) {
    if (!app.audio_pipeline || !data || !frames) {
        return frames;
    }
    app.audio_pipeline->submit(std::span<const int16_t>(data, frames * 2));
    return frames;
}

void input_poll() {}

int16_t input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }
    if (id < app.lua.buttons.size() && app.lua.buttons[id]) {
        return 1;
    }
    if (app.headless ||
        app.memory_editor.active || app.memory_editor.goto_popup) {
        return 0;
    }
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP: return keys[SDL_SCANCODE_UP];
    case RETRO_DEVICE_ID_JOYPAD_DOWN: return keys[SDL_SCANCODE_DOWN];
    case RETRO_DEVICE_ID_JOYPAD_LEFT: return keys[SDL_SCANCODE_LEFT];
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return keys[SDL_SCANCODE_RIGHT];
    case RETRO_DEVICE_ID_JOYPAD_B: return keys[SDL_SCANCODE_Z];
    case RETRO_DEVICE_ID_JOYPAD_A: return keys[SDL_SCANCODE_X];
    case RETRO_DEVICE_ID_JOYPAD_Y: return keys[SDL_SCANCODE_A];
    case RETRO_DEVICE_ID_JOYPAD_X: return keys[SDL_SCANCODE_S];
    case RETRO_DEVICE_ID_JOYPAD_L: return keys[SDL_SCANCODE_Q];
    case RETRO_DEVICE_ID_JOYPAD_R: return keys[SDL_SCANCODE_W];
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return keys[SDL_SCANCODE_RSHIFT];
    case RETRO_DEVICE_ID_JOYPAD_START: return keys[SDL_SCANCODE_RETURN];
    default: return 0;
    }
}

void load_sram() {
    auto *memory = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->load_sram({memory, memory_size});
        std::cout << "Save RAM carregado.\n";
    }
}

void save_sram() {
    const auto *memory =
        static_cast<const uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->save_sram({memory, memory_size});
    }
}

bool save_current_state() {
    std::vector<uint8_t> state(retro_serialize_size());
    if (state.empty() || !retro_serialize(state.data(), state.size())) {
        std::cerr << "Nao foi possivel salvar o estado.\n";
        return false;
    }
    if (save_manager && save_manager->save_state(state)) {
        std::cout << "Estado salvo.\n";
        return true;
    }
    return false;
}

bool load_current_state() {
    std::vector<uint8_t> state;
    if (!save_manager || !save_manager->load_state(state) ||
        !retro_unserialize(state.data(), state.size())) {
        std::cerr << "Nao foi possivel carregar o estado.\n";
        return false;
    }
    std::cout << "Estado carregado.\n";
    return true;
}

void save_state() {
    save_current_state();
}

void load_state() {
    load_current_state();
}

bool init_sdl(const retro_system_av_info &av) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "Falha ao iniciar SDL2: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    const std::string window_title =
        "SNES C++ - " + rom_path.filename().string();
    app.window = SDL_CreateWindow(
        window_title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!app.window) {
        std::cerr << "Falha ao criar janela: " << SDL_GetError() << '\n';
        return false;
    }
    app.renderer = SDL_CreateRenderer(
        app.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!app.renderer) {
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!app.renderer) {
        std::cerr << "Falha ao criar renderer: " << SDL_GetError() << '\n';
        return false;
    }
    apply_stretched_render_scale();

    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(av.timing.sample_rate);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    app.audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (app.audio) {
        SDL_PauseAudioDevice(app.audio, 0);
    } else {
        std::cerr << "Audio desativado: " << SDL_GetError() << '\n';
    }
    return true;
}

} // namespace snes::frontend
