#include "save_manager.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

namespace snes {

SaveManager::SaveManager(std::filesystem::path rom_path)
    : rom_path_(std::move(rom_path)) {}

bool SaveManager::read_file(const std::filesystem::path &path,
                            std::vector<uint8_t> &output) const {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size <= 0 ||
        static_cast<uintmax_t>(size) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    output.resize(static_cast<size_t>(size));
    file.seekg(0);
    return static_cast<bool>(
        file.read(reinterpret_cast<char *>(output.data()), size));
}

std::filesystem::path SaveManager::sidecar_path(const char *extension) const {
    auto path = rom_path_;
    path.replace_extension(extension);
    return path;
}

bool SaveManager::read_rom(std::vector<uint8_t> &output) const {
    return read_file(rom_path_, output);
}

void SaveManager::load_sram(std::span<uint8_t> memory) const {
    std::vector<uint8_t> save;
    if (memory.empty() || !read_file(sidecar_path(".srm"), save)) return;
    std::copy_n(save.data(), std::min(memory.size(), save.size()),
                memory.data());
}

void SaveManager::save_sram(std::span<const uint8_t> memory) const {
    if (memory.empty()) return;
    if (memory.size() >
        static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return;
    }
    std::ofstream file(sidecar_path(".srm"), std::ios::binary);
    file.write(reinterpret_cast<const char *>(memory.data()),
               static_cast<std::streamsize>(memory.size()));
}

bool SaveManager::save_state(std::span<const uint8_t> state) const {
    if (state.empty()) return false;
    if (state.size() >
        static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    std::ofstream file(sidecar_path(".state"), std::ios::binary);
    file.write(reinterpret_cast<const char *>(state.data()),
               static_cast<std::streamsize>(state.size()));
    return static_cast<bool>(file);
}

bool SaveManager::load_state(std::vector<uint8_t> &state) const {
    return read_file(sidecar_path(".state"), state);
}

} // namespace snes
