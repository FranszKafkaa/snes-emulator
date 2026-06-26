#pragma once

#include <filesystem>
#include <string>

#include "libretro.h"

namespace snes::frontend {

enum class ConsoleSystem {
    Unknown,
    Snes,
    N64,
};

struct CoreSelection {
    ConsoleSystem system = ConsoleSystem::Unknown;
    std::filesystem::path path;
};

class LibretroCore {
public:
    LibretroCore() = default;
    LibretroCore(const LibretroCore &) = delete;
    LibretroCore &operator=(const LibretroCore &) = delete;
    ~LibretroCore();

    bool load(const std::filesystem::path &path);
    void close();
    bool loaded() const { return handle_ != nullptr; }
    const std::filesystem::path &path() const { return path_; }
    void *symbol(const char *name) const;

    void set_environment(retro_environment_t cb) const;
    void set_video_refresh(retro_video_refresh_t cb) const;
    void set_audio_sample(retro_audio_sample_t cb) const;
    void set_audio_sample_batch(retro_audio_sample_batch_t cb) const;
    void set_input_poll(retro_input_poll_t cb) const;
    void set_input_state(retro_input_state_t cb) const;
    void init() const;
    void deinit() const;
    unsigned api_version() const;
    void get_system_info(retro_system_info *info) const;
    void get_system_av_info(retro_system_av_info *info) const;
    void set_controller_port_device(unsigned port, unsigned device) const;
    void reset() const;
    void run() const;
    size_t serialize_size() const;
    bool serialize(void *data, size_t size) const;
    bool unserialize(const void *data, size_t size) const;
    void *get_memory_data(unsigned id) const;
    size_t get_memory_size(unsigned id) const;
    bool load_game(const retro_game_info *game) const;
    void unload_game() const;

private:
    template <typename T>
    bool bind(T &slot, const char *name);

    void *handle_ = nullptr;
    std::filesystem::path path_;

    void (*set_environment_)(retro_environment_t) = nullptr;
    void (*set_video_refresh_)(retro_video_refresh_t) = nullptr;
    void (*set_audio_sample_)(retro_audio_sample_t) = nullptr;
    void (*set_audio_sample_batch_)(retro_audio_sample_batch_t) = nullptr;
    void (*set_input_poll_)(retro_input_poll_t) = nullptr;
    void (*set_input_state_)(retro_input_state_t) = nullptr;
    void (*init_)() = nullptr;
    void (*deinit_)() = nullptr;
    unsigned (*api_version_)() = nullptr;
    void (*get_system_info_)(retro_system_info *) = nullptr;
    void (*get_system_av_info_)(retro_system_av_info *) = nullptr;
    void (*set_controller_port_device_)(unsigned, unsigned) = nullptr;
    void (*reset_)() = nullptr;
    void (*run_)() = nullptr;
    size_t (*serialize_size_)() = nullptr;
    bool (*serialize_)(void *, size_t) = nullptr;
    bool (*unserialize_)(const void *, size_t) = nullptr;
    void *(*get_memory_data_)(unsigned) = nullptr;
    size_t (*get_memory_size_)(unsigned) = nullptr;
    bool (*load_game_)(const retro_game_info *) = nullptr;
    void (*unload_game_)() = nullptr;
};

ConsoleSystem detect_console_system(const std::filesystem::path &rom_path);
const char *console_system_name(ConsoleSystem system);
CoreSelection select_core(const std::filesystem::path &rom_path,
                          const std::filesystem::path &requested_core);

} // namespace snes::frontend
