#include "frontend/runtime_context.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>

#include "debug_font.h"

namespace snes::frontend {

void draw_text(int x, int y, const std::string &text, SDL_Color color,
               int scale) {
  snes::draw_debug_text(app.renderer, x, y, text, color, scale);
}

bool window_fullscreen() {
  return app.window &&
         (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP);
}

int render_logical_width() {
  return app.memory_debug && !window_fullscreen() ? game_width + debugger_width
                                                  : game_width;
}

void apply_stretched_render_scale() {
  if (!app.renderer) {
    return;
  }
  int output_width = game_width;
  int output_height = game_height;
  SDL_GetRendererOutputSize(app.renderer, &output_width, &output_height);
  const int logical_width = std::max(1, render_logical_width());
  SDL_RenderSetLogicalSize(app.renderer, 0, 0);
  SDL_RenderSetViewport(app.renderer, nullptr);
  SDL_RenderSetScale(
      app.renderer,
      static_cast<float>(output_width) / static_cast<float>(logical_width),
      static_cast<float>(output_height) / static_cast<float>(game_height));
}

int effective_speed_multiplier() {
  const int frontend_speed = app.turbo ? app.turbo_multiplier : 1;
  return std::clamp(std::max(frontend_speed, app.lua.speed_multiplier), 1, 64);
}

uint32_t memory_hash(std::span<const uint8_t> memory) {
  uint32_t hash = 2166136261U;
  for (uint8_t byte : memory) {
    hash = (hash ^ byte) * 16777619U;
  }
  return hash;
}

std::array<MemoryRegion, 3> memory_regions{{
    {"WRAM", RETRO_MEMORY_SYSTEM_RAM, 0x7E0000, "CPU 7E0000-7FFFFF"},
    {"VRAM", RETRO_MEMORY_VIDEO_RAM, 0x000000, "PPU 0000-FFFF"},
    {"SRAM", RETRO_MEMORY_SAVE_RAM, 0x700000, "CPU 700000+"},
}};

void configure_memory_regions(ConsoleSystem system) {
  if (system == ConsoleSystem::N64) {
    memory_regions = {{
        {"RDRAM", RETRO_MEMORY_SYSTEM_RAM, 0x80000000U, "CPU 80000000+"},
        {"VRAM", RETRO_MEMORY_VIDEO_RAM, 0x00000000U, "VIDEO 00000000+"},
        {"SAVE", RETRO_MEMORY_SAVE_RAM, 0x00000000U, "SAVE 00000000+"},
    }};
    app.memory_editor.address_input = memory_regions[0].base;
    return;
  }
  memory_regions = {{
      {"WRAM", RETRO_MEMORY_SYSTEM_RAM, 0x7E0000, "CPU 7E0000-7FFFFF"},
      {"VRAM", RETRO_MEMORY_VIDEO_RAM, 0x000000, "PPU 0000-FFFF"},
      {"SRAM", RETRO_MEMORY_SAVE_RAM, 0x700000, "CPU 700000+"},
  }};
  app.memory_editor.address_input = memory_regions[0].base;
}

uint8_t *wram() {
  return static_cast<uint8_t *>(core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

const MemoryRegion &selected_region() {
  return memory_regions[app.memory_editor.region];
}

uint8_t *selected_memory() {
  return static_cast<uint8_t *>(core.get_memory_data(selected_region().id));
}

size_t selected_memory_size() {
  return core.get_memory_size(selected_region().id);
}

bool checked_multiply(size_t left, size_t right, size_t &result) {
  if (left && right > std::numeric_limits<size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool frame_layout(unsigned width, unsigned height, size_t pitch,
                  size_t &row_bytes, size_t &frame_size) {
  const size_t bytes_per_pixel =
      app.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4U : 2U;
  if (!width || !height ||
      !checked_multiply(static_cast<size_t>(width), bytes_per_pixel,
                        row_bytes) ||
      pitch < row_bytes ||
      !checked_multiply(row_bytes, static_cast<size_t>(height), frame_size)) {
    return false;
  }
  return true;
}

bool resolve_memory_address(uint32_t address, unsigned &region_index,
                            size_t &offset) {
  if (app.system == ConsoleSystem::N64) {
    const size_t system_size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const uint32_t physical = address & 0x1fffffffU;
    if (system_size && ((address >= 0x80000000U && physical < system_size) ||
                        address < system_size)) {
      region_index = 0;
      offset = address < system_size ? address : physical;
      return true;
    }
  }
  for (unsigned index = 0; index < memory_regions.size(); ++index) {
    const auto &region = memory_regions[index];
    const size_t size = core.get_memory_size(region.id);
    if (size && address >= region.base &&
        static_cast<uint64_t>(address - region.base) < size) {
      region_index = index;
      offset = address - region.base;
      return true;
    }
  }
  return false;
}

std::string trim(std::string text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool parse_watch_address(std::string text, uint32_t &address) {
  text = trim(text);
  if (text.empty()) {
    return false;
  }
  if (text.front() == '$') {
    text.erase(text.begin());
  } else if (text.size() > 2 && text[0] == '0' &&
             (text[1] == 'x' || text[1] == 'X')) {
    text.erase(0, 2);
  }
  char *end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 16);
  if (!end || *end != '\0' || value > 0xFFFFFFUL) {
    return false;
  }
  address = static_cast<uint32_t>(value);
  return true;
}

void load_memory_watchlist(const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }
  std::ifstream file(path);
  if (!file) {
    std::cerr << "Nao foi possivel abrir watchlist: " << path << '\n';
    return;
  }

  std::string line;
  unsigned line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    if (const auto comment = line.find('#'); comment != std::string::npos) {
      line.resize(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::istringstream stream(line);
    std::string address_text;
    stream >> address_text;
    std::string label;
    std::getline(stream, label);
    label = trim(label);

    uint32_t address = 0;
    unsigned region = 0;
    size_t offset = 0;
    if (!parse_watch_address(address_text, address) ||
        !resolve_memory_address(address, region, offset)) {
      std::cerr << "Watchlist ignorou linha " << line_number << ": " << line
                << '\n';
      continue;
    }
    if (label.empty()) {
      char fallback[32];
      std::snprintf(fallback, sizeof(fallback), "$%06X", address);
      label = fallback;
    }
    app.custom_memory_watches.push_back(
        CustomMemoryWatch{address, region, offset, label});
  }
  if (!app.custom_memory_watches.empty()) {
    std::cout << "Watchlist carregada: " << app.custom_memory_watches.size()
              << " memorias.\n";
  }
}

bool focus_memory_address(uint32_t address) {
  unsigned region_index = 0;
  size_t offset = 0;
  if (!resolve_memory_address(address, region_index, offset)) {
    app.memory_editor.status = "ENDERECO FORA DAS REGIOES";
    return false;
  }

  auto &editor = app.memory_editor;
  editor.region = region_index;
  editor.offset = offset;
  editor.address_input = address;
  editor.address_valid = true;
  if (auto *memory = selected_memory()) {
    editor.value = memory[offset];
    editor.goto_value = memory[offset];
  }

  if (memory_regions[region_index].id == RETRO_MEMORY_SYSTEM_RAM) {
    editor.focused_activity_offset = offset;
    editor.focused_activity = true;
    editor.focused_screen_region = {};
    editor.focused_region_frozen = false;
    if (offset < app.memory_activity.screen_regions.size()) {
      const SDL_Rect known = app.memory_activity.screen_regions[offset];
      if (known.w > 0 && known.h > 0) {
        editor.focused_screen_region = known;
        editor.focused_region_frozen = true;
      }
    }
  } else {
    editor.focused_activity = false;
    editor.focused_region_frozen = false;
  }
  editor.status = "ENDERECO EM ACOMPANHAMENTO";
  return true;
}

bool write_memory_address(uint32_t address, uint8_t value);
bool save_current_state();
bool load_current_state();

} // namespace snes::frontend
