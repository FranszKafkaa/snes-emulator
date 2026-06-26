#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/ucontext.h>

#include "application.h"
#include "frontend/runtime_context.h"
#include "launch_options.h"
#include "libretro.h"

namespace {

void crash_handler(int signal, siginfo_t *, void *context) {
    void *frames[64];
    const int frame_count = backtrace(frames, 64);
    std::cerr << "\nCrash capturado: sinal " << signal
              << ". Backtrace:\n";
#if defined(__APPLE__) && defined(__aarch64__)
    if (context) {
        const auto *ucontext = static_cast<ucontext_t *>(context);
        const auto &state = ucontext->uc_mcontext->__ss;
        std::cerr << "pc=0x" << std::hex << state.__pc
                  << " lr=0x" << state.__lr
                  << " x8=0x" << state.__x[8]
                  << " x22=0x" << state.__x[22]
                  << std::dec << '\n';
    }
#endif
    backtrace_symbols_fd(frames, frame_count, STDERR_FILENO);
    std::_Exit(128 + signal);
}

void install_crash_handler() {
    struct sigaction action {};
    action.sa_sigaction = crash_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
}

} // namespace

int snes::Application::run(int argc, char **argv) {
    using namespace snes::frontend;

    install_crash_handler();

    const auto options = parse_launch_options(argc, argv);
    if (!options) return 0;

    const uint64_t frame_limit = options->frame_limit;
    rom_path = options->rom_path;
    const CoreSelection core_selection =
        select_core(rom_path, options->core_path);
    app.system = core_selection.system;
    configure_memory_regions(app.system);
    if (!std::filesystem::exists(core_selection.path)) {
        std::cerr << "Core nao encontrado para "
                  << console_system_name(app.system) << ": "
                  << core_selection.path << '\n';
        if (app.system == ConsoleSystem::N64) {
            std::cerr << "Coloque um core N64 libretro em "
                         "lib/mupen64plus_next_libretro.dylib ou passe "
                         "--core caminho/do/core.dylib.\n";
        }
        return 1;
    }
    if (!core.load(core_selection.path)) {
        return 1;
    }

    app.script_editor.path = options->script_path.empty()
        ? std::filesystem::path{"scripts/novo-script.lua"}
        : options->script_path;
    app.headless = options->headless;
    save_manager = std::make_unique<SaveManager>(rom_path);
    const bool n64_hardware_allowed =
        app.system == ConsoleSystem::N64 && !app.headless &&
        !options->n64_accurate;
    app.n64_fast = n64_hardware_allowed && options->n64_fast;
    app.n64_fullspeed =
        app.system == ConsoleSystem::N64 && options->n64_fullspeed;
    app.n64_gliden64 =
        n64_hardware_allowed &&
        (options->n64_gliden64 || options->n64_widescreen || options->n64_fast);
    app.n64_widescreen = n64_hardware_allowed && options->n64_widescreen;
    app.data_directory =
        std::filesystem::absolute(rom_path).parent_path().string();
    app.content_directory = app.data_directory;
    app.core_assets_directory =
        std::filesystem::absolute(core_selection.path).parent_path().string();
    app.system_directory = app.system == ConsoleSystem::N64
        ? app.core_assets_directory
        : app.data_directory;

    core.set_environment(environment);
    core.set_video_refresh(video_refresh);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.init();

    if (app.hardware_render) {
        if (app.headless) {
            std::cerr << "Este core precisa de OpenGL; execute sem "
                         "--headless para jogar.\n";
            core.deinit();
            return 1;
        }
        if (!init_hardware_sdl()) {
            core.deinit();
            SDL_Quit();
            return 1;
        }
    }

    retro_system_info system{};
    core.get_system_info(&system);
    if (!system.need_fullpath && !save_manager->read_rom(rom)) {
        std::cerr << "Nao foi possivel ler a ROM: " << rom_path << '\n';
        core.deinit();
        return 1;
    }
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    if (app.system == ConsoleSystem::N64) {
        for (unsigned port = 1; port < 4; ++port) {
            core.set_controller_port_device(port, RETRO_DEVICE_NONE);
        }
    }

    const std::string rom_path_string = rom_path.string();
    retro_game_info game{
        rom_path_string.c_str(),
        rom.empty() ? nullptr : rom.data(),
        rom.size(),
        nullptr};
    if (!core.load_game(&game)) {
        std::cerr << "O core recusou a ROM.\n";
        core.deinit();
        return 1;
    }
    // GLideN64 must see context_reset() after retro_load_game(), otherwise
    // Mupen64Plus-Next leaves gfx.PluginStartup null and crashes in main_run.
    if (app.n64_gliden64 && app.hardware.context_reset &&
        app.hardware_context_ready) {
        app.hardware.context_reset();
    }
    load_sram();
    load_memory_watchlist(options->watchlist_path);
    load_lua_script(options->script_path);
    update_script_editor_timestamp();

    retro_system_av_info av{};
    core.get_system_av_info(&av);
    std::cout << system.library_name << ' ' << system.library_version
              << " | " << console_system_name(app.system)
              << " | " << av.timing.fps << " FPS | "
              << av.timing.sample_rate << " Hz\n";

    if (app.hardware_render && !init_audio(av)) {
        core.unload_game();
        core.deinit();
        if (app.hardware.context_destroy) {
            app.hardware.context_destroy();
        }
        SDL_Quit();
        return 1;
    }
    if (!app.headless && !app.hardware_render && !init_sdl(av)) {
        core.unload_game();
        core.deinit();
        SDL_Quit();
        return 1;
    }
    if (!app.headless) {
        start_media_workers();
    }

    const auto frame_duration =
        std::chrono::duration<double>(1.0 / av.timing.fps);
    auto next_frame = std::chrono::steady_clock::now();
    unsigned audio_refill_frames = 0;
    uint64_t frames = 0;
    while (app.running && (!frame_limit || frames < frame_limit)) {
        if (!app.headless) {
            handle_events();
        }
        if (!app.paused) {
            reload_script_if_changed();
            const int speed = effective_speed_multiplier();
            uint64_t frame_budget =
                static_cast<uint64_t>(speed);
            if (frame_limit) {
                frame_budget = std::min(frame_budget, frame_limit - frames);
            }
            for (uint64_t step = 0; step < frame_budget; ++step) {
                run_lua_frame();
                apply_memory_lock();
                if (app.hardware_render && app.gl_context) {
                    prepare_hardware_frame();
                }
                core.run();
                evaluate_memory_triggers();
                ++frames;
                ++app.lua.frame;
            }
        }
        if (!app.headless) {
            present_latest_frame();
            if (effective_speed_multiplier() <= 1) {
                const bool audio_needs_buffer =
                    app.audio_pipeline &&
                    app.audio_pipeline->queued_milliseconds() < 120;
                if (audio_needs_buffer && audio_refill_frames < 12) {
                    ++audio_refill_frames;
                    next_frame = std::chrono::steady_clock::now();
                    continue;
                }
                if (!audio_needs_buffer) {
                    audio_refill_frames = 0;
                }
                const auto now = std::chrono::steady_clock::now();
                if (next_frame + frame_duration < now) {
                    next_frame = now;
                }
                next_frame +=
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        frame_duration);
                std::this_thread::sleep_until(next_frame);
            } else {
                next_frame = std::chrono::steady_clock::now();
            }
        }
    }

    if (!app.headless) {
        stop_media_workers();
        present_latest_frame();
        app.video_pipeline.reset();
        app.audio_pipeline.reset();
    }
    save_sram();
    if (app.lua.state) {
        lua_close(app.lua.state);
        app.lua.state = nullptr;
        app.lua.active = false;
    }
    if (app.hardware_render && app.gl_context) {
        SDL_GL_MakeCurrent(app.window, app.gl_context);
    }
    if (app.hardware.context_destroy && app.hardware_context_ready) {
        app.hardware.context_destroy();
        app.hardware_context_ready = false;
    }
    const bool skip_hardware_unload =
        app.system == ConsoleSystem::N64 && app.hardware_render;
    if (skip_hardware_unload) {
        std::cerr << "[core] pulando retro_unload_game no N64/OpenGL "
                     "para evitar crash no shutdown do plugin.\n";
    } else {
        core.unload_game();
    }
    core.deinit();
    if (app.audio) SDL_CloseAudioDevice(app.audio);
    SDL_DestroyTexture(app.texture);
    SDL_DestroyRenderer(app.renderer);
    if (app.gl_context) {
        SDL_GL_DeleteContext(app.gl_context);
    }
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    if (skip_hardware_unload) {
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }

    if (app.headless) {
        std::cout << "Teste concluido: " << frames
                  << " frames, hash de video 0x" << std::hex
                  << app.frame_hash << std::dec << '\n';
    }
    return 0;
}
