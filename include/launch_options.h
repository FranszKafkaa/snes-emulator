#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace snes {

struct LaunchOptions {
    std::filesystem::path rom_path{"mario.sfc"};
    std::filesystem::path core_path{};
    std::filesystem::path watchlist_path{};
    std::filesystem::path script_path{};
    bool headless = false;
    uint64_t frame_limit = 0;
};

std::optional<LaunchOptions> parse_launch_options(int argc, char **argv);
void print_usage(const char *program);

} // namespace snes
