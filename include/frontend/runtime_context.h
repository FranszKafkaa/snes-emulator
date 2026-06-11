#pragma once

#include <SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "libretro.h"
#include "media_pipeline.h"
#include "save_manager.h"

namespace snes::frontend {

inline constexpr int game_width = 1024;
inline constexpr int game_height = 768;
inline constexpr int debugger_width = 512;

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
    enum class DrawKind { Text, Rect, Line };

    struct DrawCommand {
        DrawKind kind = DrawKind::Text;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int x2 = 0;
        int y2 = 0;
        SDL_Color color{255, 255, 255, 255};
        int scale = 1;
        bool filled = false;
        std::string text;
    };

    lua_State *state = nullptr;
    std::array<bool, 16> buttons{};
    std::vector<DrawCommand> draw_commands;
    uint64_t frame = 0;
    int speed_multiplier = 1;
    bool active = false;
};

struct ScriptEditorLauncher {
    std::filesystem::path path{"scripts/novo-script.lua"};
    std::filesystem::file_time_type last_write_time{};
    pid_t process = -1;
    bool has_timestamp = false;
    std::string status = "PRESSIONE - PARA ABRIR O SNES LUA STUDIO";
};

struct ScriptImportPrompt {
    bool active = false;
    std::vector<std::filesystem::path> scripts;
    size_t selected = 0;
    size_t scroll = 0;
    std::string status = "SELECIONE UM SCRIPT LUA";
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
    bool turbo = false;
    int turbo_multiplier = 12;
    bool memory_debug = false;
    bool debug_overlay = false;
    bool debug_saved_window = false;
    int debug_saved_x = SDL_WINDOWPOS_CENTERED;
    int debug_saved_y = SDL_WINDOWPOS_CENTERED;
    int debug_saved_w = 960;
    int debug_saved_h = 720;
    MemoryEditor memory_editor;
    MemoryActivity memory_activity;
    std::vector<CustomMemoryWatch> custom_memory_watches;
    LuaRuntime lua;
    ScriptEditorLauncher script_editor;
    ScriptImportPrompt script_import;
    std::unique_ptr<snes::VideoPipeline> video_pipeline;
    std::unique_ptr<snes::AudioPipeline> audio_pipeline;
    uint64_t presented_serial = 0;
    uint64_t frame_hash = 1469598103934665603ULL;
};

struct MemoryRegion {
    const char *name;
    unsigned id;
    uint32_t base;
    const char *map;
};

extern Frontend app;
extern std::vector<uint8_t> rom;
extern std::filesystem::path rom_path;
extern std::unique_ptr<snes::SaveManager> save_manager;
extern const std::array<MemoryRegion, 3> memory_regions;

void draw_text(int x, int y, const std::string &text, SDL_Color color, int scale = 2);
bool window_fullscreen();
int render_logical_width();
void apply_stretched_render_scale();
int effective_speed_multiplier();
uint32_t memory_hash(std::span<const uint8_t> memory);
uint8_t *wram();
const MemoryRegion &selected_region();
uint8_t *selected_memory();
size_t selected_memory_size();
bool frame_layout(unsigned width, unsigned height, size_t pitch,
                  size_t &row_bytes, size_t &frame_size);
bool resolve_memory_address(uint32_t address, unsigned &region_index, size_t &offset);
std::string trim(std::string text);
void load_memory_watchlist(const std::filesystem::path &path);
bool focus_memory_address(uint32_t address);
bool write_memory_address(uint32_t address, uint8_t value);
bool save_current_state();
bool load_current_state();

void register_lua_api(lua_State *state);
void load_lua_script(const std::filesystem::path &path);
void update_script_editor_timestamp();
void open_script_editor();
void open_lua_script_picker();
void import_lua_script_from_path(const std::filesystem::path &selected_path);
bool handle_lua_script_picker_key(SDL_Keycode key);
void draw_lua_script_picker();
void reload_script_if_changed();
void run_lua_frame();

void clamp_editor_address();
void apply_debug_layout(bool enabled, bool fullscreen);
void set_debug_layout(bool enabled);
bool write_selected_memory();
void set_text_editor_enabled(bool enabled);
void toggle_text_editor();
void write_text_to_memory(const char *text);
bool read_selected_memory_value(uint8_t &value);
void toggle_memory_freeze();
void clear_memory_freeze();
void adjust_selected_memory_value(int delta);
void apply_memory_lock();
uint8_t current_watch_value();
uint32_t memory_address(unsigned region, size_t offset);
const CustomMemoryWatch *memory_watch_for(unsigned region, size_t offset);
int memory_watch_index_for(unsigned region, size_t offset);
std::string memory_label(unsigned region, size_t offset);
bool selected_memory_is_frozen();
bool focused_visual_marker_ready();
void select_important_memory(int delta);
void focus_important_memory();
void focus_player_candidate();
void jump_to_important_memory();
void track_memory_activity(const void *frame, unsigned width, unsigned height, size_t pitch);
void draw_screen_memory_marker();
void draw_memory_debugger();

void core_log(enum retro_log_level level, const char *format, ...);
bool environment(unsigned command, void *data);
void update_hash(const void *data, unsigned width, unsigned height, size_t pitch);
void start_media_workers();
void stop_media_workers();
void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch);
void present_latest_frame();
size_t audio_batch(const int16_t *data, size_t frames);
void audio_sample(int16_t left, int16_t right);
void input_poll();
int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id);
void load_sram();
void save_sram();
void save_state();
void load_state();
bool init_sdl(const retro_system_av_info &av);

void handle_events();

} // namespace snes::frontend
