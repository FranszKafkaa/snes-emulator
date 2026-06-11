#include "emulator/libretro_core.h"

#include <algorithm>
#include <cctype>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <vector>

namespace snes::frontend {

namespace {

std::string lower_extension(const std::filesystem::path &path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return extension;
}

std::filesystem::path first_existing_path(
    const std::vector<std::filesystem::path> &paths) {
    for (const auto &path : paths) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return paths.empty() ? std::filesystem::path{} : paths.front();
}

} // namespace

LibretroCore::~LibretroCore() {
    close();
}

template <typename T>
bool LibretroCore::bind(T &slot, const char *name) {
    slot = reinterpret_cast<T>(dlsym(handle_, name));
    if (!slot) {
        std::cerr << "Core invalido, simbolo ausente: " << name << '\n';
        return false;
    }
    return true;
}

bool LibretroCore::load(const std::filesystem::path &path) {
    close();
    path_ = path;
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        std::cerr << "Nao foi possivel carregar core " << path << ": "
                  << dlerror() << '\n';
        return false;
    }

    const bool ok =
        bind(set_environment_, "retro_set_environment") &&
        bind(set_video_refresh_, "retro_set_video_refresh") &&
        bind(set_audio_sample_, "retro_set_audio_sample") &&
        bind(set_audio_sample_batch_, "retro_set_audio_sample_batch") &&
        bind(set_input_poll_, "retro_set_input_poll") &&
        bind(set_input_state_, "retro_set_input_state") &&
        bind(init_, "retro_init") &&
        bind(deinit_, "retro_deinit") &&
        bind(api_version_, "retro_api_version") &&
        bind(get_system_info_, "retro_get_system_info") &&
        bind(get_system_av_info_, "retro_get_system_av_info") &&
        bind(set_controller_port_device_, "retro_set_controller_port_device") &&
        bind(reset_, "retro_reset") &&
        bind(run_, "retro_run") &&
        bind(serialize_size_, "retro_serialize_size") &&
        bind(serialize_, "retro_serialize") &&
        bind(unserialize_, "retro_unserialize") &&
        bind(get_memory_data_, "retro_get_memory_data") &&
        bind(get_memory_size_, "retro_get_memory_size") &&
        bind(load_game_, "retro_load_game") &&
        bind(unload_game_, "retro_unload_game");
    if (!ok) {
        close();
        return false;
    }
    return true;
}

void LibretroCore::close() {
    if (handle_) {
        dlclose(handle_);
    }
    handle_ = nullptr;
}

void *LibretroCore::symbol(const char *name) const {
    return handle_ ? dlsym(handle_, name) : nullptr;
}

void LibretroCore::set_environment(retro_environment_t cb) const { set_environment_(cb); }
void LibretroCore::set_video_refresh(retro_video_refresh_t cb) const { set_video_refresh_(cb); }
void LibretroCore::set_audio_sample(retro_audio_sample_t cb) const { set_audio_sample_(cb); }
void LibretroCore::set_audio_sample_batch(retro_audio_sample_batch_t cb) const { set_audio_sample_batch_(cb); }
void LibretroCore::set_input_poll(retro_input_poll_t cb) const { set_input_poll_(cb); }
void LibretroCore::set_input_state(retro_input_state_t cb) const { set_input_state_(cb); }
void LibretroCore::init() const { init_(); }
void LibretroCore::deinit() const { deinit_(); }
unsigned LibretroCore::api_version() const { return api_version_(); }
void LibretroCore::get_system_info(retro_system_info *info) const { get_system_info_(info); }
void LibretroCore::get_system_av_info(retro_system_av_info *info) const { get_system_av_info_(info); }
void LibretroCore::set_controller_port_device(unsigned port, unsigned device) const { set_controller_port_device_(port, device); }
void LibretroCore::reset() const { reset_(); }
void LibretroCore::run() const { run_(); }
size_t LibretroCore::serialize_size() const { return serialize_size_(); }
bool LibretroCore::serialize(void *data, size_t size) const { return serialize_(data, size); }
bool LibretroCore::unserialize(const void *data, size_t size) const { return unserialize_(data, size); }
void *LibretroCore::get_memory_data(unsigned id) const { return get_memory_data_(id); }
size_t LibretroCore::get_memory_size(unsigned id) const { return get_memory_size_(id); }
bool LibretroCore::load_game(const retro_game_info *game) const { return load_game_(game); }
void LibretroCore::unload_game() const { unload_game_(); }

ConsoleSystem detect_console_system(const std::filesystem::path &rom_path) {
    const std::string extension = lower_extension(rom_path);
    if (extension == ".sfc" || extension == ".smc" || extension == ".fig" ||
        extension == ".swc") {
        return ConsoleSystem::Snes;
    }
    if (extension == ".n64" || extension == ".z64" || extension == ".v64" ||
        extension == ".ndd") {
        return ConsoleSystem::N64;
    }
    return ConsoleSystem::Unknown;
}

const char *console_system_name(ConsoleSystem system) {
    switch (system) {
    case ConsoleSystem::Snes: return "SNES";
    case ConsoleSystem::N64: return "Nintendo 64";
    default: return "desconhecido";
    }
}

CoreSelection select_core(const std::filesystem::path &rom_path,
                          const std::filesystem::path &requested_core) {
    CoreSelection selection;
    selection.system = detect_console_system(rom_path);
    if (!requested_core.empty()) {
        selection.path = requested_core;
        return selection;
    }
    switch (selection.system) {
    case ConsoleSystem::N64:
        selection.path = first_existing_path({
            "lib/mupen64plus_next_libretro.dylib",
            "lib/parallel_n64_libretro.dylib",
        });
        break;
    case ConsoleSystem::Snes:
    case ConsoleSystem::Unknown:
        selection.path = "lib/snes9x_libretro.dylib";
        break;
    }
    return selection;
}

} // namespace snes::frontend
