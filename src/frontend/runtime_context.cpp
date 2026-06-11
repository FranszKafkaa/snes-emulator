#include "frontend/runtime_context.h"

namespace snes::frontend {

Frontend app;
LibretroCore core;
std::vector<uint8_t> rom;
std::filesystem::path rom_path;
std::unique_ptr<snes::SaveManager> save_manager;

} // namespace snes::frontend
