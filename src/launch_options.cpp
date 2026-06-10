#include "launch_options.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace snes {

void print_usage(const char *program) {
    std::cout << "Uso: " << program
              << " [rom.sfc] [--headless] [--frames N]"
                 " [--watchlist arquivo.txt] [--script arquivo.lua]\n";
}

std::optional<LaunchOptions> parse_launch_options(int argc, char **argv) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--frames" && index + 1 < argc) {
            options.frame_limit = std::strtoull(argv[++index], nullptr, 10);
        } else if ((argument == "--watchlist" ||
                    argument == "--memories") &&
                   index + 1 < argc) {
            options.watchlist_path = argv[++index];
        } else if (argument == "--script" && index + 1 < argc) {
            options.script_path = argv[++index];
        } else if (argument == "--help") {
            print_usage(argv[0]);
            return std::nullopt;
        } else if (!argument.starts_with("--")) {
            options.rom_path = argument;
        } else {
            print_usage(argv[0]);
            return std::nullopt;
        }
    }
    if (options.headless && options.frame_limit == 0) {
        options.frame_limit = 300;
    }
    return options;
}

} // namespace snes
