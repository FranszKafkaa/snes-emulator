#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace snes {

class SaveManager {
public:
    explicit SaveManager(std::filesystem::path rom_path);

    bool read_rom(std::vector<uint8_t> &output) const;
    void load_sram(std::span<uint8_t> memory) const;
    void save_sram(std::span<const uint8_t> memory) const;
    bool save_state(std::span<const uint8_t> state) const;
    bool load_state(std::vector<uint8_t> &state) const;

private:
    bool read_file(const std::filesystem::path &path,
                   std::vector<uint8_t> &output) const;
    std::filesystem::path sidecar_path(const char *extension) const;

    std::filesystem::path rom_path_;
};

} // namespace snes
