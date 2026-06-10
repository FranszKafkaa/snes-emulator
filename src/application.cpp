#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <deque>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "application.h"
#include "debug_font.h"
#include "launch_options.h"
#include "libretro.h"
#include "media_pipeline.h"
#include "save_manager.h"

namespace {

constexpr int game_width = 1024;
constexpr int game_height = 768;
constexpr int debugger_width = 512;

struct MemoryEditor {
    unsigned region = 0;
    unsigned important_index = 0;
    size_t focused_activity_offset = 0;
    SDL_Rect focused_screen_region{};
    bool focused_activity = false;
    bool focused_region_frozen = false;
    size_t offset = 0;
    uint32_t address_input = 0x7E0000;
    uint8_t value = 0;
    bool active = false;
    bool editing_value = false;
    bool text_mode = false;
    bool replace_on_type = true;
    bool address_valid = true;
    bool watch_active = false;
    bool watch_locked = false;
    unsigned watch_region = 0;
    size_t watch_offset = 0;
    uint8_t watch_value = 0;
    bool goto_popup = false;
    bool goto_editing_value = false;
    bool goto_replace_on_type = true;
    bool goto_has_address = false;
    uint32_t goto_address = 0x7E0000;
    uint8_t goto_value = 0;
    std::string status = "E PARA EDITAR";
};

struct MemoryActivity {
    struct VisualCorrelation {
        float center_x = 0.0f;
        float center_y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float movement = 0.0f;
        uint16_t evidence = 0;
        uint8_t value_axis = 0;
    };

    std::vector<uint8_t> previous_wram;
    std::vector<uint32_t> scores;
    std::vector<SDL_Rect> screen_regions;
    std::vector<VisualCorrelation> correlations;
    std::vector<uint8_t> previous_frame;
    std::array<size_t, 10> hottest{};
    size_t player_candidate_offset = 0;
    uint32_t player_candidate_score = 0;
    unsigned frame_width = 0;
    unsigned frame_height = 0;
    uint64_t frames = 0;
};

using bool8 = uint8_t;

struct CoreSOBJ {
    int16_t HPos;
    uint16_t VPos;
    uint8_t HFlip;
    uint8_t VFlip;
    uint16_t Name;
    uint8_t Priority;
    uint8_t Palette;
    uint8_t Size;
};

struct CoreSPPU {
    struct {
        bool8 High;
        uint8_t Increment;
        uint16_t Address;
        uint16_t Mask1;
        uint16_t FullGraphicCount;
        uint16_t Shift;
    } VMA;
    uint32_t WRAM;
    struct {
        uint16_t SCBase;
        uint16_t HOffset;
        uint16_t VOffset;
        uint8_t BGSize;
        uint16_t NameBase;
        uint16_t SCSize;
    } BG[4];
    uint8_t BGMode;
    uint8_t BG3Priority;
    bool8 CGFLIP;
    uint8_t CGFLIPRead;
    uint8_t CGADD;
    uint8_t CGSavedByte;
    uint16_t CGDATA[256];
    CoreSOBJ OBJ[128];
    bool8 OBJThroughMain;
    bool8 OBJThroughSub;
    bool8 OBJAddition;
    uint16_t OBJNameBase;
    uint16_t OBJNameSelect;
    uint8_t OBJSizeSelect;
    uint16_t OAMAddr;
    uint16_t SavedOAMAddr;
    uint8_t OAMPriorityRotation;
    uint8_t OAMFlip;
    uint8_t OAMReadFlip;
    uint16_t OAMTileAddress;
    uint16_t OAMWriteRegister;
    uint8_t OAMData[512 + 32];
};

struct CustomMemoryWatch {
    uint32_t address = 0;
    unsigned region = 0;
    size_t offset = 0;
    std::string label;
};

struct LuaRuntime {
    lua_State *state = nullptr;
    std::array<bool, 16> buttons{};
    uint64_t frame = 0;
    bool active = false;
};

struct LuaScriptEditor {
    std::filesystem::path path{"scripts/novo-script.lua"};
    std::vector<std::string> lines{"function on_frame(frame)", "    snes.clear_input()", "", "end"};
    size_t row = 0;
    size_t col = 0;
    size_t row_offset = 0;
    size_t col_offset = 0;
    bool active = false;
    bool dirty = false;
    bool restore_pause = false;
    std::string status = "CTRL-S SALVA  CTRL-R RECARREGA  TAB COMPLETA  ESC FECHA";
};

struct Frontend {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_AudioDeviceID audio = 0;
    unsigned texture_width = 0;
    unsigned texture_height = 0;
    enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_RGB565;
    std::string data_directory = ".";
    bool headless = false;
    bool running = true;
    bool paused = false;
    bool memory_debug = false;
    bool debug_overlay = false;
    MemoryEditor memory_editor;
    MemoryActivity memory_activity;
    std::vector<CustomMemoryWatch> custom_memory_watches;
    LuaRuntime lua;
    LuaScriptEditor lua_editor;
    std::unique_ptr<snes::VideoPipeline> video_pipeline;
    std::unique_ptr<snes::AudioPipeline> audio_pipeline;
    uint64_t presented_serial = 0;
    uint64_t frame_hash = 1469598103934665603ULL;
};

Frontend app;
std::vector<uint8_t> rom;
std::filesystem::path rom_path;
std::unique_ptr<snes::SaveManager> save_manager;

void draw_text(int x, int y, const std::string &text, SDL_Color color,
               int scale = 2) {
    snes::draw_debug_text(app.renderer, x, y, text, color, scale);
}

uint32_t memory_hash(std::span<const uint8_t> memory) {
    uint32_t hash = 2166136261U;
    for (uint8_t byte : memory) {
        hash = (hash ^ byte) * 16777619U;
    }
    return hash;
}

struct MemoryRegion {
    const char *name;
    unsigned id;
    uint32_t base;
    const char *map;
};

constexpr std::array<MemoryRegion, 3> memory_regions{{
    {"WRAM", RETRO_MEMORY_SYSTEM_RAM, 0x7E0000, "CPU 7E0000-7FFFFF"},
    {"VRAM", RETRO_MEMORY_VIDEO_RAM, 0x000000, "PPU 0000-FFFF"},
    {"SRAM", RETRO_MEMORY_SAVE_RAM, 0x700000, "CPU 700000+"},
}};

uint8_t *wram() {
    return static_cast<uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
}

const MemoryRegion &selected_region() {
    return memory_regions[app.memory_editor.region];
}

uint8_t *selected_memory() {
    return static_cast<uint8_t *>(
        retro_get_memory_data(selected_region().id));
}

size_t selected_memory_size() {
    return retro_get_memory_size(selected_region().id);
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
        !checked_multiply(row_bytes, static_cast<size_t>(height),
                          frame_size)) {
        return false;
    }
    return true;
}

bool resolve_memory_address(uint32_t address, unsigned &region_index,
                            size_t &offset) {
    for (unsigned index = 0; index < memory_regions.size(); ++index) {
        const auto &region = memory_regions[index];
        const size_t size = retro_get_memory_size(region.id);
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
            std::cerr << "Watchlist ignorou linha " << line_number
                      << ": " << line << '\n';
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
        std::cout << "Watchlist carregada: "
                  << app.custom_memory_watches.size() << " memorias.\n";
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

int script_button_id(const std::string &name) {
    if (name == "b") return RETRO_DEVICE_ID_JOYPAD_B;
    if (name == "y") return RETRO_DEVICE_ID_JOYPAD_Y;
    if (name == "select") return RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (name == "start") return RETRO_DEVICE_ID_JOYPAD_START;
    if (name == "up") return RETRO_DEVICE_ID_JOYPAD_UP;
    if (name == "down") return RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (name == "left") return RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (name == "right") return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (name == "a") return RETRO_DEVICE_ID_JOYPAD_A;
    if (name == "x") return RETRO_DEVICE_ID_JOYPAD_X;
    if (name == "l") return RETRO_DEVICE_ID_JOYPAD_L;
    if (name == "r") return RETRO_DEVICE_ID_JOYPAD_R;
    return -1;
}

int lua_read8(lua_State *state) {
    const uint32_t address =
        static_cast<uint32_t>(luaL_checkinteger(state, 1));
    unsigned region = 0;
    size_t offset = 0;
    if (!resolve_memory_address(address, region, offset)) {
        lua_pushnil(state);
        return 1;
    }
    const auto *memory = static_cast<const uint8_t *>(
        retro_get_memory_data(memory_regions[region].id));
    const size_t size = retro_get_memory_size(memory_regions[region].id);
    if (!memory || offset >= size) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushinteger(state, memory[offset]);
    return 1;
}

int lua_write8(lua_State *state) {
    const uint32_t address =
        static_cast<uint32_t>(luaL_checkinteger(state, 1));
    const uint8_t value =
        static_cast<uint8_t>(luaL_checkinteger(state, 2) & 0xff);
    lua_pushboolean(state, write_memory_address(address, value));
    return 1;
}

int lua_read16(lua_State *state) {
    const uint32_t address =
        static_cast<uint32_t>(luaL_checkinteger(state, 1));
    unsigned region = 0;
    size_t offset = 0;
    if (!resolve_memory_address(address, region, offset)) {
        lua_pushnil(state);
        return 1;
    }
    const auto *memory = static_cast<const uint8_t *>(
        retro_get_memory_data(memory_regions[region].id));
    const size_t size = retro_get_memory_size(memory_regions[region].id);
    if (!memory || offset + 1 >= size) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushinteger(state, memory[offset] | (memory[offset + 1] << 8));
    return 1;
}

int lua_write16(lua_State *state) {
    const uint32_t address =
        static_cast<uint32_t>(luaL_checkinteger(state, 1));
    const uint16_t value =
        static_cast<uint16_t>(luaL_checkinteger(state, 2) & 0xffff);
    lua_pushboolean(state,
                    write_memory_address(address, value & 0xff) &&
                        write_memory_address(address + 1, value >> 8));
    return 1;
}

int lua_set_button(lua_State *state) {
    const std::string name = luaL_checkstring(state, 1);
    const int id = script_button_id(name);
    if (id < 0 || id >= static_cast<int>(app.lua.buttons.size())) {
        return luaL_error(state, "botao desconhecido: %s", name.c_str());
    }
    app.lua.buttons[id] = lua_toboolean(state, 2);
    return 0;
}

int lua_press(lua_State *state) {
    lua_pushboolean(state, true);
    lua_set_button(state);
    return 0;
}

int lua_release(lua_State *state) {
    lua_pushboolean(state, false);
    lua_set_button(state);
    return 0;
}

int lua_clear_input(lua_State *) {
    app.lua.buttons.fill(false);
    return 0;
}

int lua_frame(lua_State *state) {
    lua_pushinteger(state, static_cast<lua_Integer>(app.lua.frame));
    return 1;
}

int lua_log(lua_State *state) {
    const int count = lua_gettop(state);
    std::cout << "[lua]";
    for (int index = 1; index <= count; ++index) {
        std::cout << ' ' << lua_tostring(state, index);
    }
    std::cout << '\n';
    return 0;
}

void register_lua_api(lua_State *state) {
    lua_newtable(state);
    lua_pushcfunction(state, lua_read8);
    lua_setfield(state, -2, "read8");
    lua_pushcfunction(state, lua_write8);
    lua_setfield(state, -2, "write8");
    lua_pushcfunction(state, lua_read16);
    lua_setfield(state, -2, "read16");
    lua_pushcfunction(state, lua_write16);
    lua_setfield(state, -2, "write16");
    lua_pushcfunction(state, lua_set_button);
    lua_setfield(state, -2, "set_button");
    lua_pushcfunction(state, lua_press);
    lua_setfield(state, -2, "press");
    lua_pushcfunction(state, lua_release);
    lua_setfield(state, -2, "release");
    lua_pushcfunction(state, lua_clear_input);
    lua_setfield(state, -2, "clear_input");
    lua_pushcfunction(state, lua_frame);
    lua_setfield(state, -2, "frame");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");
    lua_setglobal(state, "snes");
}

void load_lua_script(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }
    if (app.lua.state) {
        lua_close(app.lua.state);
        app.lua.state = nullptr;
        app.lua.active = false;
    }
    app.lua.buttons.fill(false);
    app.lua.state = luaL_newstate();
    if (!app.lua.state) {
        std::cerr << "Nao foi possivel iniciar Lua.\n";
        return;
    }
    luaL_openlibs(app.lua.state);
    register_lua_api(app.lua.state);
    if (luaL_dofile(app.lua.state, path.string().c_str()) != 0) {
        std::cerr << "Erro no script Lua: "
                  << lua_tostring(app.lua.state, -1) << '\n';
        lua_close(app.lua.state);
        app.lua.state = nullptr;
        return;
    }
    app.lua.active = true;
    std::cout << "Script Lua carregado: " << path << '\n';
}

void set_text_editor_enabled(bool enabled);

std::vector<std::string> default_lua_script() {
    return {
        "-- Script Lua para o emulador SNES.",
        "-- Pressione Ctrl-S para salvar e Ctrl-R para recarregar.",
        "",
        "function on_frame(frame)",
        "    snes.clear_input()",
        "",
        "end",
    };
}

void load_lua_editor_file() {
    auto &editor = app.lua_editor;
    std::ifstream file(editor.path);
    editor.lines.clear();
    if (!file) {
        editor.lines = default_lua_script();
        editor.dirty = true;
        editor.status = "NOVO SCRIPT: CTRL-S PARA CRIAR O ARQUIVO";
        editor.row = 3;
        editor.col = 0;
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        editor.lines.push_back(line);
    }
    if (editor.lines.empty()) {
        editor.lines.push_back("");
    }
    editor.row = std::min(editor.row, editor.lines.size() - 1);
    editor.col = std::min(editor.col, editor.lines[editor.row].size());
    editor.dirty = false;
    editor.status = "SCRIPT ABERTO";
}

bool save_lua_editor_file() {
    auto &editor = app.lua_editor;
    if (editor.path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(editor.path.parent_path(), error);
    }
    std::ofstream file(editor.path);
    if (!file) {
        editor.status = "NAO FOI POSSIVEL SALVAR";
        return false;
    }
    for (size_t index = 0; index < editor.lines.size(); ++index) {
        file << editor.lines[index];
        if (index + 1 < editor.lines.size()) {
            file << '\n';
        }
    }
    editor.dirty = false;
    editor.status = "SALVO";
    return true;
}

std::vector<std::string> lua_completion_words() {
    std::vector<std::string> words{
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while",
        "assert", "dofile", "error", "ipairs", "loadfile", "pairs",
        "pcall", "print", "require", "tonumber", "tostring", "type",
        "math.abs", "math.floor", "math.max", "math.min", "math.random",
        "string.format", "string.find", "string.match", "string.sub",
        "table.insert", "table.remove", "table.sort",
        "snes.read8", "snes.write8", "snes.read16", "snes.write16",
        "snes.press", "snes.release", "snes.set_button",
        "snes.clear_input", "snes.frame", "snes.log",
        "up", "down", "left", "right", "a", "b", "x", "y", "l", "r",
        "start", "select", "on_frame", "frame",
    };
    for (const auto &line : app.lua_editor.lines) {
        for (size_t index = 0; index < line.size();) {
            if (std::isalpha(static_cast<unsigned char>(line[index])) ||
                line[index] == '_') {
                const size_t start = index++;
                while (index < line.size() &&
                       (std::isalnum(static_cast<unsigned char>(line[index])) ||
                        line[index] == '_')) {
                    ++index;
                }
                if (index - start > 2) {
                    words.push_back(line.substr(start, index - start));
                }
            } else {
                ++index;
            }
        }
    }
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

std::string common_completion_prefix(const std::vector<std::string> &words) {
    if (words.empty()) {
        return {};
    }
    std::string prefix = words.front();
    for (size_t index = 1; index < words.size(); ++index) {
        size_t match = 0;
        while (match < prefix.size() && match < words[index].size() &&
               prefix[match] == words[index][match]) {
            ++match;
        }
        prefix.resize(match);
        if (prefix.empty()) {
            break;
        }
    }
    return prefix;
}

void autocomplete_lua_editor() {
    auto &editor = app.lua_editor;
    auto &line = editor.lines[editor.row];
    size_t start = editor.col;
    while (start > 0) {
        const char c = line[start - 1];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) {
            break;
        }
        --start;
    }
    const std::string prefix = line.substr(start, editor.col - start);
    if (prefix.empty()) {
        editor.status = "DIGITE UM PREFIXO PARA COMPLETAR";
        return;
    }

    std::vector<std::string> matches;
    for (const auto &word : lua_completion_words()) {
        if (word.starts_with(prefix)) {
            matches.push_back(word);
        }
    }
    if (matches.empty()) {
        editor.status = "SEM SUGESTAO";
        return;
    }

    const std::string replacement = common_completion_prefix(matches);
    if (replacement.size() > prefix.size() || matches.size() == 1) {
        line.replace(start, editor.col - start, replacement);
        editor.col = start + replacement.size();
        editor.dirty = true;
    }

    std::ostringstream status;
    for (size_t index = 0; index < matches.size() && index < 5; ++index) {
        if (index) status << "  ";
        status << matches[index];
    }
    editor.status = status.str();
}

void open_lua_editor() {
    auto &editor = app.lua_editor;
    if (!editor.active) {
        load_lua_editor_file();
    }
    if (app.memory_editor.text_mode) {
        set_text_editor_enabled(false);
    }
    app.memory_editor.active = false;
    app.memory_editor.goto_popup = false;
    editor.active = true;
    editor.restore_pause = app.paused;
    app.paused = true;
    if (app.audio) {
        SDL_PauseAudioDevice(app.audio, 1);
    }
    SDL_StartTextInput();
}

void close_lua_editor() {
    auto &editor = app.lua_editor;
    editor.active = false;
    app.paused = editor.restore_pause;
    if (app.audio) {
        SDL_PauseAudioDevice(app.audio, app.paused ? 1 : 0);
    }
    SDL_StopTextInput();
}

void insert_lua_editor_text(const char *text) {
    auto &editor = app.lua_editor;
    auto &line = editor.lines[editor.row];
    const std::string input = text ? text : "";
    line.insert(editor.col, input);
    editor.col += input.size();
    editor.dirty = true;
}

void move_lua_editor_cursor(int row_delta, int col_delta) {
    auto &editor = app.lua_editor;
    if (row_delta < 0) {
        editor.row -= std::min(editor.row, static_cast<size_t>(-row_delta));
    } else if (row_delta > 0) {
        editor.row = std::min(editor.lines.size() - 1,
                              editor.row + static_cast<size_t>(row_delta));
    }
    if (col_delta < 0) {
        editor.col -= std::min(editor.col, static_cast<size_t>(-col_delta));
    } else if (col_delta > 0) {
        editor.col = std::min(editor.lines[editor.row].size(),
                              editor.col + static_cast<size_t>(col_delta));
    }
    editor.col = std::min(editor.col, editor.lines[editor.row].size());
}

bool handle_lua_editor_key(SDL_Keycode key, SDL_Keymod mod) {
    auto &editor = app.lua_editor;
    if (!editor.active) {
        return false;
    }
    const bool command = (mod & KMOD_CTRL) != 0;
    if (command && key == SDLK_s) {
        save_lua_editor_file();
        return true;
    }
    if (command && key == SDLK_r) {
        if (save_lua_editor_file()) {
            load_lua_script(editor.path);
            editor.status = "SALVO E RECARREGADO";
        }
        return true;
    }
    if (key == SDLK_ESCAPE) {
        close_lua_editor();
        return true;
    }
    if (key == SDLK_TAB) {
        autocomplete_lua_editor();
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        auto &line = editor.lines[editor.row];
        const std::string rest = line.substr(editor.col);
        line.resize(editor.col);
        editor.lines.insert(editor.lines.begin() + editor.row + 1, rest);
        ++editor.row;
        editor.col = 0;
        editor.dirty = true;
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        if (editor.col > 0) {
            auto &line = editor.lines[editor.row];
            line.erase(editor.col - 1, 1);
            --editor.col;
        } else if (editor.row > 0) {
            const std::string current = editor.lines[editor.row];
            editor.lines.erase(editor.lines.begin() + editor.row);
            --editor.row;
            editor.col = editor.lines[editor.row].size();
            editor.lines[editor.row] += current;
        }
        editor.dirty = true;
        return true;
    }
    if (key == SDLK_DELETE) {
        auto &line = editor.lines[editor.row];
        if (editor.col < line.size()) {
            line.erase(editor.col, 1);
        } else if (editor.row + 1 < editor.lines.size()) {
            line += editor.lines[editor.row + 1];
            editor.lines.erase(editor.lines.begin() + editor.row + 1);
        }
        editor.dirty = true;
        return true;
    }
    if (key == SDLK_LEFT) {
        move_lua_editor_cursor(0, -1);
        return true;
    }
    if (key == SDLK_RIGHT) {
        move_lua_editor_cursor(0, 1);
        return true;
    }
    if (key == SDLK_UP) {
        move_lua_editor_cursor(-1, 0);
        return true;
    }
    if (key == SDLK_DOWN) {
        move_lua_editor_cursor(1, 0);
        return true;
    }
    if (key == SDLK_HOME) {
        editor.col = 0;
        return true;
    }
    if (key == SDLK_END) {
        editor.col = editor.lines[editor.row].size();
        return true;
    }
    if (key == SDLK_PAGEUP) {
        editor.row -= std::min(editor.row, static_cast<size_t>(18));
        editor.col = std::min(editor.col, editor.lines[editor.row].size());
        return true;
    }
    if (key == SDLK_PAGEDOWN) {
        editor.row = std::min(editor.lines.size() - 1, editor.row + 18);
        editor.col = std::min(editor.col, editor.lines[editor.row].size());
        return true;
    }
    return true;
}

void run_lua_frame() {
    if (!app.lua.active || !app.lua.state) {
        return;
    }
    lua_getglobal(app.lua.state, "on_frame");
    if (!lua_isfunction(app.lua.state, -1)) {
        lua_pop(app.lua.state, 1);
        return;
    }
    lua_pushinteger(app.lua.state, static_cast<lua_Integer>(app.lua.frame));
    if (lua_pcall(app.lua.state, 1, 0, 0) != 0) {
        std::cerr << "Erro no on_frame Lua: "
                  << lua_tostring(app.lua.state, -1) << '\n';
        lua_pop(app.lua.state, 1);
        app.lua.active = false;
    }
}

bool write_memory_address(uint32_t address, uint8_t value) {
    unsigned region_index = 0;
    size_t offset = 0;
    if (!resolve_memory_address(address, region_index, offset)) {
        app.memory_editor.status = "ENDERECO FORA DAS REGIOES";
        return false;
    }
    auto *memory = static_cast<uint8_t *>(
        retro_get_memory_data(memory_regions[region_index].id));
    if (!memory) {
        return false;
    }
    memory[offset] = value;
    auto &editor = app.memory_editor;
    if (editor.watch_active && editor.watch_locked &&
        editor.watch_region == region_index && editor.watch_offset == offset) {
        editor.watch_value = value;
    }
    app.memory_editor.status = "POKE PELO G";
    return true;
}

void clamp_editor_address() {
    const size_t size = selected_memory_size();
    app.memory_editor.offset = size
        ? std::min(app.memory_editor.offset, size - 1)
        : 0;
    if (auto *memory = selected_memory(); memory && size) {
        app.memory_editor.value = memory[app.memory_editor.offset];
    }
    app.memory_editor.address_input =
        selected_region().base +
        static_cast<uint32_t>(app.memory_editor.offset);
    app.memory_editor.address_valid = size != 0;
}

void apply_debug_layout(bool enabled, bool fullscreen) {
    if (!app.renderer || !app.window) {
        return;
    }
    app.debug_overlay = false;
    if (fullscreen) {
        app.debug_overlay = enabled;
        SDL_RenderSetLogicalSize(app.renderer, game_width, game_height);
        return;
    }
    SDL_RenderSetLogicalSize(
        app.renderer, enabled ? game_width + debugger_width : game_width,
        game_height);
    SDL_SetWindowSize(app.window, enabled ? 1440 : 960, 720);
    SDL_SetWindowPosition(app.window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
}

void set_debug_layout(bool enabled) {
    apply_debug_layout(
        enabled,
        (SDL_GetWindowFlags(app.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0);
}

bool write_selected_memory() {
    uint8_t *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!app.memory_editor.address_valid || !memory ||
        app.memory_editor.offset >= size) {
        app.memory_editor.status = "REGIAO INDISPONIVEL";
        return false;
    }
    memory[app.memory_editor.offset] = app.memory_editor.value;
    if (app.memory_editor.watch_active && app.memory_editor.watch_locked &&
        app.memory_editor.watch_region == app.memory_editor.region &&
        app.memory_editor.watch_offset == app.memory_editor.offset) {
        app.memory_editor.watch_value = app.memory_editor.value;
    }
    char status[96];
    std::snprintf(status, sizeof(status), "POKE $%06X = $%02X",
                  selected_region().base +
                      static_cast<unsigned>(app.memory_editor.offset),
                  app.memory_editor.value);
    app.memory_editor.status = status;
    return true;
}

char visible_memory_char(uint8_t value) {
    return value >= 32 && value <= 126 ? static_cast<char>(value) : '.';
}

void set_text_editor_enabled(bool enabled) {
    auto &editor = app.memory_editor;
    editor.text_mode = enabled;
    if (enabled) {
        editor.active = true;
        editor.editing_value = false;
        editor.replace_on_type = true;
        editor.status = "MODO TEXTO: DIGITE ASCII";
        clamp_editor_address();
        SDL_StartTextInput();
    } else {
        editor.active = false;
        editor.status = "MODO TEXTO DESLIGADO";
        SDL_StopTextInput();
    }
}

void toggle_text_editor() {
    set_text_editor_enabled(!app.memory_editor.text_mode);
}

void write_text_to_memory(const char *text) {
    auto &editor = app.memory_editor;
    uint8_t *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!editor.text_mode || !memory || !size || !text) {
        return;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        const auto value = static_cast<unsigned char>(*cursor);
        if (value < 32 || value > 126) {
            continue;
        }
        if (editor.offset >= size) {
            editor.status = "FIM DA REGIAO";
            break;
        }
        memory[editor.offset] = value;
        editor.value = static_cast<uint8_t>(value);
        if (editor.offset + 1 < size) {
            ++editor.offset;
            clamp_editor_address();
        }
    }
    editor.status = "TEXTO GRAVADO";
}

bool read_selected_memory_value(uint8_t &value) {
    auto &editor = app.memory_editor;
    const auto *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!editor.address_valid || !memory || editor.offset >= size) {
        return false;
    }
    value = memory[editor.offset];
    return true;
}

void toggle_memory_freeze() {
    auto &editor = app.memory_editor;
    if (!editor.address_valid || !selected_memory() ||
        editor.offset >= selected_memory_size()) {
        editor.status = "ENDERECO INVALIDO";
        return;
    }

    if (editor.watch_active && editor.watch_locked &&
        editor.watch_region == editor.region &&
        editor.watch_offset == editor.offset) {
        editor.watch_active = false;
        editor.watch_locked = false;
        editor.status = "FROZEN DESLIGADO";
        return;
    }

    if (!editor.active || !editor.editing_value) {
        uint8_t current = 0;
        if (read_selected_memory_value(current)) {
            editor.value = current;
        }
    }
    editor.watch_active = true;
    editor.watch_locked = true;
    editor.watch_region = editor.region;
    editor.watch_offset = editor.offset;
    editor.watch_value = editor.value;
    write_selected_memory();
    char status[96];
    std::snprintf(status, sizeof(status), "FROZEN $%06X = $%02X",
                  selected_region().base + static_cast<unsigned>(editor.offset),
                  editor.watch_value);
    editor.status = status;
}

void clear_memory_freeze() {
    auto &editor = app.memory_editor;
    editor.watch_active = false;
    editor.watch_locked = false;
    editor.active = false;
    editor.status = "FROZEN REMOVIDO";
}

void adjust_selected_memory_value(int delta) {
    auto &editor = app.memory_editor;
    uint8_t current = editor.value;
    if (!editor.active || !editor.editing_value) {
        if (!read_selected_memory_value(current)) {
            editor.status = "ENDERECO INVALIDO";
            return;
        }
    }
    editor.value = static_cast<uint8_t>(
        (static_cast<int>(current) + delta) & 0xff);
    editor.editing_value = true;
    editor.replace_on_type = true;
    write_selected_memory();
}

void apply_memory_lock() {
    auto &editor = app.memory_editor;
    if (!editor.watch_active || !editor.watch_locked ||
        editor.watch_region >= memory_regions.size()) {
        return;
    }
    auto *memory = static_cast<uint8_t *>(
        retro_get_memory_data(memory_regions[editor.watch_region].id));
    const size_t size =
        retro_get_memory_size(memory_regions[editor.watch_region].id);
    if (memory && editor.watch_offset < size) {
        memory[editor.watch_offset] = editor.watch_value;
    }
}

uint8_t current_watch_value() {
    const auto &editor = app.memory_editor;
    if (!editor.watch_active || editor.watch_region >= memory_regions.size()) {
        return 0;
    }
    const auto *memory = static_cast<const uint8_t *>(
        retro_get_memory_data(memory_regions[editor.watch_region].id));
    const size_t size =
        retro_get_memory_size(memory_regions[editor.watch_region].id);
    return memory && editor.watch_offset < size
        ? memory[editor.watch_offset]
        : 0;
}

uint32_t memory_address(unsigned region, size_t offset) {
    return memory_regions[region].base + static_cast<uint32_t>(offset);
}

const CustomMemoryWatch *memory_watch_for(unsigned region, size_t offset) {
    for (const auto &watch : app.custom_memory_watches) {
        if (watch.region == region && watch.offset == offset) {
            return &watch;
        }
    }
    return nullptr;
}

int memory_watch_index_for(unsigned region, size_t offset) {
    for (size_t index = 0; index < app.custom_memory_watches.size(); ++index) {
        const auto &watch = app.custom_memory_watches[index];
        if (watch.region == region && watch.offset == offset) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::string memory_label(unsigned region, size_t offset) {
    if (const auto *watch = memory_watch_for(region, offset)) {
        return watch->label;
    }
    char label[32];
    std::snprintf(label, sizeof(label), "%s $%06X",
                  memory_regions[region].name, memory_address(region, offset));
    return label;
}

bool selected_memory_is_frozen() {
    const auto &editor = app.memory_editor;
    return editor.watch_active && editor.watch_locked &&
           editor.watch_region == editor.region &&
           editor.watch_offset == editor.offset;
}

const MemoryActivity::VisualCorrelation *focused_visual_correlation() {
    const auto &editor = app.memory_editor;
    if (!editor.focused_activity ||
        editor.focused_activity_offset >= app.memory_activity.correlations.size()) {
        return nullptr;
    }
    return &app.memory_activity.correlations[editor.focused_activity_offset];
}

bool focused_visual_marker_ready() {
    const auto &editor = app.memory_editor;
    const auto *correlation = focused_visual_correlation();
    if (!app.memory_debug || !correlation || !editor.focused_region_frozen ||
        correlation->evidence < 8 || correlation->value_axis == 0) {
        return false;
    }
    const int area = editor.focused_screen_region.w * editor.focused_screen_region.h;
    return area >= 64 && area < game_width * game_height * 18 / 100;
}

void select_important_memory(int delta) {
    auto &index = app.memory_editor.important_index;
    const unsigned count = app.custom_memory_watches.empty()
        ? 10U
        : static_cast<unsigned>(app.custom_memory_watches.size());
    if (!count) {
        return;
    }
    index = static_cast<unsigned>(
        (static_cast<int>(index) + delta + static_cast<int>(count)) %
        static_cast<int>(count));
}

void focus_important_memory() {
    auto &editor = app.memory_editor;
    if (!app.custom_memory_watches.empty()) {
        editor.important_index %= app.custom_memory_watches.size();
        const auto &watch =
            app.custom_memory_watches[editor.important_index];
        focus_memory_address(watch.address);
        editor.status = watch.label;
        return;
    }
    editor.focused_activity_offset =
        app.memory_activity.hottest[editor.important_index];
    editor.focused_activity = true;
    editor.focused_screen_region = {};
    editor.focused_region_frozen = false;
    if (editor.focused_activity_offset <
        app.memory_activity.screen_regions.size()) {
        const SDL_Rect known =
            app.memory_activity.screen_regions[editor.focused_activity_offset];
        if (known.w > 0 && known.h > 0) {
            editor.focused_screen_region = known;
            editor.focused_region_frozen = true;
        }
    }
}

void focus_player_candidate() {
    auto &editor = app.memory_editor;
    if (!app.custom_memory_watches.empty()) {
        editor.important_index = 0;
        focus_important_memory();
        return;
    }
    const auto &activity = app.memory_activity;
    if (activity.player_candidate_score == 0) {
        editor.important_index = 0;
        focus_important_memory();
        editor.status = "SEM PLAYER CONFIRMADO - MAIS ATIVO";
        return;
    }

    editor.focused_activity_offset = activity.player_candidate_offset;
    editor.focused_activity = true;
    editor.focused_screen_region = {};
    editor.focused_region_frozen = false;
    if (activity.player_candidate_offset < activity.screen_regions.size()) {
        const SDL_Rect known =
            activity.screen_regions[activity.player_candidate_offset];
        if (known.w > 0 && known.h > 0) {
            editor.focused_screen_region = known;
            editor.focused_region_frozen = true;
        }
    }
    editor.region = 0;
    editor.offset = activity.player_candidate_offset;
    clamp_editor_address();
    editor.status = "PROVAVEL PLAYER SELECIONADO";
}

void jump_to_important_memory() {
    if (!app.custom_memory_watches.empty()) {
        focus_important_memory();
        app.memory_editor.status =
            app.custom_memory_watches[app.memory_editor.important_index].label;
        return;
    }
    if (!app.memory_editor.focused_activity) {
        focus_important_memory();
    }
    const size_t offset = app.memory_editor.focused_activity_offset;
    app.memory_editor.region = 0;
    app.memory_editor.offset = offset;
    clamp_editor_address();
    app.memory_editor.status = "ENDERECO ATIVO SELECIONADO";
}

void update_hottest_addresses() {
    auto &activity = app.memory_activity;
    std::array<std::pair<uint32_t, size_t>, 10> best{};
    for (size_t offset = 0; offset < activity.scores.size(); ++offset) {
        const uint32_t score = activity.scores[offset];
        if (score <= best.back().first) {
            continue;
        }
        best.back() = {score, offset};
        for (size_t index = best.size() - 1;
             index > 0 && best[index].first > best[index - 1].first;
             --index) {
            std::swap(best[index], best[index - 1]);
        }
    }
    for (size_t index = 0; index < best.size(); ++index) {
        activity.hottest[index] = best[index].second;
    }

    activity.player_candidate_score = 0;
    for (size_t offset : activity.hottest) {
        if (offset >= activity.correlations.size() ||
            offset >= activity.scores.size()) {
            continue;
        }
        const auto &correlation = activity.correlations[offset];
        if (correlation.evidence < 3 || correlation.width <= 0 ||
            correlation.height <= 0) {
            continue;
        }
        const float area = correlation.width * correlation.height;
        const float screen_area = game_width * game_height;
        if (area < 64 || area > screen_area * 0.18f) {
            continue;
        }

        const float horizontal_center =
            1.0f - std::min(1.0f,
                            std::abs(correlation.center_x - game_width / 2.0f) /
                                (game_width / 2.0f));
        const float vertical_playfield =
            std::clamp(correlation.center_y / game_height, 0.0f, 1.0f);
        const float compactness =
            1.0f - std::min(1.0f, area / (screen_area * 0.18f));
        const float movement =
            std::min(1.0f, correlation.movement / 160.0f);
        const uint32_t candidate_score = static_cast<uint32_t>(
            activity.scores[offset] +
            correlation.evidence * 48 +
            horizontal_center * 320 +
            vertical_playfield * 260 +
            compactness * 360 +
            movement * 520);
        if (candidate_score > activity.player_candidate_score) {
            activity.player_candidate_score = candidate_score;
            activity.player_candidate_offset = offset;
        }
    }
}

std::vector<SDL_Rect> find_changed_regions(
    const uint8_t *current, const uint8_t *previous, unsigned width,
    unsigned height, size_t current_pitch, size_t packed_pitch,
    size_t bytes_per_pixel) {
    constexpr unsigned tile_size = 8;
    const unsigned columns = (width + tile_size - 1) / tile_size;
    const unsigned rows = (height + tile_size - 1) / tile_size;
    std::vector<uint8_t> changed_tiles(columns * rows, 0);

    for (unsigned tile_y = 0; tile_y < rows; ++tile_y) {
        for (unsigned tile_x = 0; tile_x < columns; ++tile_x) {
            unsigned changed_pixels = 0;
            const unsigned x_end = std::min(width, (tile_x + 1) * tile_size);
            const unsigned y_end = std::min(height, (tile_y + 1) * tile_size);
            for (unsigned y = tile_y * tile_size; y < y_end; ++y) {
                const uint8_t *current_row = current + y * current_pitch;
                const uint8_t *previous_row = previous + y * packed_pitch;
                for (unsigned x = tile_x * tile_size; x < x_end; ++x) {
                    const size_t pixel = x * bytes_per_pixel;
                    if (std::memcmp(current_row + pixel, previous_row + pixel,
                                    bytes_per_pixel) != 0) {
                        ++changed_pixels;
                    }
                }
            }
            if (changed_pixels >= 3) {
                changed_tiles[tile_y * columns + tile_x] = 1;
            }
        }
    }

    std::vector<SDL_Rect> regions;
    std::deque<std::pair<unsigned, unsigned>> pending;
    for (unsigned start_y = 0; start_y < rows; ++start_y) {
        for (unsigned start_x = 0; start_x < columns; ++start_x) {
            const size_t start = start_y * columns + start_x;
            if (!changed_tiles[start]) {
                continue;
            }
            changed_tiles[start] = 0;
            pending.push_back({start_x, start_y});
            unsigned min_x = start_x;
            unsigned max_x = start_x;
            unsigned min_y = start_y;
            unsigned max_y = start_y;
            while (!pending.empty()) {
                const auto [x, y] = pending.front();
                pending.pop_front();
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
                constexpr int directions[4][2] = {
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto &direction : directions) {
                    const int next_x = static_cast<int>(x) + direction[0];
                    const int next_y = static_cast<int>(y) + direction[1];
                    if (next_x < 0 || next_y < 0 ||
                        next_x >= static_cast<int>(columns) ||
                        next_y >= static_cast<int>(rows)) {
                        continue;
                    }
                    const size_t next =
                        static_cast<size_t>(next_y) * columns + next_x;
                    if (changed_tiles[next]) {
                        changed_tiles[next] = 0;
                        pending.push_back(
                            {static_cast<unsigned>(next_x),
                             static_cast<unsigned>(next_y)});
                    }
                }
            }

            unsigned min_pixel_x = width;
            unsigned min_pixel_y = height;
            unsigned max_pixel_x = 0;
            unsigned max_pixel_y = 0;
            bool has_changed_pixel = false;
            const unsigned x_begin = min_x * tile_size;
            const unsigned y_begin = min_y * tile_size;
            const unsigned x_end = std::min(width, (max_x + 1) * tile_size);
            const unsigned y_end = std::min(height, (max_y + 1) * tile_size);
            for (unsigned y = y_begin; y < y_end; ++y) {
                const uint8_t *current_row = current + y * current_pitch;
                const uint8_t *previous_row = previous + y * packed_pitch;
                for (unsigned x = x_begin; x < x_end; ++x) {
                    const size_t pixel = x * bytes_per_pixel;
                    if (std::memcmp(current_row + pixel, previous_row + pixel,
                                    bytes_per_pixel) == 0) {
                        continue;
                    }
                    has_changed_pixel = true;
                    min_pixel_x = std::min(min_pixel_x, x);
                    min_pixel_y = std::min(min_pixel_y, y);
                    max_pixel_x = std::max(max_pixel_x, x);
                    max_pixel_y = std::max(max_pixel_y, y);
                }
            }
            if (!has_changed_pixel) {
                continue;
            }

            SDL_Rect region{
                static_cast<int>(min_pixel_x * game_width / width),
                static_cast<int>(min_pixel_y * game_height / height),
                static_cast<int>((max_pixel_x + 1) * game_width / width) -
                    static_cast<int>(min_pixel_x * game_width / width),
                static_cast<int>((max_pixel_y + 1) * game_height / height) -
                    static_cast<int>(min_pixel_y * game_height / height),
            };
            const int area = region.w * region.h;
            if (area >= 64 &&
                area < game_width * game_height * 45 / 100) {
                regions.push_back(region);
            }
        }
    }
    std::sort(regions.begin(), regions.end(), [](const SDL_Rect &left,
                                                  const SDL_Rect &right) {
        return left.w * left.h < right.w * right.h;
    });
    return regions;
}

struct RegionMatch {
    const SDL_Rect *region = nullptr;
    uint8_t axis = 0;
};

float coordinate_distance(uint8_t value, const SDL_Rect &region,
                          uint8_t axis) {
    const float center = axis == 1
        ? (region.x + region.w / 2.0f) * 255.0f / game_width
        : (region.y + region.h / 2.0f) * 239.0f / game_height;
    return std::abs(static_cast<float>(value) - center);
}

RegionMatch choose_correlated_region(
    uint8_t value, const MemoryActivity::VisualCorrelation &correlation,
    const std::vector<SDL_Rect> &regions) {
    RegionMatch best;
    float best_score = std::numeric_limits<float>::max();
    for (const SDL_Rect &region : regions) {
        const float center_x = region.x + region.w / 2.0f;
        const float center_y = region.y + region.h / 2.0f;
        for (uint8_t axis : {uint8_t{1}, uint8_t{2}}) {
            if (correlation.value_axis && axis != correlation.value_axis) {
                continue;
            }
            const float value_distance =
                coordinate_distance(value, region, axis);
            if (!correlation.evidence && value_distance > 24.0f) {
                continue;
            }

            float score = value_distance * 18.0f;
            if (correlation.evidence) {
                const float dx = center_x - correlation.center_x;
                const float dy = center_y - correlation.center_y;
                score += std::sqrt(dx * dx + dy * dy) * 0.25f;
            }
            score += std::sqrt(static_cast<float>(region.w * region.h)) * 0.1f;
            if (score < best_score) {
                best_score = score;
                best = {&region, axis};
            }
        }
    }

    if (!best.region) {
        return {};
    }
    if (!correlation.evidence && best_score > 460.0f) {
        return {};
    }
    return best;
}

void update_visual_correlation(size_t offset, uint8_t value,
                               const std::vector<SDL_Rect> &regions) {
    if (regions.empty() ||
        offset >= app.memory_activity.correlations.size()) {
        return;
    }
    auto &correlation = app.memory_activity.correlations[offset];
    const RegionMatch match =
        choose_correlated_region(value, correlation, regions);
    if (!match.region) {
        if (correlation.evidence > 0) {
            --correlation.evidence;
        }
        if (correlation.evidence == 0) {
            correlation.value_axis = 0;
            app.memory_activity.screen_regions[offset] = {};
            auto &editor = app.memory_editor;
            if (editor.focused_activity &&
                offset == editor.focused_activity_offset) {
                editor.focused_region_frozen = false;
                editor.focused_screen_region = {};
            }
        }
        return;
    }
    const SDL_Rect *best = match.region;

    const float center_x = best->x + best->w / 2.0f;
    const float center_y = best->y + best->h / 2.0f;
    if (correlation.evidence > 0) {
        const float dx = center_x - correlation.center_x;
        const float dy = center_y - correlation.center_y;
        correlation.movement =
            correlation.movement * 0.9f + std::sqrt(dx * dx + dy * dy);
    }
    const float alpha = correlation.evidence ? 0.32f : 1.0f;
    correlation.center_x += (center_x - correlation.center_x) * alpha;
    correlation.center_y += (center_y - correlation.center_y) * alpha;
    correlation.width += (best->w - correlation.width) * alpha;
    correlation.height += (best->h - correlation.height) * alpha;
    correlation.evidence =
        std::min<uint16_t>(correlation.evidence + 1, 0xFFFF);
    correlation.value_axis = match.axis;

    if (correlation.evidence >= 3) {
        SDL_Rect result = *best;
        result.x = std::clamp(result.x, 0, game_width - result.w);
        result.y = std::clamp(result.y, 0, game_height - result.h);
        app.memory_activity.screen_regions[offset] = result;
        auto &editor = app.memory_editor;
        if (editor.focused_activity &&
            offset == editor.focused_activity_offset) {
            editor.focused_screen_region = result;
            editor.focused_region_frozen = true;
        }
    }
}

void track_memory_activity(const void *frame, unsigned width, unsigned height,
                           size_t pitch) {
    uint8_t *memory = wram();
    const size_t memory_size =
        retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!memory || !memory_size || !frame) {
        return;
    }

    auto &activity = app.memory_activity;
    if (activity.previous_wram.size() != memory_size) {
        activity.previous_wram.assign(memory, memory + memory_size);
        activity.scores.assign(memory_size, 0);
        activity.screen_regions.assign(memory_size, SDL_Rect{});
        activity.correlations.assign(memory_size, {});
    }

    const size_t bytes_per_pixel =
        app.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4U : 2U;
    size_t row_bytes = 0;
    size_t frame_size = 0;
    if (!frame_layout(width, height, pitch, row_bytes, frame_size)) {
        return;
    }
    const auto *pixels = static_cast<const uint8_t *>(frame);
    std::vector<SDL_Rect> changed_regions;
    if (activity.previous_frame.size() == frame_size &&
        activity.frame_width == width && activity.frame_height == height) {
        changed_regions = find_changed_regions(
            pixels, activity.previous_frame.data(), width, height, pitch,
            row_bytes, bytes_per_pixel);
    }

    activity.previous_frame.resize(frame_size);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(activity.previous_frame.data() + y * row_bytes,
                    pixels + y * pitch, row_bytes);
    }
    activity.frame_width = width;
    activity.frame_height = height;

    for (size_t offset = 0; offset < memory_size; ++offset) {
        if (memory[offset] == activity.previous_wram[offset]) {
            continue;
        }
        const uint8_t value = memory[offset];
        activity.previous_wram[offset] = value;
        activity.scores[offset] =
            std::min<uint32_t>(activity.scores[offset] + 64, 0xFFFF);
        update_visual_correlation(offset, value, changed_regions);
    }

    ++activity.frames;
    if (activity.frames % 30 == 0) {
        update_hottest_addresses();
        for (uint32_t &score : activity.scores) {
            score = score * 15 / 16;
        }
    }
}

void draw_screen_memory_marker() {
    const auto &editor = app.memory_editor;
    if (!focused_visual_marker_ready()) {
        return;
    }
    const size_t offset = editor.focused_activity_offset;
    SDL_Rect marker = editor.focused_screen_region;
    if (marker.w <= 0 || marker.h <= 0) {
        return;
    }
    const SDL_Color marker_color = selected_memory_is_frozen()
        ? SDL_Color{85, 180, 255, 255}
        : SDL_Color{255, 220, 90, 255};
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, marker_color.r, marker_color.g,
                           marker_color.b, 42);
    SDL_RenderFillRect(app.renderer, &marker);
    SDL_SetRenderDrawColor(app.renderer, marker_color.r, marker_color.g,
                           marker_color.b, 255);
    for (int inset = 0; inset < 3; ++inset) {
        const SDL_Rect border{marker.x + inset, marker.y + inset,
                              marker.w - inset * 2, marker.h - inset * 2};
        SDL_RenderDrawRect(app.renderer, &border);
    }

    char badge[8];
    const int watch_index = memory_watch_index_for(0, offset);
    if (watch_index >= 0) {
        std::snprintf(badge, sizeof(badge), "%02d", watch_index + 1);
    } else {
        std::snprintf(badge, sizeof(badge), "M");
    }
    const int badge_w = watch_index >= 0 ? 22 : 16;
    const int badge_x = std::clamp(marker.x + 4, 0, game_width - badge_w);
    const int badge_y = std::clamp(marker.y - 16, 0, game_height - 14);
    const SDL_Rect badge_box{badge_x, badge_y, badge_w, 14};
    SDL_SetRenderDrawColor(app.renderer, 8, 10, 18, 230);
    SDL_RenderFillRect(app.renderer, &badge_box);
    SDL_SetRenderDrawColor(app.renderer, marker_color.r, marker_color.g,
                           marker_color.b, 255);
    SDL_RenderDrawRect(app.renderer, &badge_box);
    draw_text(badge_box.x + 4, badge_box.y + 3, badge,
              SDL_Color{245, 245, 245, 255}, 1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

struct SpriteSize {
    int small_width;
    int small_height;
    int large_width;
    int large_height;
};

struct SpriteSelection {
    int index = -1;
    const CoreSOBJ *sprite = nullptr;
    int width = 0;
    int height = 0;
};

SpriteSize sprite_size_pair(uint8_t select) {
    constexpr std::array<SpriteSize, 8> sizes{{
        {8, 8, 16, 16},
        {8, 8, 32, 32},
        {8, 8, 64, 64},
        {16, 16, 32, 32},
        {16, 16, 64, 64},
        {32, 32, 64, 64},
        {16, 32, 32, 64},
        {16, 32, 32, 32},
    }};
    return sizes[select & 7];
}

CoreSPPU *core_ppu() {
    static auto *ppu = [] {
        if (void *symbol = dlsym(RTLD_DEFAULT, "PPU")) {
            return reinterpret_cast<CoreSPPU *>(symbol);
        }
        return reinterpret_cast<CoreSPPU *>(dlsym(RTLD_DEFAULT, "_PPU"));
    }();
    return ppu;
}

std::pair<int, int> sprite_dimensions(const CoreSOBJ &sprite) {
    const CoreSPPU *ppu = core_ppu();
    const SpriteSize sizes = sprite_size_pair(ppu ? ppu->OBJSizeSelect : 0);
    return sprite.Size ? std::pair{sizes.large_width, sizes.large_height}
                       : std::pair{sizes.small_width, sizes.small_height};
}

uint32_t sprite_tile_address(const CoreSOBJ &sprite, int tile_x, int tile_y) {
    const CoreSPPU *ppu = core_ppu();
    if (!ppu) {
        return 0;
    }
    const uint16_t tile = static_cast<uint16_t>(
        (sprite.Name + tile_x + tile_y * 16) & 0x1ff);
    return (ppu->OBJNameBase + (tile & 0xff) * 32U +
            ((tile & 0x100) ? ppu->OBJNameSelect : 0)) &
           0xffffU;
}

uint8_t sprite_pixel(const uint8_t *vram, const CoreSOBJ &sprite,
                     int width, int height, int x, int y) {
    if (sprite.HFlip) x = width - 1 - x;
    if (sprite.VFlip) y = height - 1 - y;

    const int tile_x = x / 8;
    const int tile_y = y / 8;
    const int pixel_x = x % 8;
    const int pixel_y = y % 8;
    const uint32_t address = sprite_tile_address(sprite, tile_x, tile_y);
    const uint32_t row = address + static_cast<uint32_t>(pixel_y) * 2U;
    const uint8_t plane0 = vram[(row + 0) & 0xffffU];
    const uint8_t plane1 = vram[(row + 1) & 0xffffU];
    const uint8_t plane2 = vram[(row + 16) & 0xffffU];
    const uint8_t plane3 = vram[(row + 17) & 0xffffU];
    const int bit = 7 - pixel_x;
    return static_cast<uint8_t>(((plane0 >> bit) & 1) |
                                (((plane1 >> bit) & 1) << 1) |
                                (((plane2 >> bit) & 1) << 2) |
                                (((plane3 >> bit) & 1) << 3));
}

SDL_Color cgram_color(uint8_t palette, uint8_t color_index) {
    const CoreSPPU *ppu = core_ppu();
    if (!ppu) {
        return SDL_Color{255, 255, 255, 255};
    }
    const uint16_t color = ppu->CGDATA[128 + (palette & 7) * 16 + color_index];
    const uint8_t red = color & 0x1f;
    const uint8_t green = (color >> 5) & 0x1f;
    const uint8_t blue = (color >> 10) & 0x1f;
    return SDL_Color{
        static_cast<uint8_t>(red * 255 / 31),
        static_cast<uint8_t>(green * 255 / 31),
        static_cast<uint8_t>(blue * 255 / 31),
        255};
}

bool sprite_has_pixels(const uint8_t *vram, const CoreSOBJ &sprite,
                       int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (sprite_pixel(vram, sprite, width, height, x, y)) {
                return true;
            }
        }
    }
    return false;
}

bool sprite_uses_vram_range(const CoreSOBJ &sprite, int width, int height,
                            size_t offset) {
    for (int tile_y = 0; tile_y < height / 8; ++tile_y) {
        for (int tile_x = 0; tile_x < width / 8; ++tile_x) {
            const uint32_t address = sprite_tile_address(sprite, tile_x, tile_y);
            if (offset >= address && offset < address + 32) {
                return true;
            }
        }
    }
    return false;
}

SDL_Rect sprite_screen_rect(const CoreSOBJ &sprite, int width, int height) {
    return SDL_Rect{
        sprite.HPos * game_width / 256,
        static_cast<int>(sprite.VPos) * game_height / 224,
        std::max(1, width * game_width / 256),
        std::max(1, height * game_height / 224),
    };
}

int center_distance_score(const SDL_Rect &left, const SDL_Rect &right) {
    const int left_x = left.x + left.w / 2;
    const int left_y = left.y + left.h / 2;
    const int right_x = right.x + right.w / 2;
    const int right_y = right.y + right.h / 2;
    return std::abs(left_x - right_x) + std::abs(left_y - right_y);
}

SpriteSelection select_oam_sprite(const uint8_t *vram) {
    const CoreSPPU *ppu = core_ppu();
    if (!ppu || !vram) {
        return {};
    }
    const auto &editor = app.memory_editor;
    SpriteSelection best;
    int best_score = std::numeric_limits<int>::max();
    const uint8_t value =
        selected_memory() && editor.offset < selected_memory_size()
            ? selected_memory()[editor.offset]
            : editor.value;
    uint8_t axis = 0;
    if (editor.focused_activity_offset <
        app.memory_activity.correlations.size()) {
        axis = app.memory_activity.correlations[editor.focused_activity_offset]
                   .value_axis;
    }

    for (int index = 0; index < 128; ++index) {
        const CoreSOBJ &sprite = ppu->OBJ[index];
        const auto [width, height] = sprite_dimensions(sprite);
        if (sprite.VPos >= 240 || sprite.HPos <= -width || sprite.HPos >= 256) {
            continue;
        }
        if (!sprite_has_pixels(vram, sprite, width, height)) {
            continue;
        }

        int score = 0;
        if (selected_region().id == RETRO_MEMORY_VIDEO_RAM) {
            if (!sprite_uses_vram_range(sprite, width, height, editor.offset)) {
                continue;
            }
            score = static_cast<int>(std::abs(
                static_cast<int>(sprite_tile_address(sprite, 0, 0)) -
                static_cast<int>(editor.offset)));
        } else if (editor.focused_region_frozen) {
            score = center_distance_score(sprite_screen_rect(sprite, width, height),
                                          editor.focused_screen_region);
            if (axis == 1 || axis == 2) {
                const int coordinate =
                    axis == 1 ? (sprite.HPos & 0xff) : (sprite.VPos & 0xff);
                score += std::abs(static_cast<int>(value) - coordinate) * 2;
            }
        } else if (axis == 1 || axis == 2) {
            const int coordinate =
                axis == 1 ? (sprite.HPos & 0xff) : (sprite.VPos & 0xff);
            score = std::abs(static_cast<int>(value) - coordinate);
            if (score > 4) {
                continue;
            }
        } else {
            score = index + 4096;
        }

        score += index;
        if (score < best_score) {
            best_score = score;
            best = {index, &sprite, width, height};
        }
    }
    return best;
}

void draw_preview_checker(const SDL_Rect &preview) {
    constexpr int tile = 8;
    for (int y = preview.y; y < preview.y + preview.h; y += tile) {
        for (int x = preview.x; x < preview.x + preview.w; x += tile) {
            const bool bright = ((x - preview.x) / tile +
                                 (y - preview.y) / tile) %
                                2;
            const uint8_t shade = bright ? 38 : 18;
            SDL_SetRenderDrawColor(app.renderer, shade, shade, shade + 10, 255);
            const SDL_Rect square{x, y, tile, tile};
            SDL_RenderFillRect(app.renderer, &square);
        }
    }
}

void draw_correlated_sprite_preview(int panel_x) {
    draw_text(panel_x + 18, 254, "SPRITE OAM/VRAM",
              SDL_Color{255, 220, 90, 255}, 1);
    const SDL_Rect preview{panel_x + 18, 274, 220, 142};
    draw_preview_checker(preview);
    SDL_SetRenderDrawColor(app.renderer, 75, 170, 220, 255);
    SDL_RenderDrawRect(app.renderer, &preview);

    const auto *vram = static_cast<const uint8_t *>(
        retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM));
    const auto selection = select_oam_sprite(vram);
    if (!core_ppu()) {
        draw_text(preview.x + 12, preview.y + 60, "PPU NAO EXPORTADO",
                  SDL_Color{255, 120, 120, 255}, 1);
        return;
    }
    if (!vram || retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM) < 0x10000) {
        draw_text(preview.x + 12, preview.y + 60, "VRAM INDISPONIVEL",
                  SDL_Color{255, 120, 120, 255}, 1);
        return;
    }
    if (!selection.sprite) {
        draw_text(preview.x + 12, preview.y + 60, "SEM OBJ VISIVEL",
                  SDL_Color{160, 160, 160, 255}, 1);
        return;
    }

    const auto &editor = app.memory_editor;
    const auto &correlation =
        editor.focused_activity_offset < app.memory_activity.correlations.size()
            ? app.memory_activity.correlations[editor.focused_activity_offset]
            : MemoryActivity::VisualCorrelation{};
    char confidence[64];
    const char axis = correlation.value_axis == 1 ? 'X' :
                      correlation.value_axis == 2 ? 'Y' : '?';
    std::snprintf(confidence, sizeof(confidence),
                  "OBJ %03d  %dx%d  EIXO %c",
                  selection.index, selection.width, selection.height, axis);
    draw_text(panel_x + 18, 418, confidence,
              SDL_Color{150, 200, 230, 255}, 1);

    const int scale = std::max(
        1, std::min(preview.w / selection.width, preview.h / selection.height));
    const int origin_x = preview.x + (preview.w - selection.width * scale) / 2;
    const int origin_y = preview.y + (preview.h - selection.height * scale) / 2;
    for (int y = 0; y < selection.height; ++y) {
        for (int x = 0; x < selection.width; ++x) {
            const uint8_t color_index =
                sprite_pixel(vram, *selection.sprite, selection.width,
                             selection.height, x, y);
            if (!color_index) {
                continue;
            }
            const SDL_Color color =
                cgram_color(selection.sprite->Palette, color_index);
            SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, 255);
            const SDL_Rect pixel{origin_x + x * scale, origin_y + y * scale,
                                 scale, scale};
            SDL_RenderFillRect(app.renderer, &pixel);
        }
    }
    SDL_SetRenderDrawColor(app.renderer, 255, 70, 70, 255);
    SDL_RenderDrawRect(app.renderer, &preview);
}

void draw_watch_form(int panel_x) {
    auto &editor = app.memory_editor;
    draw_text(panel_x + 258, 254, "MEMORIA SELECIONADA",
              SDL_Color{255, 220, 90, 255}, 1);

    const SDL_Color address_color =
        editor.active && !editor.editing_value
            ? SDL_Color{255, 220, 90, 255}
            : SDL_Color{210, 210, 210, 255};
    const SDL_Color value_color =
        editor.active && editor.editing_value
            ? SDL_Color{255, 220, 90, 255}
            : SDL_Color{210, 210, 210, 255};

    SDL_SetRenderDrawColor(app.renderer, 24, 28, 40, 255);
    const SDL_Rect address_box{panel_x + 258, 300, 230, 38};
    const SDL_Rect value_box{panel_x + 258, 348, 110, 38};
    const SDL_Rect edit_box{panel_x + 378, 348, 110, 38};
    SDL_RenderFillRect(app.renderer, &address_box);
    SDL_RenderFillRect(app.renderer, &value_box);
    SDL_RenderFillRect(app.renderer, &edit_box);
    SDL_SetRenderDrawColor(app.renderer, address_color.r, address_color.g,
                           address_color.b, 255);
    SDL_RenderDrawRect(app.renderer, &address_box);
    SDL_SetRenderDrawColor(app.renderer, value_color.r, value_color.g,
                           value_color.b, 255);
    SDL_RenderDrawRect(app.renderer, &value_box);
    SDL_RenderDrawRect(app.renderer, &edit_box);

    char line[96];
    const std::string label = memory_label(editor.region, editor.offset);
    std::snprintf(line, sizeof(line), "%.28s", label.c_str());
    draw_text(panel_x + 258, 278, line, SDL_Color{235, 235, 235, 255}, 1);

    std::snprintf(line, sizeof(line), "%s $%06X", selected_region().name,
                  selected_region().base + static_cast<unsigned>(editor.offset));
    draw_text(address_box.x + 8, address_box.y + 11, line, address_color, 1);

    uint8_t current = 0;
    const bool has_current = read_selected_memory_value(current);
    std::snprintf(line, sizeof(line), "JOGO $%02X", has_current ? current : 0);
    draw_text(value_box.x + 8, value_box.y + 11, line, value_color, 1);
    const uint8_t staged =
        editor.active && editor.editing_value ? editor.value : current;
    std::snprintf(line, sizeof(line), "NOVO $%02X", has_current ? staged : 0);
    draw_text(edit_box.x + 8, edit_box.y + 11, line, value_color, 1);

    if (selected_memory_is_frozen()) {
        draw_text(panel_x + 258, 396, "FROZEN: ESTE VALOR FICA FIXO",
                  SDL_Color{85, 180, 255, 255}, 1);
    } else if (editor.watch_active && editor.watch_locked &&
               editor.watch_region < memory_regions.size()) {
        const auto &watch_region = memory_regions[editor.watch_region];
        std::snprintf(line, sizeof(line), "FROZEN: %s $%06X = $%02X",
                      watch_region.name,
                      watch_region.base +
                          static_cast<unsigned>(editor.watch_offset),
                      current_watch_value());
        draw_text(panel_x + 258, 396, line,
                  SDL_Color{85, 180, 255, 255}, 1);
    } else {
        draw_text(panel_x + 258, 396, "LIVE: O JOGO PODE ALTERAR",
                  SDL_Color{160, 160, 160, 255}, 1);
    }

    const auto *correlation = focused_visual_correlation();
    const char axis = correlation && correlation->value_axis == 1 ? 'X' :
                      correlation && correlation->value_axis == 2 ? 'Y' : '?';
    std::snprintf(line, sizeof(line), "VISUAL %s  EIXO %c",
                  focused_visual_marker_ready() ? "OK" : "BAIXA",
                  axis);
    draw_text(panel_x + 258, 416, line,
              focused_visual_marker_ready() ? SDL_Color{110, 235, 150, 255}
                                            : SDL_Color{170, 170, 170, 255},
              1);
}

void draw_goto_popup(int panel_x) {
    const auto &editor = app.memory_editor;
    if (!editor.goto_popup) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 248);
    const SDL_Rect popup{panel_x + 44, 176, 424, 228};
    SDL_RenderFillRect(app.renderer, &popup);
    SDL_SetRenderDrawColor(app.renderer, 255, 190, 60, 255);
    SDL_RenderDrawRect(app.renderer, &popup);

    draw_text(popup.x + 18, popup.y + 18, "G - IR PARA MEMORIA",
              SDL_Color{255, 220, 90, 255});
    draw_text(popup.x + 18, popup.y + 48,
              "DIGITE O ENDERECO EM HEXADECIMAL",
              SDL_Color{180, 180, 180, 255}, 1);

    const SDL_Color address_color =
        !editor.goto_editing_value ? SDL_Color{255, 220, 90, 255}
                                   : SDL_Color{210, 210, 210, 255};
    const SDL_Color value_color =
        editor.goto_editing_value ? SDL_Color{255, 220, 90, 255}
                                  : SDL_Color{210, 210, 210, 255};
    char line[96];
    std::snprintf(line, sizeof(line), "ENDERECO  $%06X",
                  editor.goto_address);
    draw_text(popup.x + 18, popup.y + 82, line, address_color);
    std::snprintf(line, sizeof(line), "VALOR     $%02X",
                  editor.goto_value);
    draw_text(popup.x + 18, popup.y + 116, line, value_color);

    draw_text(popup.x + 18, popup.y + 158,
              "ENTER CONFIRMA  V EDITA VALOR\n"
              "BACKSPACE LIMPA  ESC FECHA",
              SDL_Color{170, 200, 225, 255}, 1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void draw_memory_debugger() {
    const int panel_x = app.debug_overlay ? game_width - debugger_width
                                          : game_width;
    SDL_SetRenderDrawColor(app.renderer, 8, 10, 18, 255);
    const SDL_Rect panel{panel_x, 0, debugger_width, game_height};
    SDL_RenderFillRect(app.renderer, &panel);
    SDL_SetRenderDrawColor(app.renderer, 75, 170, 220, 255);
    SDL_RenderDrawRect(app.renderer, &panel);

    draw_text(panel_x + 18, 20, "DEBUG DE MEMORIA",
              SDL_Color{255, 220, 90, 255});
    draw_text(panel_x + 18, 44, "I FECHA  [ ] SELECIONA  ENTER EDITA",
              SDL_Color{170, 170, 170, 255}, 1);

    draw_text(panel_x + 18, 68,
              app.custom_memory_watches.empty()
                  ? "ENDERECOS WRAM EM HEXADECIMAL"
                  : "MEMORIAS VIGIADAS",
              SDL_Color{255, 220, 90, 255}, 1);
    if (!app.custom_memory_watches.empty()) {
        const unsigned count =
            static_cast<unsigned>(app.custom_memory_watches.size());
        const unsigned selected =
            std::min(app.memory_editor.important_index, count - 1);
        const unsigned first = selected >= 9 ? selected - 9 : 0;
        for (unsigned row = 0; row < 10 && first + row < count; ++row) {
            const unsigned index = first + row;
            const auto &watch = app.custom_memory_watches[index];
            const auto *memory = static_cast<const uint8_t *>(
                retro_get_memory_data(memory_regions[watch.region].id));
            const size_t size =
                retro_get_memory_size(memory_regions[watch.region].id);
            const uint8_t value =
                memory && watch.offset < size ? memory[watch.offset] : 0;
            char important_line[128];
            std::snprintf(important_line, sizeof(important_line),
                          "%c%02u %-20.20s $%06X $%02X",
                          index == selected ? '>' : ' ', index + 1,
                          watch.label.c_str(), watch.address, value);
            const bool frozen_watch =
                app.memory_editor.watch_active &&
                app.memory_editor.watch_locked &&
                app.memory_editor.watch_region == watch.region &&
                app.memory_editor.watch_offset == watch.offset;
            draw_text(panel_x + 18, 88 + row * 16, important_line,
                      frozen_watch ? SDL_Color{85, 180, 255, 255}
                                   : index == selected
                                         ? SDL_Color{255, 220, 90, 255}
                                         : SDL_Color{220, 220, 220, 255},
                      1);
        }
    } else {
        const uint8_t *wram_memory = wram();
        for (unsigned index = 0; index < app.memory_activity.hottest.size(); ++index) {
            const size_t offset = app.memory_activity.hottest[index];
            const uint32_t score =
                offset < app.memory_activity.scores.size()
                    ? app.memory_activity.scores[offset]
                    : 0;
            char important_line[96];
            const char marker =
                app.memory_editor.focused_activity &&
                        offset == app.memory_editor.focused_activity_offset
                    ? '>'
                    : offset == app.memory_activity.player_candidate_offset &&
                              app.memory_activity.player_candidate_score > 0
                        ? 'P'
                        : ' ';
            std::snprintf(important_line, sizeof(important_line),
                          "%c $%06X = $%02X  ATIV $%04X",
                          marker,
                          0x7E0000U + static_cast<unsigned>(offset),
                          wram_memory &&
                                  offset <
                                      retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)
                              ? wram_memory[offset]
                              : 0,
                          score);
            draw_text(panel_x + 18, 88 + index * 16, important_line,
                      app.memory_editor.focused_activity &&
                              offset == app.memory_editor.focused_activity_offset
                          ? SDL_Color{255, 220, 90, 255}
                          : SDL_Color{220, 220, 220, 255},
                      1);
        }
    }

    draw_correlated_sprite_preview(panel_x);
    draw_watch_form(panel_x);

    char player_confidence[96];
    std::snprintf(player_confidence, sizeof(player_confidence),
                  "P = PROVAVEL PLAYER  SCORE $%04X",
                  app.memory_activity.player_candidate_score);
    draw_text(panel_x + 18, 236, player_confidence,
              SDL_Color{150, 200, 230, 255}, 1);

    const auto &region = selected_region();
    uint8_t *memory = selected_memory();
    const size_t size = selected_memory_size();
    char line[128];
    std::snprintf(line, sizeof(line), "%s  %s", region.name, region.map);
    draw_text(panel_x + 18, 438, "MAPA DE MEMORIA",
              SDL_Color{255, 220, 90, 255}, 1);
    draw_text(panel_x + 18, 456, line, SDL_Color{104, 226, 255, 255}, 1);
    if (!memory || !size) {
        draw_text(panel_x + 18, 474, "REGIAO INDISPONIVEL",
                  SDL_Color{255, 120, 120, 255});
        return;
    }

    std::snprintf(line, sizeof(line), "TAM $%06zX  HASH $%08X", size,
                  memory_hash(std::span<const uint8_t>(memory, size)));
    draw_text(panel_x + 18, 474, line, SDL_Color{170, 170, 170, 255}, 1);

    const size_t selected = std::min(app.memory_editor.offset, size - 1);
    const size_t first = (selected / 16 >= 1 ? selected / 16 - 1 : 0) * 16;
    draw_text(panel_x + 362, 482, "TEXTO",
              SDL_Color{120, 190, 230, 255}, 1);
    for (int row = 0; row < 4 && first + row * 16 < size; ++row) {
        const size_t row_offset = first + row * 16;
        std::snprintf(line, sizeof(line), "$%06X:",
                      region.base + static_cast<unsigned>(row_offset));
        draw_text(panel_x + 18, 498 + row * 20, line,
                  SDL_Color{120, 190, 230, 255}, 1);
        char ascii[17]{};
        for (int column = 0; column < 16 &&
                             row_offset + column < size; ++column) {
            const size_t offset = row_offset + column;
            const int x = panel_x + 74 + column * 20;
            const int y = 498 + row * 20;
            if (offset == selected) {
                SDL_SetRenderDrawColor(app.renderer, 230, 190, 55, 255);
                const SDL_Rect highlight{x - 2, y - 2, 16, 11};
                SDL_RenderFillRect(app.renderer, &highlight);
            }
            ascii[column] = visible_memory_char(memory[offset]);
            char byte[3];
            std::snprintf(byte, sizeof(byte), "%02X", memory[offset]);
            draw_text(x, y, byte,
                      offset == selected ? SDL_Color{15, 15, 20, 255}
                                         : SDL_Color{235, 235, 235, 255},
                      1);
        }
        for (int column = 0; column < 16 &&
                             row_offset + column < size; ++column) {
            const size_t offset = row_offset + column;
            const int x = panel_x + 362 + column * 7;
            const int y = 498 + row * 20;
            if (offset == selected) {
                SDL_SetRenderDrawColor(app.renderer, 230, 190, 55, 255);
                const SDL_Rect highlight{x - 1, y - 2, 8, 11};
                SDL_RenderFillRect(app.renderer, &highlight);
            }
            char letter[2]{ascii[column], '\0'};
            draw_text(x, y, letter,
                      offset == selected ? SDL_Color{15, 15, 20, 255}
                                         : SDL_Color{210, 240, 210, 255},
                      1);
        }
    }

    draw_text(panel_x + 18, 594, app.memory_editor.status,
              SDL_Color{110, 235, 150, 255}, 1);
    draw_text(panel_x + 18, 626,
              "ENTER EDITA  SPACE POKE  +/KP- MUDA\n"
              "F/L FREEZE  DEL LIMPA  TAB CAMPO\n"
              "G IR ENDERECO  T TEXTO  - LUA",
              SDL_Color{170, 170, 170, 255}, 1);
    draw_goto_popup(panel_x);
}

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
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char **>(data) = app.data_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback *>(data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool *>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return false;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
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
    app.video_pipeline = std::make_unique<snes::VideoPipeline>(
        [](const snes::VideoFrame &frame) {
            update_hash(frame.pixels.data(), frame.width, frame.height,
                        frame.pitch);
        });
    app.video_pipeline->start();
    if (app.audio) {
        app.audio_pipeline =
            std::make_unique<snes::AudioPipeline>(app.audio);
        app.audio_pipeline->start();
    }
}

void stop_media_workers() {
    if (app.video_pipeline) app.video_pipeline->stop();
    if (app.audio_pipeline) app.audio_pipeline->stop();
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
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

void draw_lua_script_editor();

void present_latest_frame() {
    const auto frame =
        app.video_pipeline ? app.video_pipeline->latest() : nullptr;
    if (!frame || (frame->serial == app.presented_serial &&
                   !app.lua_editor.active)) {
        return;
    }

    if (!app.texture || frame->width != app.texture_width ||
        frame->height != app.texture_height) {
        SDL_DestroyTexture(app.texture);
        const Uint32 format = frame->format == RETRO_PIXEL_FORMAT_XRGB8888
                                  ? SDL_PIXELFORMAT_ARGB8888
                                  : SDL_PIXELFORMAT_RGB565;
        app.texture = SDL_CreateTexture(
            app.renderer, format, SDL_TEXTUREACCESS_STREAMING,
            frame->width, frame->height);
        if (!app.texture) {
            std::cerr << "Falha ao criar textura: " << SDL_GetError() << '\n';
            app.running = false;
            return;
        }
        app.texture_width = frame->width;
        app.texture_height = frame->height;
    }

    SDL_UpdateTexture(app.texture, nullptr, frame->pixels.data(),
                      static_cast<int>(frame->pitch));
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app.renderer);
    const SDL_Rect game_area{0, 0, game_width, game_height};
    SDL_RenderCopy(app.renderer, app.texture, nullptr, &game_area);
    if (app.memory_debug) {
        draw_screen_memory_marker();
        draw_memory_debugger();
    }
    draw_lua_script_editor();
    SDL_RenderPresent(app.renderer);
    app.presented_serial = frame->serial;
}

std::string clip_text(std::string text, size_t width) {
    if (text.size() <= width) {
        return text;
    }
    if (width == 0) {
        return {};
    }
    text.resize(width - 1);
    text.push_back('>');
    return text;
}

void draw_lua_script_editor() {
    if (!app.lua_editor.active) {
        return;
    }
    auto &editor = app.lua_editor;
    const int margin = 24;
    const int header_h = 54;
    const int footer_h = 54;
    const int line_h = 18;
    const int char_w = 12;
    const int gutter_w = 58;
    const int editor_w = game_width - margin * 2;
    const int editor_h = game_height - margin * 2;
    const int text_rows = std::max(1, (editor_h - header_h - footer_h) / line_h);
    const int text_cols = std::max(1, (editor_w - gutter_w - 18) / char_w);

    if (editor.row < editor.row_offset) {
        editor.row_offset = editor.row;
    } else if (editor.row >= editor.row_offset + static_cast<size_t>(text_rows)) {
        editor.row_offset = editor.row - static_cast<size_t>(text_rows) + 1;
    }
    if (editor.col < editor.col_offset) {
        editor.col_offset = editor.col;
    } else if (editor.col >= editor.col_offset + static_cast<size_t>(text_cols)) {
        editor.col_offset = editor.col - static_cast<size_t>(text_cols) + 1;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 190);
    const SDL_Rect shade{0, 0, game_width, game_height};
    SDL_RenderFillRect(app.renderer, &shade);

    SDL_SetRenderDrawColor(app.renderer, 12, 16, 24, 252);
    const SDL_Rect panel{margin, margin, editor_w, editor_h};
    SDL_RenderFillRect(app.renderer, &panel);
    SDL_SetRenderDrawColor(app.renderer, 90, 190, 230, 255);
    SDL_RenderDrawRect(app.renderer, &panel);

    const std::string title =
        "EDITOR LUA  " + editor.path.string() + (editor.dirty ? "  +" : "");
    draw_text(margin + 16, margin + 14, clip_text(title, 78),
              SDL_Color{210, 240, 255, 255}, 2);
    draw_text(margin + 16, margin + 36,
              "CTRL-S SALVA  CTRL-R SALVA/RECARREGA  TAB COMPLETA  ESC FECHA",
              SDL_Color{150, 170, 185, 255}, 1);

    const int text_y = margin + header_h;
    for (int screen_row = 0; screen_row < text_rows; ++screen_row) {
        const size_t file_row = editor.row_offset + static_cast<size_t>(screen_row);
        const int y = text_y + screen_row * line_h;
        if (file_row >= editor.lines.size()) {
            draw_text(margin + 16, y, "~", SDL_Color{80, 90, 105, 255}, 2);
            continue;
        }

        char number[16];
        std::snprintf(number, sizeof(number), "%4zu", file_row + 1);
        const bool current = file_row == editor.row;
        if (current) {
            SDL_SetRenderDrawColor(app.renderer, 28, 42, 54, 255);
            const SDL_Rect row_box{margin + 8, y - 2, editor_w - 16, line_h};
            SDL_RenderFillRect(app.renderer, &row_box);
        }
        draw_text(margin + 16, y, number,
                  current ? SDL_Color{255, 210, 90, 255}
                          : SDL_Color{100, 120, 135, 255}, 2);

        const auto &line = editor.lines[file_row];
        const std::string visible =
            editor.col_offset < line.size()
                ? line.substr(editor.col_offset, static_cast<size_t>(text_cols))
                : "";
        draw_text(margin + gutter_w, y, visible,
                  SDL_Color{225, 230, 220, 255}, 2);
    }

    if (editor.row >= editor.row_offset &&
        editor.row < editor.row_offset + static_cast<size_t>(text_rows)) {
        const int cursor_x = margin + gutter_w +
            static_cast<int>(editor.col - editor.col_offset) * char_w;
        const int cursor_y = text_y +
            static_cast<int>(editor.row - editor.row_offset) * line_h;
        SDL_SetRenderDrawColor(app.renderer, 255, 225, 90, 255);
        const SDL_Rect cursor{cursor_x, cursor_y - 1, 2, line_h};
        SDL_RenderFillRect(app.renderer, &cursor);
    }

    const int footer_y = margin + editor_h - footer_h + 12;
    draw_text(margin + 16, footer_y, clip_text(editor.status, 78),
              SDL_Color{120, 235, 160, 255}, 1);
    draw_text(margin + 16, footer_y + 18,
              "API: SNES.READ8/WRITE8  PRESS/RELEASE  SET_BUTTON  CLEAR_INPUT  FRAME  LOG",
              SDL_Color{150, 170, 185, 255}, 1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
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
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }
    if (id < app.lua.buttons.size() && app.lua.buttons[id]) {
        return 1;
    }
    if (app.headless ||
        app.memory_editor.active || app.memory_editor.goto_popup ||
        app.lua_editor.active) {
        return 0;
    }
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
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
    auto *memory = static_cast<uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->load_sram({memory, memory_size});
        std::cout << "Save RAM carregado.\n";
    }
}

void save_sram() {
    const auto *memory =
        static_cast<const uint8_t *>(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM));
    const size_t memory_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (save_manager && memory && memory_size) {
        save_manager->save_sram({memory, memory_size});
    }
}

void save_state() {
    std::vector<uint8_t> state(retro_serialize_size());
    if (state.empty() || !retro_serialize(state.data(), state.size())) {
        std::cerr << "Nao foi possivel salvar o estado.\n";
        return;
    }
    if (save_manager && save_manager->save_state(state)) {
        std::cout << "Estado salvo.\n";
    }
}

void load_state() {
    std::vector<uint8_t> state;
    if (!save_manager || !save_manager->load_state(state) ||
        !retro_unserialize(state.data(), state.size())) {
        std::cerr << "Nao foi possivel carregar o estado.\n";
        return;
    }
    std::cout << "Estado carregado.\n";
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
    SDL_RenderSetLogicalSize(app.renderer, game_width, game_height);

    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(av.timing.sample_rate);
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    app.audio = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (app.audio) {
        SDL_PauseAudioDevice(app.audio, 0);
    } else {
        std::cerr << "Audio desativado: " << SDL_GetError() << '\n';
    }
    return true;
}

int hex_digit(SDL_Keycode key) {
    if (key >= SDLK_0 && key <= SDLK_9) {
        return static_cast<int>(key - SDLK_0);
    }
    if (key >= SDLK_a && key <= SDLK_f) {
        return static_cast<int>(key - SDLK_a + 10);
    }
    return -1;
}

void open_goto_popup() {
    auto &editor = app.memory_editor;
    if (!app.memory_debug) {
        app.memory_debug = true;
        set_debug_layout(true);
    }
    if (editor.text_mode) {
        set_text_editor_enabled(false);
    }
    if (!editor.goto_has_address) {
        editor.goto_address = editor.address_input;
        editor.goto_value = editor.value;
    }
    editor.goto_popup = true;
    editor.goto_editing_value = false;
    editor.goto_replace_on_type = true;
    editor.active = false;
    editor.status = "G ABERTO";
}

bool handle_goto_popup_key(SDL_Keycode key) {
    auto &editor = app.memory_editor;
    if (!editor.goto_popup) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        editor.goto_popup = false;
        editor.status = "G FECHADO";
        return true;
    }
    if (key == SDLK_g) {
        editor.goto_editing_value = false;
        editor.goto_replace_on_type = true;
        return true;
    }
    if (key == SDLK_v) {
        editor.goto_editing_value = true;
        editor.goto_replace_on_type = true;
        editor.status = "DIGITE O VALOR";
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        if (editor.goto_editing_value) {
            editor.goto_value = 0;
        } else {
            editor.goto_address = 0;
        }
        editor.goto_replace_on_type = true;
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (editor.goto_editing_value) {
            if (write_memory_address(editor.goto_address,
                                     editor.goto_value)) {
                focus_memory_address(editor.goto_address);
                editor.goto_has_address = true;
                editor.goto_popup = false;
            }
        } else if (focus_memory_address(editor.goto_address)) {
            editor.goto_has_address = true;
            editor.goto_value = editor.value;
            editor.goto_popup = false;
        }
        return true;
    }

    const int digit = hex_digit(key);
    if (digit < 0) {
        return true;
    }
    if (editor.goto_editing_value) {
        editor.goto_value = editor.goto_replace_on_type
            ? static_cast<uint8_t>(digit)
            : static_cast<uint8_t>((editor.goto_value << 4) | digit);
    } else {
        editor.goto_address = editor.goto_replace_on_type
            ? static_cast<uint32_t>(digit)
            : ((editor.goto_address << 4) | static_cast<uint32_t>(digit)) &
                  0xFFFFFFU;
    }
    editor.goto_replace_on_type = false;
    return true;
}

void select_memory_region(unsigned region) {
    app.memory_editor.region = region % memory_regions.size();
    app.memory_editor.offset = 0;
    app.memory_editor.address_input = selected_region().base;
    app.memory_editor.replace_on_type = true;
    app.memory_editor.address_valid = true;
    app.memory_editor.status = std::string("REGIAO ") + selected_region().name;
    clamp_editor_address();
}

void move_memory_address(int64_t delta) {
    const size_t size = selected_memory_size();
    if (!size) {
        return;
    }
    const int64_t current = static_cast<int64_t>(app.memory_editor.offset);
    app.memory_editor.offset = static_cast<size_t>(
        std::clamp<int64_t>(current + delta, 0,
                            static_cast<int64_t>(size - 1)));
    clamp_editor_address();
    app.memory_editor.replace_on_type = true;
}

void apply_hex_digit(int digit) {
    auto &editor = app.memory_editor;
    if (editor.editing_value) {
        editor.value = editor.replace_on_type
            ? static_cast<uint8_t>(digit)
            : static_cast<uint8_t>((editor.value << 4) | digit);
        editor.replace_on_type = false;
        editor.status = "ENTER OU SPACE PARA POKE";
        return;
    }

    editor.address_input = editor.replace_on_type
        ? static_cast<uint32_t>(digit)
        : ((editor.address_input << 4) | static_cast<uint32_t>(digit)) &
              0xFFFFFFU;
    editor.replace_on_type = false;

    const uint32_t base = selected_region().base;
    const size_t size = selected_memory_size();
    const uint64_t address = editor.address_input;
    if (address >= base && address - base < size) {
        editor.offset = static_cast<size_t>(address - base);
    } else if (address < size) {
        editor.offset = static_cast<size_t>(address);
    } else {
        editor.address_valid = false;
        editor.status = "ENDERECO FORA DA REGIAO";
        return;
    }
    editor.address_valid = true;
    if (auto *memory = selected_memory()) {
        editor.value = memory[editor.offset];
    }
    editor.status = "TAB PARA EDITAR VALOR";
}

void begin_selected_value_edit() {
    auto &editor = app.memory_editor;
    uint8_t current = 0;
    if (read_selected_memory_value(current)) {
        editor.value = current;
    }
    editor.active = true;
    editor.editing_value = true;
    editor.replace_on_type = true;
    editor.status = "DIGITE HEX, ENTER/SPACE POKE";
}

bool handle_memory_editor_key(SDL_Keycode key) {
    auto &editor = app.memory_editor;
    if (!app.memory_debug) {
        return false;
    }
    if (key == SDLK_t) {
        toggle_text_editor();
        return true;
    }
    if (editor.text_mode) {
        if (key == SDLK_ESCAPE) {
            set_text_editor_enabled(false);
            editor.active = false;
            return true;
        }
        if (key == SDLK_LEFT) {
            move_memory_address(-1);
            return true;
        }
        if (key == SDLK_RIGHT) {
            move_memory_address(1);
            return true;
        }
        if (key == SDLK_UP || key == SDLK_PAGEUP) {
            move_memory_address(-16);
            return true;
        }
        if (key == SDLK_DOWN || key == SDLK_PAGEDOWN) {
            move_memory_address(16);
            return true;
        }
        if (key == SDLK_BACKSPACE) {
            move_memory_address(-1);
            editor.value = 0;
            write_selected_memory();
            editor.status = "CARACTERE APAGADO";
            return true;
        }
        if (key == SDLK_DELETE) {
            editor.value = 0;
            write_selected_memory();
            editor.status = "CARACTERE APAGADO";
            return true;
        }
        return true;
    }
    if (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_KP_PLUS) {
        adjust_selected_memory_value(1);
        return true;
    }
    if (key == SDLK_KP_MINUS) {
        adjust_selected_memory_value(-1);
        return true;
    }
    if (key == SDLK_f || key == SDLK_l) {
        toggle_memory_freeze();
        return true;
    }
    if (key == SDLK_SPACE) {
        if (!editor.active || !editor.editing_value) {
            uint8_t current = 0;
            if (read_selected_memory_value(current)) {
                editor.value = current;
            }
        }
        if (write_selected_memory()) {
            editor.active = false;
        }
        return true;
    }
    if (key == SDLK_DELETE) {
        clear_memory_freeze();
        return true;
    }
    if (!editor.active) {
        if (key == SDLK_LEFTBRACKET) {
            select_important_memory(-1);
            focus_important_memory();
            jump_to_important_memory();
            return true;
        }
        if (key == SDLK_RIGHTBRACKET) {
            select_important_memory(1);
            focus_important_memory();
            jump_to_important_memory();
            return true;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            jump_to_important_memory();
            begin_selected_value_edit();
            return true;
        }
        if (key == SDLK_e) {
            editor.active = true;
            editor.editing_value = false;
            editor.replace_on_type = true;
            editor.status = "FORMULARIO DE ENDERECO ABERTO";
            clamp_editor_address();
            return true;
        }
        if (key == SDLK_r) {
            select_memory_region(editor.region + 1);
            return true;
        }
        return false;
    }

    if (key == SDLK_ESCAPE) {
        editor.active = false;
        editor.status = "EDICAO ENCERRADA";
        return true;
    }
    if (key == SDLK_TAB) {
        editor.editing_value = !editor.editing_value;
        editor.replace_on_type = true;
        editor.status = editor.editing_value ? "DIGITE O NOVO VALOR"
                                             : "DIGITE O ENDERECO";
        return true;
    }
    if (key == SDLK_r) {
        select_memory_region(editor.region + 1);
        return true;
    }
    if (key == SDLK_LEFT) {
        move_memory_address(-1);
        return true;
    }
    if (key == SDLK_RIGHT) {
        move_memory_address(1);
        return true;
    }
    if (key == SDLK_UP || key == SDLK_PAGEUP) {
        move_memory_address(-16);
        return true;
    }
    if (key == SDLK_DOWN || key == SDLK_PAGEDOWN) {
        move_memory_address(16);
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        if (editor.editing_value) {
            editor.value = 0;
        } else {
            editor.address_input = 0;
            editor.address_valid = false;
        }
        editor.replace_on_type = true;
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (editor.editing_value) {
            if (write_selected_memory()) {
                editor.active = false;
            }
        } else {
            if (!editor.address_valid) {
                editor.status = "ENDERECO FORA DA REGIAO";
                return true;
            }
            editor.editing_value = true;
            editor.replace_on_type = true;
            editor.status = "DIGITE O NOVO VALOR";
        }
        return true;
    }
    const int digit = hex_digit(key);
    if (digit >= 0) {
        apply_hex_digit(digit);
        return true;
    }
    return true;
}

void handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            app.running = false;
        }
        if (event.type == SDL_TEXTINPUT && app.lua_editor.active) {
            insert_lua_editor_text(event.text.text);
            continue;
        }
        if (event.type == SDL_TEXTINPUT && app.memory_editor.text_mode) {
            write_text_to_memory(event.text.text);
            continue;
        }
        if (event.type != SDL_KEYDOWN || event.key.repeat) {
            continue;
        }
        if (app.lua_editor.active) {
            handle_lua_editor_key(event.key.keysym.sym,
                                  static_cast<SDL_Keymod>(event.key.keysym.mod));
            continue;
        }
        if (event.key.keysym.sym == SDLK_MINUS) {
            open_lua_editor();
            continue;
        }
        if (event.key.keysym.sym == SDLK_g &&
            !app.memory_editor.goto_popup) {
            open_goto_popup();
            continue;
        }
        if (handle_goto_popup_key(event.key.keysym.sym)) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_i) {
            app.memory_debug = !app.memory_debug;
            if (!app.memory_debug) {
                if (app.memory_editor.text_mode) {
                    set_text_editor_enabled(false);
                }
                app.memory_editor.active = false;
            } else {
                focus_player_candidate();
            }
            set_debug_layout(app.memory_debug);
            continue;
        }
        if (handle_memory_editor_key(event.key.keysym.sym)) {
            continue;
        }
        switch (event.key.keysym.sym) {
        case SDLK_ESCAPE: app.running = false; break;
        case SDLK_F2: retro_reset(); break;
        case SDLK_F5: save_state(); break;
        case SDLK_F8: load_state(); break;
        case SDLK_F11: {
            const Uint32 flags = SDL_GetWindowFlags(app.window);
            const bool entering_fullscreen =
                !(flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_SetWindowFullscreen(
                app.window,
                entering_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            apply_debug_layout(app.memory_debug, entering_fullscreen);
            break;
        }
        case SDLK_p:
            app.paused = !app.paused;
            if (app.audio) {
                SDL_PauseAudioDevice(app.audio, app.paused ? 1 : 0);
            }
            break;
        default: break;
        }
    }
}

} // namespace

int snes::Application::run(int argc, char **argv) {
    const auto options = parse_launch_options(argc, argv);
    if (!options) return 0;
    const uint64_t frame_limit = options->frame_limit;
    rom_path = options->rom_path;
    app.lua_editor.path = options->script_path.empty()
        ? std::filesystem::path{"scripts/novo-script.lua"}
        : options->script_path;
    app.headless = options->headless;
    save_manager = std::make_unique<SaveManager>(rom_path);
    app.data_directory =
        std::filesystem::absolute(rom_path).parent_path().string();

    if (!save_manager->read_rom(rom)) {
        std::cerr << "Nao foi possivel ler a ROM: " << rom_path << '\n';
        return 1;
    }

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    const std::string rom_path_string = rom_path.string();
    retro_game_info game{
        rom_path_string.c_str(), rom.data(), rom.size(), nullptr};
    if (!retro_load_game(&game)) {
        std::cerr << "O core recusou a ROM.\n";
        retro_deinit();
        return 1;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    load_sram();
    load_memory_watchlist(options->watchlist_path);
    load_lua_script(options->script_path);

    retro_system_info system{};
    retro_get_system_info(&system);
    retro_system_av_info av{};
    retro_get_system_av_info(&av);
    std::cout << system.library_name << ' ' << system.library_version
              << " | " << av.timing.fps << " FPS | "
              << av.timing.sample_rate << " Hz\n";

    if (!app.headless && !init_sdl(av)) {
        retro_unload_game();
        retro_deinit();
        SDL_Quit();
        return 1;
    }
    if (!app.headless) {
        start_media_workers();
    }

    const auto frame_duration =
        std::chrono::duration<double>(1.0 / av.timing.fps);
    auto next_frame = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    while (app.running && (!frame_limit || frames < frame_limit)) {
        if (!app.headless) {
            handle_events();
        }
        if (!app.paused) {
            run_lua_frame();
            apply_memory_lock();
            retro_run();
            ++frames;
            ++app.lua.frame;
        }
        if (!app.headless) {
            present_latest_frame();
            next_frame +=
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    frame_duration);
            std::this_thread::sleep_until(next_frame);
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
    retro_unload_game();
    retro_deinit();
    if (app.audio) SDL_CloseAudioDevice(app.audio);
    SDL_DestroyTexture(app.texture);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();

    if (app.headless) {
        std::cout << "Teste concluido: " << frames
                  << " frames, hash de video 0x" << std::hex
                  << app.frame_hash << std::dec << '\n';
    }
    return 0;
}
