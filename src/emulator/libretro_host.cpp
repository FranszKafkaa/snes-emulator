#include "frontend/runtime_context.h"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL_opengl.h>

namespace snes::frontend {

namespace {

constexpr unsigned RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK =
    69;
constexpr unsigned RETRO_ENVIRONMENT_GET_CAN_DUPE_PRIVATE =
    0x800000 | RETRO_ENVIRONMENT_GET_CAN_DUPE;

struct CoreOptionsUpdateDisplayCallback {
    bool (*callback)(void);
};

std::unordered_map<std::string, std::string> core_variables;

uintptr_t current_framebuffer() {
    return 0;
}

extern "C" uintptr_t gl_stub() {
    return 0;
}

bool already_logged_missing_gl(const char *symbol) {
    static std::vector<std::string> missing_symbols;
    static bool limit_reported = false;
    if (!symbol) {
        return true;
    }
    for (const auto &logged : missing_symbols) {
        if (logged == symbol) {
            return true;
        }
    }
    if (missing_symbols.size() >= 24) {
        if (!limit_reported) {
            std::cerr << "[opengl] outras funcoes ausentes omitidas.\n";
            limit_reported = true;
        }
        return true;
    }
    missing_symbols.emplace_back(symbol);
    return false;
}

bool log_missing_gl_symbols() {
    static const bool enabled = std::getenv("SNES_LOG_MISSING_GL") != nullptr;
    return enabled;
}

retro_proc_address_t get_proc_address(const char *symbol) {
    if (auto *address = SDL_GL_GetProcAddress(symbol)) {
        return reinterpret_cast<retro_proc_address_t>(address);
    }
    const char *void_optional[] = {
        "glInvalidateFramebuffer",
        "glInvalidateSubFramebuffer",
        "glDebugMessageCallback",
        "glDebugMessageCallbackARB",
        "glDebugMessageControl",
        "glDebugMessageControlARB",
        "glDebugMessageInsert",
        "glDebugMessageInsertARB",
        "glPushDebugGroup",
        "glPopDebugGroup",
        "glObjectLabel",
        "glObjectPtrLabel",
        "glTextureBarrier",
        "glTextureBarrierNV",
        "glVertexP2ui",
        "glVertexP2uiv",
        "glVertexP3ui",
        "glVertexP3uiv",
        "glVertexP4ui",
        "glVertexP4uiv",
        "glTexCoordP1ui",
        "glTexCoordP1uiv",
        "glTexCoordP2ui",
        "glTexCoordP2uiv",
        "glTexCoordP3ui",
        "glTexCoordP3uiv",
        "glTexCoordP4ui",
        "glTexCoordP4uiv",
        "glMultiTexCoordP1ui",
        "glMultiTexCoordP1uiv",
        "glMultiTexCoordP2ui",
        "glMultiTexCoordP2uiv",
        "glMultiTexCoordP3ui",
        "glMultiTexCoordP3uiv",
        "glMultiTexCoordP4ui",
        "glMultiTexCoordP4uiv",
        "glNormalP3ui",
        "glNormalP3uiv",
        "glColorP3ui",
        "glColorP3uiv",
        "glColorP4ui",
        "glColorP4uiv",
        "glSecondaryColorP3ui",
        "glSecondaryColorP3uiv",
    };
    for (const char *optional : void_optional) {
        if (std::strcmp(symbol, optional) == 0) {
            return reinterpret_cast<retro_proc_address_t>(gl_stub);
        }
    }
    if (app.n64_gliden64 && log_missing_gl_symbols() &&
        !already_logged_missing_gl(symbol)) {
        std::cerr << "[opengl] funcao ausente para GLideN64: "
                  << (symbol ? symbol : "(null)") << '\n';
    }
    return nullptr;
}

std::pair<int, int> hardware_window_size() {
    if (app.n64_fast && !app.n64_widescreen) {
        return {640, 480};
    }
    return app.n64_widescreen ? std::pair{960, 540} : std::pair{960, 720};
}

void enforce_hardware_window_size() {
    if (!app.window || !app.n64_gliden64) {
        return;
    }
    const auto [width, height] = hardware_window_size();
    int current_width = 0;
    int current_height = 0;
    SDL_GetWindowSize(app.window, &current_width, &current_height);
    if (current_width != width || current_height != height) {
        SDL_SetWindowSize(app.window, width, height);
    }
    SDL_SetWindowMinimumSize(app.window, width, height);
    SDL_SetWindowMaximumSize(app.window, width, height);
}

bool set_rumble_state(unsigned, retro_rumble_effect, uint16_t) {
    return false;
}

retro_time_t perf_time_usec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::microseconds>(
               clock::now().time_since_epoch())
        .count();
}

retro_perf_tick_t perf_counter() {
    return static_cast<retro_perf_tick_t>(perf_time_usec());
}

uint64_t cpu_features() {
    return 0;
}

void perf_register(retro_perf_counter *counter) {
    if (counter) {
        counter->registered = true;
    }
}

void perf_start(retro_perf_counter *counter) {
    if (counter) {
        counter->start = perf_counter();
    }
}

void perf_stop(retro_perf_counter *counter) {
    if (counter) {
        const retro_perf_tick_t now = perf_counter();
        counter->total += now - counter->start;
        ++counter->call_cnt;
    }
}

void perf_log() {}

bool core_options_update_display() {
    return false;
}

std::string default_variable_value(const char *definition) {
    if (!definition) {
        return {};
    }
    std::string text = definition;
    const auto separator = text.find(';');
    if (separator != std::string::npos) {
        text.erase(0, separator + 1);
    }
    while (!text.empty() && text.front() == ' ') {
        text.erase(text.begin());
    }
    const auto option_separator = text.find('|');
    if (option_separator != std::string::npos) {
        text.resize(option_separator);
    }
    return text;
}

void set_core_variables(const retro_variable *variables) {
    core_variables.clear();
    if (!variables) {
        return;
    }
    for (const retro_variable *variable = variables; variable->key; ++variable) {
        core_variables[variable->key] =
            default_variable_value(variable->value);
    }
}

const char *default_core_option_value(const retro_core_option_value *values,
                                      const char *default_value) {
    if (default_value) {
        for (const auto *value = values; value && value->value; ++value) {
            if (std::strcmp(value->value, default_value) == 0) {
                return default_value;
            }
        }
    }
    return values && values[0].value ? values[0].value : "";
}

void set_core_options(const retro_core_option_definition *definitions) {
    core_variables.clear();
    if (!definitions) {
        return;
    }
    for (const auto *definition = definitions; definition->key; ++definition) {
        core_variables[definition->key] =
            default_core_option_value(definition->values,
                                      definition->default_value);
    }
}

void set_core_options_v2(const retro_core_option_v2_definition *definitions) {
    core_variables.clear();
    if (!definitions) {
        return;
    }
    for (const auto *definition = definitions; definition->key; ++definition) {
        core_variables[definition->key] =
            default_core_option_value(definition->values,
                                      definition->default_value);
    }
}

void apply_n64_core_overrides() {
    if (app.system != ConsoleSystem::N64) {
        return;
    }
    const std::pair<const char *, const char *> stable_overrides[] = {
        {"mupen64plus-cpucore", "cached_interpreter"},
        {"mupen64plus-rdp-plugin", "angrylion"},
        {"mupen64plus-rsp-plugin", "parallel"},
        {"mupen64plus-angrylion-vioverlay", "Unfiltered"},
        {"mupen64plus-angrylion-sync", "Low"},
        {"mupen64plus-angrylion-multithread", "all threads"},
        {"mupen64plus-pak1", "none"},
        {"mupen64plus-pak2", "none"},
        {"mupen64plus-pak3", "none"},
        {"mupen64plus-pak4", "none"},
    };
    const std::pair<const char *, const char *> widescreen_overrides[] = {
        {"mupen64plus-cpucore", "cached_interpreter"},
        {"mupen64plus-rdp-plugin", "gliden64"},
        {"mupen64plus-rsp-plugin", "parallel"},
        {"mupen64plus-aspect", "Wide (Adjusted)"},
        {"mupen64plus-169screensize", "960x540"},
        {"mupen64plus-EnableNativeResFactor", "1"},
        {"mupen64plus-ThreadedRenderer", "False"},
        {"mupen64plus-EnableFBEmulation", "False"},
        {"mupen64plus-EnableN64DepthCompare", "False"},
        {"mupen64plus-EnableShadersStorage", "False"},
        {"mupen64plus-EnableTextureCache", "False"},
        {"mupen64plus-GLideN64IniBehaviour", "early"},
        {"mupen64plus-pak1", "none"},
        {"mupen64plus-pak2", "none"},
        {"mupen64plus-pak3", "none"},
        {"mupen64plus-pak4", "none"},
    };
    const std::pair<const char *, const char *> gliden64_overrides[] = {
        {"mupen64plus-cpucore", "cached_interpreter"},
        {"mupen64plus-rdp-plugin", "gliden64"},
        {"mupen64plus-rsp-plugin", "parallel"},
        {"mupen64plus-aspect", "4:3"},
        {"mupen64plus-43screensize", "960x720"},
        {"mupen64plus-EnableNativeResFactor", "1"},
        {"mupen64plus-ThreadedRenderer", "False"},
        {"mupen64plus-EnableFBEmulation", "False"},
        {"mupen64plus-EnableN64DepthCompare", "False"},
        {"mupen64plus-EnableShadersStorage", "False"},
        {"mupen64plus-EnableTextureCache", "False"},
        {"mupen64plus-GLideN64IniBehaviour", "early"},
        {"mupen64plus-pak1", "none"},
        {"mupen64plus-pak2", "none"},
        {"mupen64plus-pak3", "none"},
        {"mupen64plus-pak4", "none"},
    };
    const std::pair<const char *, const char *> fast_overrides[] = {
        {"mupen64plus-cpucore", "cached_interpreter"},
        {"mupen64plus-rdp-plugin", "gliden64"},
        {"mupen64plus-rsp-plugin", "parallel"},
        {"mupen64plus-aspect", "4:3"},
        {"mupen64plus-43screensize", "640x480"},
        {"mupen64plus-169screensize", "640x360"},
        {"mupen64plus-EnableNativeResFactor", "1"},
        {"mupen64plus-ThreadedRenderer", "True"},
        {"mupen64plus-HybridFilter", "False"},
        {"mupen64plus-MultiSampling", "0"},
        {"mupen64plus-FXAA", "0"},
        {"mupen64plus-EnableNativeResTexrects", "Optimized"},
        {"mupen64plus-EnableShadersStorage", "False"},
        {"mupen64plus-EnableTextureCache", "False"},
        {"mupen64plus-FrameDuping", "False"},
        {"mupen64plus-GLideN64IniBehaviour", "early"},
        {"mupen64plus-pak1", "none"},
        {"mupen64plus-pak2", "none"},
        {"mupen64plus-pak3", "none"},
        {"mupen64plus-pak4", "none"},
    };
    const std::pair<const char *, const char *> fullspeed_overrides[] = {
        {"mupen64plus-Framerate", "Fullspeed"},
        {"mupen64plus-CountPerOp", "1"},
        {"mupen64plus-CountPerOpDenomPot", "0"},
    };
    const std::span<const std::pair<const char *, const char *>> overrides =
        app.n64_widescreen
            ? std::span<const std::pair<const char *, const char *>>{
                  widescreen_overrides}
            : app.n64_fast
                ? std::span<const std::pair<const char *, const char *>>{
                      fast_overrides}
            : app.n64_gliden64
                ? std::span<const std::pair<const char *, const char *>>{
                      gliden64_overrides}
            : std::span<const std::pair<const char *, const char *>>{
                  stable_overrides};
    for (const auto &[key, value] : overrides) {
        if (auto it = core_variables.find(key); it != core_variables.end()) {
            it->second = value;
        }
    }
    if (app.n64_fullspeed) {
        for (const auto &[key, value] : fullspeed_overrides) {
            if (auto it = core_variables.find(key);
                it != core_variables.end()) {
                it->second = value;
            }
        }
    }
}

} // namespace

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
        *static_cast<const char **>(data) = app.system_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char **>(data) = app.data_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *static_cast<const char **>(data) = app.core_assets_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback *>(data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    case RETRO_ENVIRONMENT_GET_CAN_DUPE_PRIVATE:
        *static_cast<bool *>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
        *static_cast<uint64_t *>(data) =
            (1ULL << RETRO_DEVICE_JOYPAD) |
            (1ULL << RETRO_DEVICE_ANALOG);
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        static_cast<retro_rumble_interface *>(data)->set_rumble_state =
            set_rumble_state;
        return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
        auto *perf = static_cast<retro_perf_callback *>(data);
        perf->get_time_usec = perf_time_usec;
        perf->get_cpu_features = cpu_features;
        perf->get_perf_counter = perf_counter;
        perf->perf_register = perf_register;
        perf->perf_start = perf_start;
        perf->perf_stop = perf_stop;
        perf->perf_log = perf_log;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned *>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *variable = static_cast<retro_variable *>(data);
        const auto found = core_variables.find(variable->key ? variable->key : "");
        if (found == core_variables.end()) {
            variable->value = nullptr;
            return false;
        }
        variable->value = found->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        set_core_variables(static_cast<const retro_variable *>(data));
        apply_n64_core_overrides();
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        set_core_options(static_cast<const retro_core_option_definition *>(data));
        apply_n64_core_overrides();
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
        const auto *options = static_cast<const retro_core_options_intl *>(data);
        set_core_options(options ? options->us : nullptr);
        apply_n64_core_overrides();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
        const auto *options = static_cast<const retro_core_options_v2 *>(data);
        set_core_options_v2(options ? options->definitions : nullptr);
        apply_n64_core_overrides();
        return false;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        const auto *options =
            static_cast<const retro_core_options_v2_intl *>(data);
        set_core_options_v2(options && options->us ? options->us->definitions
                                                   : nullptr);
        apply_n64_core_overrides();
        return false;
    }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned *>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        static_cast<CoreOptionsUpdateDisplayCallback *>(data)->callback =
            core_options_update_display;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto *callback = static_cast<retro_hw_render_callback *>(data);
        if (callback->context_type != RETRO_HW_CONTEXT_OPENGL &&
            callback->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
            std::cerr << "Core pediu contexto grafico nao suportado: "
                      << callback->context_type << '\n';
            return false;
        }
        callback->get_current_framebuffer = current_framebuffer;
        callback->get_proc_address = get_proc_address;
        app.hardware = *callback;
        app.hardware_render = true;
        std::cerr << "[opengl] core pediu contexto tipo "
                  << callback->context_type << " versao "
                  << callback->version_major << '.'
                  << callback->version_minor << '\n';
        if (app.headless) {
            std::cerr << "Este core precisa de OpenGL; execute sem "
                         "--headless para jogar.\n";
            return false;
        }
        if (!init_hardware_sdl()) {
            return false;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return false;
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
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
    if (!app.hardware_render) {
        app.video_pipeline = std::make_unique<snes::VideoPipeline>(
            [](const snes::VideoFrame &frame) {
                update_hash(frame.pixels.data(), frame.width, frame.height,
                            frame.pitch);
            });
        app.video_pipeline->start();
    }
    if (app.audio) {
        app.audio_pipeline =
            std::make_unique<snes::AudioPipeline>(
                app.audio, app.audio_sample_rate);
        app.audio_pipeline->start();
    }
}

void stop_media_workers() {
    if (app.video_pipeline) app.video_pipeline->stop();
    if (app.audio_pipeline) app.audio_pipeline->stop();
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    if (app.hardware_render) {
        if (data == RETRO_HW_FRAME_BUFFER_VALID || data == nullptr) {
            return;
        }
    }
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
    if (app.hardware_render) {
        if (app.window) {
            enforce_hardware_window_size();
            SDL_GL_SwapWindow(app.window);
            ++app.presented_serial;
        }
        return;
    }
    const auto frame =
        app.video_pipeline ? app.video_pipeline->latest() : nullptr;
    if (!frame || frame->serial == app.presented_serial) {
        return;
    }
    if (!update_video_filter_texture(*frame)) {
        return;
    }
    SDL_RenderSetLogicalSize(app.renderer, 0, 0);
    SDL_RenderSetViewport(app.renderer, nullptr);
    SDL_RenderSetScale(app.renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);

    apply_stretched_render_scale();
    const SDL_Rect game_area{0, 0, game_logical_width(), game_height};
    SDL_RenderCopy(app.renderer, app.texture, nullptr, &game_area);
    draw_video_filter_overlay();
    draw_lua_overlay();
    draw_frontend_status();
    draw_lua_script_picker();
    draw_video_filter_menu();
    if (app.memory_debug && !window_fullscreen()) {
        draw_screen_memory_marker();
        draw_memory_debugger();
    }
    SDL_RenderPresent(app.renderer);
    app.presented_serial = frame->serial;
}

void prepare_hardware_frame() {
    if (!app.hardware_render || !app.window || !app.gl_context) {
        return;
    }
    using GlViewport = void (*)(GLint, GLint, GLsizei, GLsizei);
    using GlScissor = void (*)(GLint, GLint, GLsizei, GLsizei);
    using GlEnable = void (*)(GLenum);
    using GlDisable = void (*)(GLenum);
    using GlClearColor = void (*)(GLfloat, GLfloat, GLfloat, GLfloat);
    using GlClear = void (*)(GLbitfield);
    const auto viewport = reinterpret_cast<GlViewport>(
        SDL_GL_GetProcAddress("glViewport"));
    const auto scissor = reinterpret_cast<GlScissor>(
        SDL_GL_GetProcAddress("glScissor"));
    const auto enable = reinterpret_cast<GlEnable>(
        SDL_GL_GetProcAddress("glEnable"));
    const auto disable = reinterpret_cast<GlDisable>(
        SDL_GL_GetProcAddress("glDisable"));
    const auto clear_color = reinterpret_cast<GlClearColor>(
        SDL_GL_GetProcAddress("glClearColor"));
    const auto clear = reinterpret_cast<GlClear>(
        SDL_GL_GetProcAddress("glClear"));
    if (!viewport || !scissor || !enable || !disable ||
        !clear_color || !clear) {
        return;
    }
    SDL_GL_MakeCurrent(app.window, app.gl_context);
    enforce_hardware_window_size();
    int drawable_width = 0;
    int drawable_height = 0;
    SDL_GL_GetDrawableSize(app.window, &drawable_width, &drawable_height);
    if (drawable_width <= 0 || drawable_height <= 0) {
        SDL_GetWindowSize(app.window, &drawable_width, &drawable_height);
    }
    viewport(0, 0, drawable_width, drawable_height);
    scissor(0, 0, drawable_width, drawable_height);
    enable(GL_SCISSOR_TEST);
    clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    disable(GL_SCISSOR_TEST);
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
    if (port != 0) {
        return 0;
    }
    if (app.headless ||
        app.memory_editor.active || app.memory_editor.goto_popup ||
        app.memory_editor.search_popup ||
        app.memory_editor.name_popup ||
        app.script_import.active || app.video_filter.menu_active) {
        return 0;
    }
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    if (device == RETRO_DEVICE_ANALOG) {
        const int16_t negative = -0x7fff;
        const int16_t positive = 0x7fff;
        if (id == RETRO_DEVICE_ID_ANALOG_X) {
            if (keys[SDL_SCANCODE_A]) return negative;
            if (keys[SDL_SCANCODE_D]) return positive;
        }
        if (id == RETRO_DEVICE_ID_ANALOG_Y) {
            if (keys[SDL_SCANCODE_W]) return negative;
            if (keys[SDL_SCANCODE_S]) return positive;
        }
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }
    if (id < app.lua.buttons.size() && app.lua.buttons[id]) {
        return 1;
    }
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
    auto *memory = static_cast<uint8_t *>(core.get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->load_sram({memory, memory_size});
        std::cout << "Save RAM carregado.\n";
    }
}

void save_sram() {
    const auto *memory =
        static_cast<const uint8_t *>(core.get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->save_sram({memory, memory_size});
    }
}

bool save_current_state() {
    std::vector<uint8_t> state(core.serialize_size());
    if (state.empty() || !core.serialize(state.data(), state.size())) {
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
        !core.unserialize(state.data(), state.size())) {
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

    return init_audio(av);
}

bool init_audio(const retro_system_av_info &av) {
    if (app.audio) {
        return true;
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::cerr << "Falha ao iniciar audio SDL2: " << SDL_GetError() << '\n';
        return false;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(av.timing.sample_rate);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = app.system == ConsoleSystem::N64 ? 2048 : 1024;
    SDL_AudioSpec obtained{};
    app.audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
    app.audio_sample_rate = app.audio ? obtained.freq : wanted.freq;
    if (app.audio) {
        SDL_PauseAudioDevice(app.audio, 1);
    } else {
        std::cerr << "Audio desativado: " << SDL_GetError() << '\n';
    }
    return true;
}

bool init_hardware_sdl() {
    if (app.window && app.gl_context) {
        return true;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "Falha ao iniciar SDL2: " << SDL_GetError() << '\n';
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, app.hardware.depth ? 24 : 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, app.hardware.stencil ? 8 : 0);
    if (app.hardware.context_type == RETRO_HW_CONTEXT_OPENGL_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
    }

    const std::string window_title =
        "SNES C++ - " + rom_path.filename().string();
    auto try_context = [&](unsigned major, unsigned minor) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        if (!app.window) {
            const auto [window_width, window_height] = hardware_window_size();
            app.window = SDL_CreateWindow(
                window_title.c_str(),
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                window_width, window_height,
                SDL_WINDOW_OPENGL);
            if (app.window) {
                enforce_hardware_window_size();
            }
        }
        if (!app.window) {
            return false;
        }
        app.gl_context = SDL_GL_CreateContext(app.window);
        if (app.gl_context) {
            std::cerr << "[opengl] contexto criado: " << major << '.'
                      << minor << '\n';
            return true;
        }
        return false;
    };

    const unsigned requested_major = app.hardware.version_major;
    const unsigned requested_minor = app.hardware.version_minor;
    bool context_created = false;
    if (app.hardware.context_type == RETRO_HW_CONTEXT_OPENGL_CORE) {
        const unsigned major = requested_major ? requested_major : 3U;
        const unsigned minor = requested_major ? requested_minor : 2U;
        if (app.n64_gliden64) {
            context_created = try_context(4, 1);
        }
        if (!context_created) {
            context_created = try_context(std::min(major, 4U),
                                          major > 4U ? 1U : minor);
        }
        if (!context_created) {
            context_created = try_context(4, 1);
        }
        if (!context_created) {
            context_created = try_context(3, 2);
        }
    } else {
        context_created = try_context(requested_major ? requested_major : 2U,
                                      requested_major ? requested_minor : 1U);
    }
    if (!app.window) {
        std::cerr << "Falha ao criar janela OpenGL: " << SDL_GetError() << '\n';
        return false;
    }
    if (!context_created) {
        std::cerr << "Falha ao criar contexto OpenGL: " << SDL_GetError()
                  << '\n';
        return false;
    }
    SDL_GL_MakeCurrent(app.window, app.gl_context);
    SDL_GL_SetSwapInterval(1);
    app.hardware_context_ready = true;
    // Mupen64Plus-Next only connects GLideN64's plugin table from
    // context_reset() after retro_load_game() has marked first_context_reset.
    if (app.hardware.context_reset && !app.n64_gliden64) {
        app.hardware.context_reset();
    }
    return true;
}

} // namespace snes::frontend
