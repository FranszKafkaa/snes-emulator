#include "frontend/runtime_context.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace snes::frontend {

namespace {

constexpr size_t large_memory_threshold = 512U * 1024U;
constexpr size_t large_memory_scan_budget = 256U * 1024U;
constexpr size_t large_memory_hottest_budget = 128U * 1024U;

bool large_memory_region(size_t size) {
    return size > large_memory_threshold;
}

} // namespace

struct N64ActorGuess {
    size_t offset = 0;
    uint32_t address = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t score = 0;
    uint16_t stability = 0;
    bool found = false;
};

struct NamedStructField {
    std::string name;
    size_t offset = 0;
};

struct PlayerStructSignature {
    std::string name = "N64 actor/player";
    bool loaded_from_file = false;
    bool has_actor_id = true;
    uint16_t actor_id = 0;
    size_t actor_id_offset = 0x0000;
    size_t struct_size = 0x0180;
    size_t pos_x = 0x0024;
    size_t pos_y = 0x0028;
    size_t pos_z = 0x002c;
    size_t rot_x = 0x0030;
    size_t rot_y = 0x0032;
    size_t rot_z = 0x0034;
    size_t vel_x = 0x005c;
    size_t vel_y = 0x0060;
    size_t vel_z = 0x0064;
    size_t prev = 0x0118;
    size_t next = 0x011c;
    size_t update_fn = 0x0130;
    size_t draw_fn = 0x0134;
    size_t heap_min = 0x100000;
    size_t heap_max = 0x500000;
    std::vector<NamedStructField> fields;
};

N64ActorGuess find_link_actor_guess();
std::vector<N64ActorGuess> find_n64_actor_structs();
PlayerStructSignature &mutable_player_struct_signature();
const PlayerStructSignature &player_struct_signature();

struct SnesSpriteStruct {
    int index = -1;
    SDL_Rect screen{};
    int width = 0;
    int height = 0;
    uint16_t tile = 0;
    uint8_t palette = 0;
    uint32_t score = 0;
    uint16_t stability = 0;
    bool found = false;
};

SnesSpriteStruct find_snes_player_struct();
std::vector<SnesSpriteStruct> find_snes_sprite_structs();

bool selected_n64_link_struct() {
    if (app.system != ConsoleSystem::N64 || app.memory_editor.region != 0) {
        return false;
    }
    const N64ActorGuess link = find_link_actor_guess();
    return link.found && app.memory_editor.offset >= link.offset &&
           app.memory_editor.offset <
               link.offset + player_struct_signature().struct_size;
}

bool selected_snes_sprite_struct() {
    if (app.system != ConsoleSystem::Snes) {
        return false;
    }
    return app.memory_editor.focused_activity &&
           app.memory_editor.focused_activity_offset < 128;
}

void draw_3d_box(const SDL_Rect &front, SDL_Color color, int depth) {
    const SDL_Rect back{front.x + depth, front.y - depth,
                        front.w, front.h};
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, 215);
    for (int inset = 0; inset < 2; ++inset) {
        const SDL_Rect f{front.x + inset, front.y + inset,
                         front.w - inset * 2, front.h - inset * 2};
        const SDL_Rect b{back.x + inset, back.y + inset,
                         back.w - inset * 2, back.h - inset * 2};
        SDL_RenderDrawRect(app.renderer, &f);
        SDL_RenderDrawRect(app.renderer, &b);
    }
    SDL_RenderDrawLine(app.renderer, front.x, front.y, back.x, back.y);
    SDL_RenderDrawLine(app.renderer, front.x + front.w, front.y,
                       back.x + back.w, back.y);
    SDL_RenderDrawLine(app.renderer, front.x, front.y + front.h,
                       back.x, back.y + back.h);
    SDL_RenderDrawLine(app.renderer, front.x + front.w, front.y + front.h,
                       back.x + back.w, back.y + back.h);
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, 32);
    SDL_RenderFillRect(app.renderer, &front);
}

bool write_memory_address(uint32_t address, uint8_t value) {
    unsigned region_index = 0;
    size_t offset = 0;
    if (!resolve_memory_address(address, region_index, offset)) {
        app.memory_editor.status = "ENDERECO FORA DAS REGIOES";
        return false;
    }
    auto *memory = static_cast<uint8_t *>(
        core.get_memory_data(memory_regions[region_index].id));
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
    if (!fullscreen) {
        if (enabled) {
            if (!app.debug_saved_window) {
                SDL_GetWindowPosition(app.window,
                                      &app.debug_saved_x,
                                      &app.debug_saved_y);
                SDL_GetWindowSize(app.window,
                                  &app.debug_saved_w,
                                  &app.debug_saved_h);
                app.debug_saved_window = true;
            }
            const int content_width = game_logical_width();
            const int debug_width =
                app.debug_saved_w +
                app.debug_saved_w * debugger_width / content_width;
            SDL_SetWindowSize(app.window, debug_width, app.debug_saved_h);
            SDL_SetWindowPosition(app.window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
        } else if (app.debug_saved_window) {
            SDL_SetWindowSize(app.window, app.debug_saved_w, app.debug_saved_h);
            SDL_SetWindowPosition(app.window,
                                  app.debug_saved_x,
                                  app.debug_saved_y);
            app.debug_saved_window = false;
        }
    }
    apply_stretched_render_scale();
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
        core.get_memory_data(memory_regions[editor.watch_region].id));
    const size_t size =
        core.get_memory_size(memory_regions[editor.watch_region].id);
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
        core.get_memory_data(memory_regions[editor.watch_region].id));
    const size_t size =
        core.get_memory_size(memory_regions[editor.watch_region].id);
    return memory && editor.watch_offset < size
        ? memory[editor.watch_offset]
        : 0;
}

const char *watch_type_name(CustomMemoryWatch::ValueType type) {
    switch (type) {
    case CustomMemoryWatch::ValueType::U8: return "u8";
    case CustomMemoryWatch::ValueType::S8: return "s8";
    case CustomMemoryWatch::ValueType::U16: return "u16";
    case CustomMemoryWatch::ValueType::S16: return "s16";
    case CustomMemoryWatch::ValueType::U32: return "u32";
    case CustomMemoryWatch::ValueType::S32: return "s32";
    case CustomMemoryWatch::ValueType::F32: return "f32";
    case CustomMemoryWatch::ValueType::BE16: return "be16";
    case CustomMemoryWatch::ValueType::BE32: return "be32";
    }
    return "u8";
}

const char *watch_trigger_name(CustomMemoryWatch::TriggerKind trigger) {
    switch (trigger) {
    case CustomMemoryWatch::TriggerKind::None: return "";
    case CustomMemoryWatch::TriggerKind::Change: return "chg";
    case CustomMemoryWatch::TriggerKind::Eq: return "eq";
    case CustomMemoryWatch::TriggerKind::Ne: return "ne";
    case CustomMemoryWatch::TriggerKind::Gt: return "gt";
    case CustomMemoryWatch::TriggerKind::Lt: return "lt";
    }
    return "";
}

size_t watch_type_size(CustomMemoryWatch::ValueType type) {
    switch (type) {
    case CustomMemoryWatch::ValueType::U8:
    case CustomMemoryWatch::ValueType::S8:
        return 1;
    case CustomMemoryWatch::ValueType::U16:
    case CustomMemoryWatch::ValueType::S16:
    case CustomMemoryWatch::ValueType::BE16:
        return 2;
    case CustomMemoryWatch::ValueType::U32:
    case CustomMemoryWatch::ValueType::S32:
    case CustomMemoryWatch::ValueType::F32:
    case CustomMemoryWatch::ValueType::BE32:
        return 4;
    }
    return 1;
}

bool read_watch_raw_value(const CustomMemoryWatch &watch, uint64_t &value) {
    if (watch.region >= memory_regions.size()) {
        return false;
    }
    const auto *memory = static_cast<const uint8_t *>(
        core.get_memory_data(memory_regions[watch.region].id));
    const size_t size = core.get_memory_size(memory_regions[watch.region].id);
    const size_t bytes = watch_type_size(watch.type);
    if (!memory || watch.offset + bytes > size) {
        return false;
    }
    if (watch.type == CustomMemoryWatch::ValueType::BE16) {
        value = (static_cast<uint64_t>(memory[watch.offset]) << 8U) |
                memory[watch.offset + 1];
        return true;
    }
    if (watch.type == CustomMemoryWatch::ValueType::BE32) {
        value = (static_cast<uint64_t>(memory[watch.offset]) << 24U) |
                (static_cast<uint64_t>(memory[watch.offset + 1]) << 16U) |
                (static_cast<uint64_t>(memory[watch.offset + 2]) << 8U) |
                memory[watch.offset + 3];
        return true;
    }
    value = 0;
    for (size_t index = 0; index < bytes; ++index) {
        value |= static_cast<uint64_t>(memory[watch.offset + index])
                 << (index * 8U);
    }
    return true;
}

int64_t watch_compare_value(const CustomMemoryWatch &watch, uint64_t raw) {
    switch (watch.type) {
    case CustomMemoryWatch::ValueType::S8:
        return static_cast<int8_t>(raw & 0xffU);
    case CustomMemoryWatch::ValueType::S16:
        return static_cast<int16_t>(raw & 0xffffU);
    case CustomMemoryWatch::ValueType::S32:
        return static_cast<int32_t>(raw & 0xffffffffU);
    default:
        return static_cast<int64_t>(raw);
    }
}

std::string format_watch_value(const CustomMemoryWatch &watch, uint64_t raw) {
    char text[64];
    switch (watch.type) {
    case CustomMemoryWatch::ValueType::S8:
        std::snprintf(text, sizeof(text), "%+d",
                      static_cast<int>(static_cast<int8_t>(raw & 0xffU)));
        break;
    case CustomMemoryWatch::ValueType::S16:
        std::snprintf(text, sizeof(text), "%+d",
                      static_cast<int>(static_cast<int16_t>(raw & 0xffffU)));
        break;
    case CustomMemoryWatch::ValueType::S32:
        std::snprintf(text, sizeof(text), "%+d",
                      static_cast<int32_t>(raw & 0xffffffffU));
        break;
    case CustomMemoryWatch::ValueType::F32: {
        const uint32_t bits = static_cast<uint32_t>(raw & 0xffffffffU);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        std::snprintf(text, sizeof(text), "%.3f", value);
        break;
    }
    case CustomMemoryWatch::ValueType::U16:
    case CustomMemoryWatch::ValueType::BE16:
        std::snprintf(text, sizeof(text), "$%04llX",
                      static_cast<unsigned long long>(raw & 0xffffU));
        break;
    case CustomMemoryWatch::ValueType::U32:
    case CustomMemoryWatch::ValueType::BE32:
        std::snprintf(text, sizeof(text), "$%08llX",
                      static_cast<unsigned long long>(raw & 0xffffffffU));
        break;
    case CustomMemoryWatch::ValueType::U8:
    default:
        std::snprintf(text, sizeof(text), "$%02llX",
                      static_cast<unsigned long long>(raw & 0xffU));
        break;
    }
    return text;
}

void evaluate_memory_triggers() {
    for (auto &watch : app.custom_memory_watches) {
        uint64_t raw = 0;
        if (!read_watch_raw_value(watch, raw)) {
            continue;
        }
        bool hit = false;
        const int64_t value = watch_compare_value(watch, raw);
        switch (watch.trigger) {
        case CustomMemoryWatch::TriggerKind::None:
            break;
        case CustomMemoryWatch::TriggerKind::Change:
            hit = watch.has_last_value && raw != watch.last_value;
            break;
        case CustomMemoryWatch::TriggerKind::Eq:
            hit = value == watch.trigger_value;
            break;
        case CustomMemoryWatch::TriggerKind::Ne:
            hit = value != watch.trigger_value;
            break;
        case CustomMemoryWatch::TriggerKind::Gt:
            hit = value > watch.trigger_value;
            break;
        case CustomMemoryWatch::TriggerKind::Lt:
            hit = value < watch.trigger_value;
            break;
        }
        watch.last_value = raw;
        watch.has_last_value = true;
        if (!hit) {
            watch.trigger_latched = false;
            continue;
        }
        if (watch.trigger_latched) {
            continue;
        }
        watch.trigger_latched = true;
        char status[128];
        std::snprintf(status, sizeof(status), "TRIGGER %s $%06X %s",
                      watch.label.c_str(), watch.address,
                      format_watch_value(watch, raw).c_str());
        app.memory_editor.status = status;
        std::cout << "[debug] " << status << '\n';
        if (watch.trigger_pause && !app.headless) {
            app.paused = true;
            if (app.audio) {
                SDL_PauseAudioDevice(app.audio, 1);
            }
        }
        break;
    }
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
    unsigned count = 10U;
    if (!app.custom_memory_watches.empty()) {
        count = static_cast<unsigned>(app.custom_memory_watches.size());
    } else if (app.system == ConsoleSystem::Snes) {
        count = static_cast<unsigned>(find_snes_sprite_structs().size());
    } else if (app.system == ConsoleSystem::N64) {
        count = static_cast<unsigned>(find_n64_actor_structs().size());
    }
    if (!count) {
        return;
    }
    index = static_cast<unsigned>(
        (static_cast<int>(index) + delta + static_cast<int>(count)) %
        static_cast<int>(count));
}

void focus_important_memory() {
    auto &editor = app.memory_editor;
    if (app.system == ConsoleSystem::Snes &&
        app.custom_memory_watches.empty()) {
        const auto structs = find_snes_sprite_structs();
        if (structs.empty()) {
            editor.status = "SEM STRUCT DE SPRITE";
            return;
        }
        editor.important_index %= structs.size();
        const auto &sprite = structs[editor.important_index];
        editor.focused_activity_offset = static_cast<size_t>(sprite.index);
        editor.focused_activity = true;
        editor.focused_screen_region = sprite.screen;
        editor.focused_region_frozen = true;
        editor.region = 1;
        editor.offset = static_cast<size_t>(sprite.tile) * 32U;
        clamp_editor_address();
        editor.status = "STRUCT DE SPRITE SELECIONADA";
        return;
    }
    if (app.system == ConsoleSystem::N64 &&
        app.custom_memory_watches.empty()) {
        const auto structs = find_n64_actor_structs();
        if (structs.empty()) {
            editor.status = "SEM STRUCT N64";
            return;
        }
        editor.important_index %= structs.size();
        const auto &actor = structs[editor.important_index];
        editor.focused_activity_offset = actor.offset;
        editor.focused_activity = true;
        editor.focused_screen_region = {};
        editor.focused_region_frozen = false;
        editor.region = 0;
        editor.offset = actor.offset;
        app.memory_activity.player_candidate_offset = actor.offset;
        app.memory_activity.player_candidate_score = actor.score;
        clamp_editor_address();
        editor.status = "STRUCT N64 SELECIONADA";
        return;
    }
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
    if (app.system == ConsoleSystem::N64) {
        const N64ActorGuess link = find_link_actor_guess();
        if (link.found) {
            editor.focused_activity_offset = link.offset;
            editor.focused_activity = true;
            editor.focused_screen_region = {};
            editor.focused_region_frozen = false;
            editor.region = 0;
            editor.offset = link.offset;
            app.memory_activity.player_candidate_offset = link.offset;
            app.memory_activity.player_candidate_score = link.score;
            app.memory_activity.player_struct_score = link.score;
            app.memory_activity.player_struct_stability = link.stability;
            clamp_editor_address();
            editor.status = "STRUCT DO PLAYER SELECIONADA";
            return;
        }
    }
    if (app.system == ConsoleSystem::Snes) {
        const SnesSpriteStruct sprite = find_snes_player_struct();
        if (sprite.found) {
            editor.focused_activity_offset = static_cast<size_t>(sprite.index);
            editor.focused_activity = true;
            editor.focused_screen_region = sprite.screen;
            editor.focused_region_frozen = true;
            editor.region = 1;
            editor.offset = static_cast<size_t>(sprite.tile) * 32U;
            app.memory_activity.player_candidate_offset =
                static_cast<size_t>(sprite.index);
            app.memory_activity.player_candidate_score = sprite.score;
            app.memory_activity.player_struct_score = sprite.score;
            app.memory_activity.player_struct_stability = sprite.stability;
            clamp_editor_address();
            editor.status = "STRUCT DE SPRITE DO PLAYER";
            return;
        }
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
    if (app.system == ConsoleSystem::Snes &&
        app.custom_memory_watches.empty()) {
        focus_important_memory();
        return;
    }
    if (app.system == ConsoleSystem::N64 &&
        app.custom_memory_watches.empty()) {
        focus_important_memory();
        return;
    }
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
    const size_t score_count = activity.scores.size();
    const bool partial = large_memory_region(score_count);
    const size_t start = partial ? activity.hottest_scan_cursor : 0;
    const size_t budget = partial
        ? std::min(large_memory_hottest_budget, score_count)
        : score_count;
    const size_t end = std::min(score_count, start + budget);
    auto scan_score = [&](size_t offset) {
        const uint32_t score = activity.scores[offset];
        if (score <= best.back().first) {
            return;
        }
        best.back() = {score, offset};
        for (size_t index = best.size() - 1;
             index > 0 && best[index].first > best[index - 1].first;
             --index) {
            std::swap(best[index], best[index - 1]);
        }
    };
    for (size_t offset : activity.hottest) {
        if (offset < score_count) {
            scan_score(offset);
        }
    }
    for (size_t offset = start; offset < end; ++offset) {
        scan_score(offset);
    }
    if (partial) {
        activity.hottest_scan_cursor = end >= score_count ? 0 : end;
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
    if (!app.memory_debug) {
        return;
    }
    uint8_t *memory = wram();
    const size_t memory_size =
        core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!memory || !memory_size || !frame) {
        return;
    }

    auto &activity = app.memory_activity;
    if (activity.previous_wram.size() != memory_size) {
        activity.previous_wram.assign(memory, memory + memory_size);
        activity.scores.assign(memory_size, 0);
        activity.screen_regions.assign(memory_size, SDL_Rect{});
        activity.correlations.assign(memory_size, {});
        activity.scan_cursor = 0;
        activity.hottest_scan_cursor = 0;
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

    const bool partial_scan = large_memory_region(memory_size);
    const size_t scan_start = partial_scan ? activity.scan_cursor : 0;
    const size_t scan_budget = partial_scan
        ? std::min(large_memory_scan_budget, memory_size)
        : memory_size;
    const size_t scan_end = std::min(memory_size, scan_start + scan_budget);
    for (size_t offset = scan_start; offset < scan_end; ++offset) {
        if (memory[offset] == activity.previous_wram[offset]) {
            continue;
        }
        const uint8_t value = memory[offset];
        activity.previous_wram[offset] = value;
        activity.scores[offset] =
            std::min<uint32_t>(activity.scores[offset] + 64, 0xFFFF);
        update_visual_correlation(offset, value, changed_regions);
    }
    if (partial_scan) {
        activity.scan_cursor = scan_end >= memory_size ? 0 : scan_end;
    }

    ++activity.frames;
    if (activity.frames % 30 == 0) {
        update_hottest_addresses();
        if (partial_scan) {
            for (size_t offset = scan_start; offset < scan_end; ++offset) {
                activity.scores[offset] = activity.scores[offset] * 15 / 16;
            }
        } else {
            for (uint32_t &score : activity.scores) {
                score = score * 15 / 16;
            }
        }
    }
}

void draw_screen_memory_marker() {
    const auto &editor = app.memory_editor;
    const bool link_struct = selected_n64_link_struct();
    const bool snes_struct = selected_snes_sprite_struct();
    const SnesSpriteStruct snes_player =
        app.system == ConsoleSystem::Snes ? find_snes_player_struct()
                                          : SnesSpriteStruct{};
    const bool snes_auto_player = app.system == ConsoleSystem::Snes &&
                                  snes_player.found && !snes_struct;
    if (!focused_visual_marker_ready() && !link_struct && !snes_struct &&
        !snes_auto_player) {
        return;
    }
    size_t offset = editor.focused_activity_offset;
    SDL_Rect marker = editor.focused_screen_region;
    if ((marker.w <= 0 || marker.h <= 0) && link_struct) {
        marker = SDL_Rect{game_width / 2 - 42, game_height * 52 / 100,
                          84, 138};
    }
    if ((marker.w <= 0 || marker.h <= 0) && snes_struct) {
        const auto structs = find_snes_sprite_structs();
        for (const auto &sprite : structs) {
            if (sprite.index ==
                static_cast<int>(app.memory_editor.focused_activity_offset)) {
                marker = sprite.screen;
                break;
            }
        }
        if (marker.w <= 0 || marker.h <= 0) {
            marker = find_snes_player_struct().screen;
        }
    }
    if ((marker.w <= 0 || marker.h <= 0) && snes_auto_player) {
        marker = snes_player.screen;
        offset = static_cast<size_t>(snes_player.index);
    }
    if (marker.w <= 0 || marker.h <= 0) {
        return;
    }
    const SDL_Color marker_color = selected_memory_is_frozen()
        ? SDL_Color{85, 180, 255, 255}
        : (snes_struct || snes_auto_player) ? SDL_Color{110, 235, 150, 255}
                                            : SDL_Color{255, 220, 90, 255};
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    if (link_struct) {
        draw_3d_box(marker, SDL_Color{110, 235, 150, 255},
                    std::max(10, marker.w / 5));
    }
    SDL_SetRenderDrawColor(app.renderer, marker_color.r, marker_color.g,
                           marker_color.b, link_struct ? 18 : 42);
    if (!link_struct) {
        SDL_RenderFillRect(app.renderer, &marker);
    }
    SDL_SetRenderDrawColor(app.renderer, marker_color.r, marker_color.g,
                           marker_color.b, 255);
    if (!link_struct) {
        for (int inset = 0; inset < 3; ++inset) {
            const SDL_Rect border{marker.x + inset, marker.y + inset,
                                  marker.w - inset * 2, marker.h - inset * 2};
            SDL_RenderDrawRect(app.renderer, &border);
        }
    }

    char badge[8];
    const int watch_index = memory_watch_index_for(0, offset);
    if (watch_index >= 0) {
        std::snprintf(badge, sizeof(badge), "%02d", watch_index + 1);
    } else if (snes_auto_player) {
        std::snprintf(badge, sizeof(badge), "P");
    } else if (snes_struct) {
        std::snprintf(badge, sizeof(badge), "S");
    } else if (link_struct) {
        std::snprintf(badge, sizeof(badge), "P");
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
    if (app.system != ConsoleSystem::Snes) {
        return nullptr;
    }
    if (void *symbol = core.symbol("PPU")) {
        return reinterpret_cast<CoreSPPU *>(symbol);
    }
    return reinterpret_cast<CoreSPPU *>(core.symbol("_PPU"));
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

std::vector<SnesSpriteStruct> find_snes_sprite_structs() {
    const CoreSPPU *ppu = core_ppu();
    const auto *vram = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_VIDEO_RAM));
    if (!ppu || !vram ||
        core.get_memory_size(RETRO_MEMORY_VIDEO_RAM) < 0x10000) {
        return {};
    }

    static int locked_index = -1;
    static uint16_t locked_stability = 0;
    std::vector<SnesSpriteStruct> result;
    result.reserve(16);

    for (int index = 0; index < 128; ++index) {
        const CoreSOBJ &sprite = ppu->OBJ[index];
        const auto [width, height] = sprite_dimensions(sprite);
        if (sprite.VPos >= 240 || sprite.HPos <= -width || sprite.HPos >= 256) {
            continue;
        }
        if (!sprite_has_pixels(vram, sprite, width, height)) {
            continue;
        }

        const SDL_Rect rect = sprite_screen_rect(sprite, width, height);
        const int area = rect.w * rect.h;
        if (area < 48) {
            continue;
        }

        const int center_x = rect.x + rect.w / 2;
        const int center_y = rect.y + rect.h / 2;
        const float horizontal =
            1.0f - std::min(1.0f,
                            std::abs(center_x - game_width / 2.0f) /
                                (game_width / 2.0f));
        const float lower_playfield =
            std::clamp(center_y / static_cast<float>(game_height), 0.0f, 1.0f);
        const float size_score =
            std::min(1.0f, area / static_cast<float>(game_width * game_height / 12));

        uint32_t score = static_cast<uint32_t>(
            size_score * 620.0f + horizontal * 280.0f +
            lower_playfield * 300.0f);
        score += static_cast<uint32_t>(std::max(width, height) * 6);
        score += static_cast<uint32_t>((127 - index) / 2);
        if (index == locked_index) {
            score += static_cast<uint32_t>(locked_stability) * 36U;
        }

        result.push_back(SnesSpriteStruct{
            index,
            rect,
            width,
            height,
            sprite.Name,
            sprite.Palette,
            score,
            static_cast<uint16_t>(index == locked_index ? locked_stability : 0),
            true,
        });
    }

    std::sort(result.begin(), result.end(),
              [](const SnesSpriteStruct &left,
                 const SnesSpriteStruct &right) {
                  return left.score > right.score;
              });
    if (result.size() > 10) {
        result.resize(10);
    }

    if (!result.empty()) {
        if (locked_index == result.front().index) {
            locked_stability = std::min<uint16_t>(locked_stability + 1, 120);
        } else if (locked_stability == 0 ||
                   result.front().score >
                       (app.memory_activity.player_struct_score + 180)) {
            locked_index = result.front().index;
            locked_stability = 1;
        } else {
            for (auto &candidate : result) {
                if (candidate.index == locked_index) {
                    candidate.score += static_cast<uint32_t>(locked_stability) * 36U;
                    break;
                }
            }
            std::sort(result.begin(), result.end(),
                      [](const SnesSpriteStruct &left,
                         const SnesSpriteStruct &right) {
                          return left.score > right.score;
                      });
        }
        for (auto &candidate : result) {
            if (candidate.index == locked_index) {
                candidate.stability = locked_stability;
            }
        }
    }

    return result;
}

SnesSpriteStruct find_snes_player_struct() {
    const auto structs = find_snes_sprite_structs();
    if (structs.empty()) {
        app.memory_activity.player_struct_score = 0;
        app.memory_activity.player_struct_stability = 0;
        return {};
    }
    const auto player = structs.front();
    app.memory_activity.player_candidate_offset =
        static_cast<size_t>(player.index);
    app.memory_activity.player_candidate_score = player.score;
    app.memory_activity.player_struct_score = player.score;
    app.memory_activity.player_struct_stability = player.stability;
    return player;
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
    auto sprite_for_index = [&](int index) {
        SpriteSelection selection;
        if (index < 0 || index >= 128) {
            return selection;
        }
        const CoreSOBJ &sprite = ppu->OBJ[index];
        const auto [width, height] = sprite_dimensions(sprite);
        if (sprite.VPos >= 240 || sprite.HPos <= -width ||
            sprite.HPos >= 256 ||
            !sprite_has_pixels(vram, sprite, width, height)) {
            return selection;
        }
        selection = {index, &sprite, width, height};
        return selection;
    };

    if (app.system == ConsoleSystem::Snes) {
        if (app.memory_editor.focused_activity &&
            app.memory_editor.focused_activity_offset < 128) {
            if (auto selection = sprite_for_index(
                    static_cast<int>(app.memory_editor.focused_activity_offset));
                selection.sprite) {
                return selection;
            }
        }
        const SnesSpriteStruct player = find_snes_player_struct();
        if (player.found) {
            if (auto selection = sprite_for_index(player.index);
                selection.sprite) {
                return selection;
            }
        }
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

uint16_t read_u16_be(const uint8_t *memory, size_t size, size_t offset) {
    if (!memory || offset + 1 >= size) {
        return 0;
    }
    return static_cast<uint16_t>((memory[offset] << 8) | memory[offset + 1]);
}

uint32_t read_u32_be(const uint8_t *memory, size_t size, size_t offset) {
    if (!memory || offset + 3 >= size) {
        return 0;
    }
    return (static_cast<uint32_t>(memory[offset]) << 24U) |
           (static_cast<uint32_t>(memory[offset + 1]) << 16U) |
           (static_cast<uint32_t>(memory[offset + 2]) << 8U) |
           static_cast<uint32_t>(memory[offset + 3]);
}

float read_f32_be(const uint8_t *memory, size_t size, size_t offset) {
    const uint32_t bits = read_u32_be(memory, size, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool n64_rdram_address(uint32_t value) {
    const size_t ram_size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const uint32_t physical = value & 0x1fffffffU;
    return ram_size && value >= 0x80000000U && value <= 0x807fffffU &&
           physical < ram_size;
}

bool n64_segmented_address(uint32_t value) {
    const uint8_t segment = static_cast<uint8_t>(value >> 24U);
    const uint32_t offset = value & 0x00ffffffU;
    return segment >= 0x02 && segment <= 0x0f && offset < 0x800000U;
}

const char *n64_segment_hint(uint8_t segment) {
    switch (segment) {
    case 0x04: return "SCENE/ROOM ASSET";
    case 0x05: return "OBJECT/ACTOR ASSET";
    case 0x06: return "OBJECT/ANIM/MODEL";
    case 0x07: return "DISPLAY LIST/TEXTURE";
    case 0x08: return "SKYBOX/EFFECT ASSET";
    default: return "ASSET SEGMENTADO";
    }
}

struct N64FieldMeaning {
    const char *kind = "RUNTIME";
    std::string detail;
    SDL_Color color{220, 220, 220, 255};
};

bool plausible_world_coordinate(float value) {
    return std::isfinite(value) && value > -20000.0f && value < 20000.0f;
}

bool plausible_actor_angle(uint16_t value) {
    return value != 0xffffU;
}

bool parse_size_value(const std::string &text, size_t &value) {
    try {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(text, &consumed, 0);
        if (consumed != text.size()) {
            return false;
        }
        value = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u16_value(const std::string &text, uint16_t &value) {
    size_t parsed = 0;
    if (!parse_size_value(text, parsed) || parsed > 0xffffU) {
        return false;
    }
    value = static_cast<uint16_t>(parsed);
    return true;
}

std::string lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return text;
}

std::vector<std::filesystem::path> player_struct_signature_paths() {
    std::vector<std::filesystem::path> paths;
    const auto stem = rom_path.stem();
    if (!stem.empty()) {
        paths.push_back(std::filesystem::path{"structs"} /
                        (stem.string() + ".struct"));
        paths.push_back(std::filesystem::path{"structs"} /
                        (stem.string() + ".txt"));
        paths.push_back(std::filesystem::path{app.content_directory} /
                        (stem.string() + ".struct"));
    }
    paths.push_back(std::filesystem::path{"structs"} / "n64-default.struct");
    return paths;
}

bool apply_signature_key(PlayerStructSignature &signature,
                         const std::string &key,
                         const std::string &value) {
    const std::string normalized = lower_copy(trim(key));
    const std::string text = trim(value);
    size_t parsed = 0;
    if (normalized == "name") {
        signature.name = text;
        return true;
    }
    if (normalized.rfind("field.", 0) == 0 ||
        normalized.rfind("field_", 0) == 0) {
        if (!parse_size_value(text, parsed)) {
            return false;
        }
        const size_t prefix_size = normalized[5] == '.' ? 6U : 6U;
        std::string field_name = trim(key.substr(prefix_size));
        if (field_name.empty()) {
            return false;
        }
        for (auto &field : signature.fields) {
            if (lower_copy(field.name) == lower_copy(field_name)) {
                field.offset = parsed;
                return true;
            }
        }
        signature.fields.push_back(NamedStructField{field_name, parsed});
        return true;
    }
    if (normalized == "actor_id") {
        if (lower_copy(text) == "any") {
            signature.has_actor_id = false;
            return true;
        }
        uint16_t actor_id = 0;
        if (!parse_u16_value(text, actor_id)) {
            return false;
        }
        signature.actor_id = actor_id;
        signature.has_actor_id = true;
        return true;
    }
    auto set_size = [&](size_t &field) {
        if (!parse_size_value(text, parsed)) {
            return false;
        }
        field = parsed;
        return true;
    };
    if (normalized == "actor_id_offset") return set_size(signature.actor_id_offset);
    if (normalized == "struct_size") return set_size(signature.struct_size);
    if (normalized == "pos_x") return set_size(signature.pos_x);
    if (normalized == "pos_y") return set_size(signature.pos_y);
    if (normalized == "pos_z") return set_size(signature.pos_z);
    if (normalized == "rot_x") return set_size(signature.rot_x);
    if (normalized == "rot_y") return set_size(signature.rot_y);
    if (normalized == "rot_z") return set_size(signature.rot_z);
    if (normalized == "vel_x") return set_size(signature.vel_x);
    if (normalized == "vel_y") return set_size(signature.vel_y);
    if (normalized == "vel_z") return set_size(signature.vel_z);
    if (normalized == "prev") return set_size(signature.prev);
    if (normalized == "next") return set_size(signature.next);
    if (normalized == "update_fn") return set_size(signature.update_fn);
    if (normalized == "draw_fn") return set_size(signature.draw_fn);
    if (normalized == "heap_min") return set_size(signature.heap_min);
    if (normalized == "heap_max") return set_size(signature.heap_max);
    return false;
}

bool load_player_struct_signature_file(
    const std::filesystem::path &path,
    PlayerStructSignature &signature) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    PlayerStructSignature loaded = signature;
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
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            std::cerr << "[struct] linha ignorada em " << path << ':'
                      << line_number << ": " << line << '\n';
            continue;
        }
        if (!apply_signature_key(loaded, line.substr(0, equals),
                                 line.substr(equals + 1))) {
            std::cerr << "[struct] chave invalida em " << path << ':'
                      << line_number << ": " << line << '\n';
        }
    }

    loaded.loaded_from_file = true;
    signature = loaded;
    std::cerr << "[struct] assinatura carregada: " << signature.name
              << " (" << path << ")\n";
    return true;
}

PlayerStructSignature &mutable_player_struct_signature() {
    static PlayerStructSignature signature;
    static std::filesystem::path loaded_for_rom;
    if (loaded_for_rom == rom_path) {
        return signature;
    }

    loaded_for_rom = rom_path;
    signature = PlayerStructSignature{};
    for (const auto &path : player_struct_signature_paths()) {
        if (load_player_struct_signature_file(path, signature)) {
            return signature;
        }
    }
    std::cerr << "[struct] usando assinatura padrao N64 actor/player\n";
    return signature;
}

const PlayerStructSignature &player_struct_signature() {
    return mutable_player_struct_signature();
}

uint32_t n64_actor_struct_score(const uint8_t *ram, size_t size,
                                size_t offset, N64ActorGuess *guess,
                                bool require_actor_id = true) {
    const PlayerStructSignature &signature = player_struct_signature();
    if (!ram || offset + signature.struct_size >= size || (offset & 0x3U) != 0) {
        return 0;
    }
    if (require_actor_id && signature.has_actor_id &&
        read_u16_be(ram, size, offset + signature.actor_id_offset) !=
            signature.actor_id) {
        return 0;
    }

    const float x = read_f32_be(ram, size, offset + signature.pos_x);
    const float y = read_f32_be(ram, size, offset + signature.pos_y);
    const float z = read_f32_be(ram, size, offset + signature.pos_z);
    if (!plausible_world_coordinate(x) ||
        !plausible_world_coordinate(y) ||
        !plausible_world_coordinate(z)) {
        return 0;
    }
    if (std::abs(x) + std::abs(y) + std::abs(z) < 0.001f) {
        return 0;
    }

    uint32_t score = 900;
    if (offset >= signature.heap_min && offset < signature.heap_max) {
        score += 180;
    }
    const uint16_t rot_x = read_u16_be(ram, size, offset + signature.rot_x);
    const uint16_t rot_y = read_u16_be(ram, size, offset + signature.rot_y);
    const uint16_t rot_z = read_u16_be(ram, size, offset + signature.rot_z);
    if (plausible_actor_angle(rot_x)) {
        score += 20;
    }
    if (plausible_actor_angle(rot_y)) {
        score += 45;
    }
    if (plausible_actor_angle(rot_z)) {
        score += 20;
    }

    const float vx = read_f32_be(ram, size, offset + signature.vel_x);
    const float vy = read_f32_be(ram, size, offset + signature.vel_y);
    const float vz = read_f32_be(ram, size, offset + signature.vel_z);
    if (plausible_world_coordinate(vx) && std::abs(vx) < 400.0f) {
        score += 40;
    }
    if (plausible_world_coordinate(vy) && std::abs(vy) < 400.0f) {
        score += 40;
    }
    if (plausible_world_coordinate(vz) && std::abs(vz) < 400.0f) {
        score += 40;
    }

    const uint32_t update_ptr = read_u32_be(ram, size, offset + signature.update_fn);
    const uint32_t draw_ptr = read_u32_be(ram, size, offset + signature.draw_fn);
    if (n64_rdram_address(update_ptr)) {
        score += 240;
    }
    if (n64_rdram_address(draw_ptr)) {
        score += 240;
    }

    const uint32_t prev = read_u32_be(ram, size, offset + signature.prev);
    const uint32_t next = read_u32_be(ram, size, offset + signature.next);
    if (prev == 0 || n64_rdram_address(prev)) {
        score += 65;
    }
    if (next == 0 || n64_rdram_address(next)) {
        score += 65;
    }

    const uint32_t variable_score =
        offset < app.memory_activity.scores.size()
            ? app.memory_activity.scores[offset]
            : 0;
    score += std::min<uint32_t>(variable_score, 0x700);

    if (guess) {
        *guess = N64ActorGuess{
            offset,
            0x80000000U + static_cast<uint32_t>(offset),
            x,
            y,
            z,
            score,
            0,
            true,
        };
    }
    return score;
}

std::vector<N64ActorGuess> find_n64_actor_structs() {
    static std::vector<N64ActorGuess> cached;
    static uint64_t cached_frame = std::numeric_limits<uint64_t>::max();
    if (cached_frame == app.memory_activity.frames ||
        (cached_frame != std::numeric_limits<uint64_t>::max() &&
         app.memory_activity.frames - cached_frame < 12)) {
        return cached;
    }
    cached_frame = app.memory_activity.frames;
    cached.clear();

    const auto *ram = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const size_t size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || size < 0x200) {
        return cached;
    }

    constexpr size_t max_structs = 64;
    std::array<N64ActorGuess, max_structs> best{};
    const PlayerStructSignature &signature = player_struct_signature();
    const size_t begin = std::min(signature.heap_min, size);
    const size_t end = std::min(signature.heap_max, size > signature.struct_size
                                                    ? size - signature.struct_size
                                                    : 0);
    if (begin >= end) {
        return cached;
    }

    for (size_t offset = begin; offset < end; offset += 4) {
        N64ActorGuess candidate;
        if (!n64_actor_struct_score(ram, size, offset, &candidate, false)) {
            continue;
        }
        if (candidate.score <= best.back().score) {
            continue;
        }
        best.back() = candidate;
        for (size_t index = best.size() - 1;
             index > 0 && best[index].score > best[index - 1].score;
             --index) {
            std::swap(best[index], best[index - 1]);
        }
    }

    const N64ActorGuess player = find_link_actor_guess();
    for (auto &candidate : best) {
        if (!candidate.found) {
            continue;
        }
        if (player.found && candidate.offset == player.offset) {
            candidate.stability = player.stability;
            candidate.score = std::max(candidate.score, player.score);
        }
        cached.push_back(candidate);
    }
    return cached;
}

N64ActorGuess find_link_actor_guess() {
    static N64ActorGuess cached;
    static uint64_t cached_frame = std::numeric_limits<uint64_t>::max();
    static size_t scan_cursor = 0;
    static size_t locked_offset = 0;
    static uint16_t locked_stability = 0;
    if (cached_frame == app.memory_activity.frames ||
        (cached.found && app.memory_activity.frames - cached_frame < 4)) {
        return cached;
    }
    cached_frame = app.memory_activity.frames;

    const auto *ram = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const size_t size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || size < 0x200) {
        cached = {};
        return cached;
    }

    N64ActorGuess best;
    if (locked_stability > 0) {
        N64ActorGuess locked;
        if (n64_actor_struct_score(ram, size, locked_offset, &locked) > 0) {
            locked.score += static_cast<uint32_t>(locked_stability) * 48U;
            best = locked;
        } else {
            locked_stability = 0;
            locked_offset = 0;
        }
    }

    if (cached.found) {
        N64ActorGuess refreshed;
        if (n64_actor_struct_score(ram, size, cached.offset, &refreshed) > 0) {
            refreshed.score += 220;
            if (!best.found || refreshed.score > best.score) {
                best = refreshed;
            }
        }
    }

    const PlayerStructSignature &signature = player_struct_signature();
    const size_t end = size > signature.struct_size
        ? size - signature.struct_size
        : 0;
    constexpr size_t scan_budget = 384U * 1024U;
    const size_t start = std::min(scan_cursor, end);
    const size_t stop = std::min(end, start + scan_budget);
    auto scan_range = [&](size_t begin, size_t finish) {
        for (size_t offset = begin; offset < finish; offset += 4) {
            N64ActorGuess candidate;
            const uint32_t score =
                n64_actor_struct_score(ram, size, offset, &candidate);
            if (score && (!best.found || candidate.score > best.score)) {
                best = candidate;
            }
        }
    };
    scan_range(start, stop);
    if (stop >= end && start != 0) {
        scan_range(0, std::min(scan_budget - (stop - start), end));
    }
    scan_cursor = stop >= end ? 0 : stop;

    if (best.found) {
        if (locked_offset == best.offset) {
            locked_stability = std::min<uint16_t>(locked_stability + 1, 120);
        } else if (locked_stability == 0 || best.score > cached.score + 260) {
            locked_offset = best.offset;
            locked_stability = 1;
        } else {
            best = cached;
        }
        best.stability = locked_stability;
        best.score += static_cast<uint32_t>(locked_stability) * 32U;
        app.memory_activity.player_candidate_offset = best.offset;
        app.memory_activity.player_candidate_score = best.score;
        app.memory_activity.player_struct_score = best.score;
        app.memory_activity.player_struct_stability = best.stability;
        if (!app.memory_activity.hottest.empty()) {
            app.memory_activity.hottest[0] = best.offset;
        }
    }

    cached = best;
    return cached;
}

bool focus_debug_search_result(const std::string &query_text) {
    const std::string query = lower_copy(trim(query_text));
    if (query.empty()) {
        app.memory_editor.status = "DIGITE TEXTO PARA BUSCAR";
        return false;
    }

    auto matches = [&](const std::string &text) {
        return lower_copy(text).find(query) != std::string::npos;
    };
    auto focus_region_offset = [&](unsigned region, size_t offset,
                                   const std::string &status) {
        auto &editor = app.memory_editor;
        editor.region = region % memory_regions.size();
        editor.offset = offset;
        clamp_editor_address();
        editor.address_input =
            selected_region().base + static_cast<uint32_t>(editor.offset);
        editor.address_valid = true;
        if (auto *memory = selected_memory();
            memory && editor.offset < selected_memory_size()) {
            editor.value = memory[editor.offset];
        }
        editor.status = status;
        return true;
    };

    for (const auto &watch : app.custom_memory_watches) {
        if (matches(watch.label)) {
            return focus_region_offset(watch.region, watch.offset,
                                       "WATCH: " + watch.label);
        }
    }

    if (app.system == ConsoleSystem::N64) {
        const auto &signature = player_struct_signature();
        N64ActorGuess base_actor;
        if (app.memory_editor.focused_activity &&
            app.memory_editor.region == 0) {
            base_actor.offset = app.memory_editor.focused_activity_offset;
            base_actor.address =
                0x80000000U + static_cast<uint32_t>(base_actor.offset);
            base_actor.found = true;
        }
        if (!base_actor.found) {
            base_actor = find_link_actor_guess();
        }
        if (!base_actor.found) {
            const auto actors = find_n64_actor_structs();
            if (!actors.empty()) {
                base_actor = actors.front();
            }
        }
        if (base_actor.found &&
            (matches(signature.name) || query == "actor" ||
             query == "struct" || query == "player")) {
            return focus_region_offset(0, base_actor.offset,
                                       "STRUCT: " + signature.name);
        }

        const NamedStructField builtin_fields[] = {
            {"actor_id", signature.actor_id_offset},
            {"pos_x", signature.pos_x},
            {"pos_y", signature.pos_y},
            {"pos_z", signature.pos_z},
            {"rot_x", signature.rot_x},
            {"rot_y", signature.rot_y},
            {"rot_z", signature.rot_z},
            {"vel_x", signature.vel_x},
            {"vel_y", signature.vel_y},
            {"vel_z", signature.vel_z},
            {"prev", signature.prev},
            {"next", signature.next},
            {"update_fn", signature.update_fn},
            {"draw_fn", signature.draw_fn},
        };
        if (base_actor.found) {
            for (const auto &field : builtin_fields) {
                if (matches(field.name)) {
                    return focus_region_offset(
                        0, base_actor.offset + field.offset,
                        "FIELD: " + field.name);
                }
            }
            for (const auto &field : signature.fields) {
                if (matches(field.name)) {
                    return focus_region_offset(
                        0, base_actor.offset + field.offset,
                        "FIELD: " + field.name);
                }
            }
        }
    }

    if (app.system == ConsoleSystem::Snes &&
        (query == "sprite" || query == "oam" || query == "struct")) {
        const auto sprites = find_snes_sprite_structs();
        if (!sprites.empty()) {
            return focus_region_offset(
                1, static_cast<size_t>(sprites.front().tile) * 32U,
                "STRUCT SNES: OAM/SPRITE");
        }
    }

    return false;
}

std::string sanitize_struct_field_name(const std::string &raw) {
    std::string name;
    for (unsigned char ch : trim(raw)) {
        if (std::isalnum(ch)) {
            name.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == '_' || ch == '-' || std::isspace(ch)) {
            if (name.empty() || name.back() != '_') {
                name.push_back('_');
            }
        }
    }
    while (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name;
}

bool selected_n64_struct_local_offset(size_t &local_offset) {
    const auto &editor = app.memory_editor;
    if (app.system != ConsoleSystem::N64 || editor.region != 0) {
        return false;
    }
    const auto &signature = player_struct_signature();
    size_t base = 0;
    if (editor.focused_activity) {
        base = editor.focused_activity_offset;
    } else {
        const N64ActorGuess link = find_link_actor_guess();
        if (!link.found) {
            return false;
        }
        base = link.offset;
    }
    if (editor.offset < base ||
        editor.offset >= base + signature.struct_size) {
        return false;
    }
    local_offset = editor.offset - base;
    return true;
}

std::filesystem::path writable_struct_signature_path() {
    const auto paths = player_struct_signature_paths();
    if (!paths.empty()) {
        return paths.front();
    }
    return std::filesystem::path{"structs"} / "n64-default.struct";
}

bool save_debug_field_name(const std::string &raw_name) {
    auto &editor = app.memory_editor;
    const std::string name = sanitize_struct_field_name(raw_name);
    if (name.empty()) {
        editor.status = "NOME INVALIDO";
        return false;
    }

    size_t local_offset = 0;
    if (!selected_n64_struct_local_offset(local_offset)) {
        editor.status = "SELECIONE OFFSET DENTRO DA STRUCT N64";
        return false;
    }

    auto &signature = mutable_player_struct_signature();
    bool replaced = false;
    for (auto &field : signature.fields) {
        if (lower_copy(field.name) == name) {
            field.offset = local_offset;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        signature.fields.push_back(NamedStructField{name, local_offset});
    }

    const std::filesystem::path path = writable_struct_signature_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::app);
    if (!file) {
        editor.status = "NAO SALVOU STRUCT";
        return false;
    }
    file << "\nfield." << name << "=0x" << std::hex << std::uppercase
         << local_offset << std::dec << "\n";

    char status[128];
    std::snprintf(status, sizeof(status), "FIELD %s = +$%04zX",
                  name.c_str(), local_offset);
    editor.status = status;
    return true;
}

const char *n64_region_hint(unsigned region, uint32_t address) {
    if (memory_regions[region].id == RETRO_MEMORY_SAVE_RAM) {
        return "SAVE / EEPROM";
    }
    if (memory_regions[region].id == RETRO_MEMORY_VIDEO_RAM) {
        return "VIDEO / BUFFER";
    }
    if (memory_regions[region].id != RETRO_MEMORY_SYSTEM_RAM) {
        return "REGIAO DO CORE";
    }
    if (address < 0x80100000U) {
        return "BOOT / CODIGO / TABELAS";
    }
    if (address < 0x80200000U) {
        return "ESTADO DO JOGO / GLOBAL";
    }
    if (address < 0x80400000U) {
        return "ATORES / CENA / HEAP";
    }
    return "EXPANSION / HEAP ALTO";
}

std::string ascii_window(const uint8_t *memory, size_t size, size_t offset) {
    if (!memory || !size) {
        return {};
    }
    const size_t begin = offset > 8 ? offset - 8 : 0;
    const size_t end = std::min(size, begin + 24);
    std::string result;
    result.reserve(end - begin);
    for (size_t index = begin; index < end; ++index) {
        const unsigned char value = memory[index];
        result.push_back(std::isprint(value) ? static_cast<char>(value) : '.');
    }
    return result;
}

bool ascii_has_word(const std::string &text) {
    unsigned run = 0;
    for (char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch)) ||
            ch == '_' || ch == '-') {
            ++run;
            if (run >= 4) {
                return true;
            }
        } else {
            run = 0;
        }
    }
    return false;
}

struct LinkPreviewPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct LinkPreviewProjected {
    float x = 0.0f;
    float y = 0.0f;
};

SDL_Color shade(SDL_Color color, float factor) {
    return SDL_Color{
        static_cast<uint8_t>(std::clamp(color.r * factor, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(color.g * factor, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(color.b * factor, 0.0f, 255.0f)),
        color.a,
    };
}

LinkPreviewPoint rotate_y(LinkPreviewPoint point, float yaw) {
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);
    return LinkPreviewPoint{
        point.x * c - point.z * s,
        point.y,
        point.x * s + point.z * c,
    };
}

LinkPreviewProjected project_link_point(const SDL_Rect &preview,
                                        LinkPreviewPoint point,
                                        float yaw,
                                        float scale) {
    const LinkPreviewPoint rotated = rotate_y(point, yaw);
    const float center_x = preview.x + preview.w * 0.5f;
    const float base_y = preview.y + preview.h - 20.0f;
    return LinkPreviewProjected{
        center_x + (rotated.x - rotated.z * 0.46f) * scale,
        base_y - (rotated.y + rotated.z * 0.22f) * scale,
    };
}

void draw_solid_triangle(LinkPreviewProjected a,
                         LinkPreviewProjected b,
                         LinkPreviewProjected c,
                         SDL_Color color) {
    SDL_Vertex vertices[3] = {
        {SDL_FPoint{a.x, a.y}, color, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{b.x, b.y}, color, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{c.x, c.y}, color, SDL_FPoint{0.0f, 0.0f}},
    };
    SDL_RenderGeometry(app.renderer, nullptr, vertices, 3, nullptr, 0);
}

void draw_solid_quad(LinkPreviewProjected a,
                     LinkPreviewProjected b,
                     LinkPreviewProjected c,
                     LinkPreviewProjected d,
                     SDL_Color color) {
    SDL_Vertex vertices[4] = {
        {SDL_FPoint{a.x, a.y}, color, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{b.x, b.y}, color, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{c.x, c.y}, color, SDL_FPoint{0.0f, 0.0f}},
        {SDL_FPoint{d.x, d.y}, color, SDL_FPoint{0.0f, 0.0f}},
    };
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(app.renderer, nullptr, vertices, 4, indices, 6);
}

void draw_model_line(const SDL_Rect &preview,
                     LinkPreviewPoint a,
                     LinkPreviewPoint b,
                     float yaw,
                     float scale,
                     SDL_Color color) {
    const LinkPreviewProjected pa = project_link_point(preview, a, yaw, scale);
    const LinkPreviewProjected pb = project_link_point(preview, b, yaw, scale);
    SDL_SetRenderDrawColor(app.renderer, color.r, color.g, color.b, color.a);
    SDL_RenderDrawLineF(app.renderer, pa.x, pa.y, pb.x, pb.y);
}

void draw_model_box(const SDL_Rect &preview,
                    LinkPreviewPoint min,
                    LinkPreviewPoint max,
                    float yaw,
                    float scale,
                    SDL_Color color) {
    const LinkPreviewPoint p[8] = {
        {min.x, min.y, min.z}, {max.x, min.y, min.z},
        {max.x, max.y, min.z}, {min.x, max.y, min.z},
        {min.x, min.y, max.z}, {max.x, min.y, max.z},
        {max.x, max.y, max.z}, {min.x, max.y, max.z},
    };
    LinkPreviewProjected q[8];
    for (int index = 0; index < 8; ++index) {
        q[index] = project_link_point(preview, p[index], yaw, scale);
    }

    draw_solid_quad(q[4], q[5], q[6], q[7], shade(color, 0.62f));
    draw_solid_quad(q[1], q[5], q[6], q[2], shade(color, 0.78f));
    draw_solid_quad(q[0], q[1], q[2], q[3], color);
    draw_solid_quad(q[3], q[2], q[6], q[7], shade(color, 1.12f));

    SDL_SetRenderDrawColor(app.renderer, 5, 8, 12, 190);
    constexpr int edges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const auto &edge : edges) {
        SDL_RenderDrawLineF(app.renderer, q[edge[0]].x, q[edge[0]].y,
                            q[edge[1]].x, q[edge[1]].y);
    }
}

SDL_Rect expanded_region(SDL_Rect region, int min_w, int min_h,
                         float padding) {
    if (region.w <= 0 || region.h <= 0) {
        return {};
    }
    const float cx = region.x + region.w * 0.5f;
    const float cy = region.y + region.h * 0.5f;
    const int width = std::max(min_w, static_cast<int>(region.w * padding));
    const int height = std::max(min_h, static_cast<int>(region.h * padding));
    return SDL_Rect{
        static_cast<int>(std::round(cx - width * 0.5f)),
        static_cast<int>(std::round(cy - height * 0.5f)),
        width,
        height,
    };
}

SDL_Rect n64_link_asset_region() {
    SDL_Rect region = app.memory_editor.focused_screen_region;
    if (region.w <= 0 || region.h <= 0) {
        region = SDL_Rect{game_width / 2 - 42, game_height * 52 / 100,
                          84, 138};
    }
    return expanded_region(region, 160, 220, 1.7f);
}

bool draw_frame_asset_crop(const SDL_Rect &preview,
                           SDL_Rect logical_region,
                           SDL_Color,
                           const char *empty_message) {
    if (!app.texture || app.texture_width == 0 || app.texture_height == 0 ||
        logical_region.w <= 0 || logical_region.h <= 0) {
        (void)empty_message;
        return false;
    }

    const int logical_width = std::max(1, game_logical_width());
    const float sx = static_cast<float>(app.texture_width) /
                     static_cast<float>(logical_width);
    const float sy = static_cast<float>(app.texture_height) /
                     static_cast<float>(game_height);
    SDL_Rect src{
        static_cast<int>(std::floor(logical_region.x * sx)),
        static_cast<int>(std::floor(logical_region.y * sy)),
        static_cast<int>(std::ceil(logical_region.w * sx)),
        static_cast<int>(std::ceil(logical_region.h * sy)),
    };
    src.x = std::clamp(src.x, 0, static_cast<int>(app.texture_width) - 1);
    src.y = std::clamp(src.y, 0, static_cast<int>(app.texture_height) - 1);
    src.w = std::clamp(src.w, 1, static_cast<int>(app.texture_width) - src.x);
    src.h = std::clamp(src.h, 1, static_cast<int>(app.texture_height) - src.y);

    SDL_Rect dst = preview;
    const float source_aspect =
        static_cast<float>(src.w) / static_cast<float>(std::max(1, src.h));
    const float dest_aspect =
        static_cast<float>(preview.w) / static_cast<float>(std::max(1, preview.h));
    if (source_aspect > dest_aspect) {
        dst.w = preview.w;
        dst.h = std::max(1, static_cast<int>(preview.w / source_aspect));
        dst.y = preview.y + (preview.h - dst.h) / 2;
    } else {
        dst.h = preview.h;
        dst.w = std::max(1, static_cast<int>(preview.h * source_aspect));
        dst.x = preview.x + (preview.w - dst.w) / 2;
    }

    SDL_RenderCopy(app.renderer, app.texture, &src, &dst);
    return true;
}

float link_preview_yaw(const N64ActorGuess &link) {
    const auto *ram = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const size_t size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const PlayerStructSignature &signature = player_struct_signature();
    if (!link.found || !ram ||
        link.offset + signature.rot_y + 1 >= size) {
        return 0.62f;
    }
    const uint16_t raw = read_u16_be(ram, size, link.offset + signature.rot_y);
    return static_cast<float>(raw) * 6.2831853f / 65536.0f + 0.62f;
}

void draw_link_model_preview(const SDL_Rect &preview,
                             const N64ActorGuess &link,
                             bool selected_link_actor) {
    const float yaw = link_preview_yaw(link);
    const float scale = 1.08f;
    const SDL_Color tunic = selected_link_actor
        ? SDL_Color{70, 218, 94, 245}
        : SDL_Color{62, 166, 85, 235};
    const SDL_Color sleeve{42, 126, 68, 235};
    const SDL_Color skin{232, 188, 132, 245};
    const SDL_Color boot{90, 58, 32, 245};
    const SDL_Color hair{205, 158, 58, 245};
    const SDL_Color shield{56, 92, 170, 240};
    const SDL_Color metal{214, 224, 232, 240};

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 0, 0, 0, 95);
    const SDL_Rect shadow{
        preview.x + preview.w / 2 - 34,
        preview.y + preview.h - 32,
        68,
        12,
    };
    SDL_RenderFillRect(app.renderer, &shadow);

    draw_model_box(preview, {-11.0f, 3.0f, -5.0f}, {-2.0f, 42.0f, 5.0f},
                   yaw, scale, boot);
    draw_model_box(preview, {2.0f, 3.0f, -5.0f}, {11.0f, 42.0f, 5.0f},
                   yaw, scale, boot);
    draw_model_box(preview, {-18.0f, 39.0f, -8.0f}, {18.0f, 86.0f, 8.0f},
                   yaw, scale, tunic);
    draw_model_box(preview, {-25.0f, 50.0f, -5.0f}, {-16.0f, 78.0f, 5.0f},
                   yaw, scale, sleeve);
    draw_model_box(preview, {16.0f, 50.0f, -5.0f}, {25.0f, 78.0f, 5.0f},
                   yaw, scale, sleeve);
    draw_model_box(preview, {-13.0f, 88.0f, -7.0f}, {13.0f, 112.0f, 7.0f},
                   yaw, scale, skin);
    draw_model_box(preview, {-16.0f, 101.0f, -8.0f}, {16.0f, 112.0f, -2.0f},
                   yaw, scale, hair);

    const LinkPreviewProjected hat_a =
        project_link_point(preview, {-15.0f, 108.0f, -4.0f}, yaw, scale);
    const LinkPreviewProjected hat_b =
        project_link_point(preview, {15.0f, 108.0f, -4.0f}, yaw, scale);
    const LinkPreviewProjected hat_c =
        project_link_point(preview, {-2.0f, 144.0f, 4.0f}, yaw, scale);
    draw_solid_triangle(hat_a, hat_b, hat_c, shade(tunic, 1.08f));

    draw_model_box(preview, {-31.0f, 45.0f, 4.0f}, {-20.0f, 84.0f, 8.0f},
                   yaw, scale, shield);
    draw_model_line(preview, {26.0f, 47.0f, -4.0f}, {42.0f, 104.0f, -7.0f},
                    yaw, scale, metal);
    draw_model_line(preview, {24.0f, 47.0f, -4.0f}, {40.0f, 104.0f, -7.0f},
                    yaw, scale, metal);

    const SDL_Color axis = selected_link_actor
        ? SDL_Color{110, 235, 150, 210}
        : SDL_Color{150, 200, 230, 190};
    draw_model_line(preview, {-38.0f, 0.0f, 0.0f}, {38.0f, 0.0f, 0.0f},
                    yaw, scale, axis);
    draw_model_line(preview, {0.0f, 0.0f, -28.0f}, {0.0f, 0.0f, 28.0f},
                    yaw, scale, axis);
}

void draw_link_struct_render(const SDL_Rect &preview,
                             const N64ActorGuess &link,
                             bool selected_link_actor) {
    if (!link.found) {
        return;
    }

    const SDL_Color color = selected_link_actor
        ? SDL_Color{110, 235, 150, 255}
        : SDL_Color{150, 200, 230, 255};
    draw_frame_asset_crop(preview, n64_link_asset_region(), color,
                          "SEM ASSET VISIVEL");
}

N64FieldMeaning classify_n64_field(
    unsigned region, size_t offset, uint8_t u8, uint16_t u16, uint32_t u32,
    uint32_t score, const std::string &ascii,
    const MemoryActivity::VisualCorrelation *correlation,
    const N64ActorGuess &link) {
    char detail[128];

    if (memory_regions[region].id == RETRO_MEMORY_SAVE_RAM) {
        return {"SAVE/PROGRESSO", "persistente: itens, flags ou progresso",
                SDL_Color{110, 235, 150, 255}};
    }

    if (n64_segmented_address(u32)) {
        const uint8_t segment = static_cast<uint8_t>(u32 >> 24U);
        std::snprintf(detail, sizeof(detail), "%s %02X:%06X",
                      n64_segment_hint(segment), segment,
                      u32 & 0x00ffffffU);
        return {"ASSET PTR", detail, SDL_Color{110, 235, 150, 255}};
    }

    if (n64_rdram_address(u32)) {
        std::snprintf(detail, sizeof(detail), "aponta para RDRAM $%08X", u32);
        return {"PONTEIRO", detail, SDL_Color{110, 235, 150, 255}};
    }

    const PlayerStructSignature &signature = player_struct_signature();
    if (link.found && region == 0 && offset >= link.offset &&
        offset < link.offset + signature.struct_size) {
        const size_t local = offset - link.offset;
        if (local >= 0x24 && local < 0x30) {
            const char axis = local < 0x28 ? 'X' : local < 0x2c ? 'Y' : 'Z';
            std::snprintf(detail, sizeof(detail),
                          "Link.pos%c float, actor +$%03zX", axis, local);
            return {"POSICAO", detail, SDL_Color{110, 235, 150, 255}};
        }
        if (local >= 0x30 && local < 0x38) {
            std::snprintf(detail, sizeof(detail),
                          "rotacao/angulo do actor +$%03zX", local);
            return {"ROTACAO", detail, SDL_Color{110, 235, 150, 255}};
        }
        if (local >= 0x130 && local < 0x138) {
            std::snprintf(detail, sizeof(detail),
                          "ponteiro update/draw do Link +$%03zX", local);
            return {"FUNCAO", detail, SDL_Color{110, 235, 150, 255}};
        }
        if (local >= 0x50 && local < 0x90) {
            std::snprintf(detail, sizeof(detail),
                          "fisica/colisao/estado +$%03zX", local);
            return {"LINK RUNTIME", detail, SDL_Color{255, 220, 90, 255}};
        }
        std::snprintf(detail, sizeof(detail),
                      "struct viva do Link +$%03zX", local);
        return {"LINK RUNTIME", detail, SDL_Color{255, 220, 90, 255}};
    }

    if (correlation && correlation->evidence >= 3 &&
        (correlation->value_axis == 1 || correlation->value_axis == 2)) {
        std::snprintf(detail, sizeof(detail), "correlacao visual eixo %c",
                      correlation->value_axis == 1 ? 'X' : 'Y');
        return {"COORD VISUAL", detail, SDL_Color{110, 235, 150, 255}};
    }

    if (ascii_has_word(ascii)) {
        return {"TEXTO/NOME", "bytes proximos parecem string ASCII",
                SDL_Color{170, 200, 230, 255}};
    }

    if ((u8 == 0 || u8 == 1) && score < 0x200) {
        return {"FLAG", "0/1: booleano ou bitfield pequeno",
                SDL_Color{170, 200, 230, 255}};
    }

    if (score > 0 && u8 <= 0x3c) {
        std::snprintf(detail, sizeof(detail),
                      "valor pequeno ativo: timer/estado ($%02X)", u8);
        return {"TIMER/ESTADO", detail, SDL_Color{255, 220, 90, 255}};
    }

    if (score > 0) {
        std::snprintf(detail, sizeof(detail),
                      "muda durante runtime, score $%04X", score);
        return {"RUNTIME", detail, SDL_Color{255, 220, 90, 255}};
    }

    if (u16 == 0 || u32 == 0) {
        return {"VAZIO/ZERO", "sem atividade recente detectada",
                SDL_Color{160, 160, 160, 255}};
    }

    return {"DADO BRUTO", "sem padrao conhecido ainda",
            SDL_Color{160, 160, 160, 255}};
}

void draw_n64_memory_preview(int panel_x) {
    const SDL_Rect preview{panel_x + 18, 256, 220, 142};

    const N64ActorGuess link = find_link_actor_guess();
    const bool selected_link_actor =
        link.found && app.memory_editor.region == 0 &&
        app.memory_editor.offset >= link.offset &&
        app.memory_editor.offset <
            link.offset + player_struct_signature().struct_size;
    draw_link_struct_render(preview, link, selected_link_actor);

    auto &editor = app.memory_editor;
    const uint8_t *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!memory || !size || editor.offset >= size) {
        draw_text(panel_x + 18, 404, "STRUCT VISUAL SEM MEMORIA SELECIONADA",
                  SDL_Color{255, 120, 120, 255}, 1);
        if (link.found) {
            char line[128];
            std::snprintf(line, sizeof(line),
                          "LINK $%08X EST %u XYZ %.0f %.0f %.0f",
                          link.address, link.stability, link.x, link.y, link.z);
            draw_text(panel_x + 18, 424, line,
                      SDL_Color{150, 200, 230, 255}, 1);
        }
        return;
    }

    const uint32_t address =
        selected_region().base + static_cast<uint32_t>(editor.offset);
    const uint8_t u8 = memory[editor.offset];
    const int8_t s8 = static_cast<int8_t>(u8);
    const uint16_t u16 = read_u16_be(memory, size, editor.offset);
    const uint32_t u32 = read_u32_be(memory, size, editor.offset);
    const uint32_t score =
        editor.offset < app.memory_activity.scores.size()
            ? app.memory_activity.scores[editor.offset]
            : 0;
    const auto *correlation = focused_visual_correlation();

    const std::string ascii = ascii_window(memory, size, editor.offset);
    const N64FieldMeaning meaning =
        classify_n64_field(editor.region, editor.offset, u8, u16, u32, score,
                           ascii, correlation, link);

    char line[128];
    std::snprintf(line, sizeof(line), "$%08X  %s", address, meaning.kind);
    draw_text(panel_x + 18, 404, line, meaning.color, 1);
    std::snprintf(line, sizeof(line), "U8 %u S8 %+d  BE16 $%04X", u8, s8, u16);
    draw_text(panel_x + 18, 420, line,
              SDL_Color{220, 220, 220, 255}, 1);
    std::snprintf(line, sizeof(line), "%.29s", meaning.detail.c_str());
    draw_text(panel_x + 18, 436, line,
              SDL_Color{220, 220, 220, 255}, 1);
    std::snprintf(line, sizeof(line), "TXT %.20s", ascii.c_str());
    draw_text(panel_x + 18, 452, line,
              SDL_Color{170, 200, 230, 255}, 1);

    if (link.found) {
        std::snprintf(line, sizeof(line), "LINK $%08X EST %u XYZ %.0f %.0f %.0f",
                      link.address, link.stability, link.x, link.y, link.z);
        draw_text(panel_x + 18, 468, line,
                  selected_link_actor ? SDL_Color{110, 235, 150, 255}
                                      : SDL_Color{150, 200, 230, 255},
                  1);
    } else {
        draw_text(panel_x + 18, 468, "LINK: ENTRE NO JOGO PARA DETECTAR",
                  SDL_Color{170, 170, 170, 255}, 1);
    }
}

void draw_correlated_sprite_preview(int panel_x) {
    if (app.system == ConsoleSystem::N64) {
        draw_n64_memory_preview(panel_x);
        return;
    }
    draw_text(panel_x + 18, 254, "PREVIEW SPRITE / OAM",
              SDL_Color{255, 220, 90, 255}, 1);
    const SDL_Rect preview{panel_x + 18, 274, 220, 142};
    draw_preview_checker(preview);
    SDL_SetRenderDrawColor(app.renderer, 75, 170, 220, 255);
    SDL_RenderDrawRect(app.renderer, &preview);

    const auto *vram = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_VIDEO_RAM));
    const auto selection = select_oam_sprite(vram);
    if (!core_ppu()) {
        draw_text(preview.x + 12, preview.y + 60, "PPU NAO EXPORTADO",
                  SDL_Color{255, 120, 120, 255}, 1);
        return;
    }
    if (!vram || core.get_memory_size(RETRO_MEMORY_VIDEO_RAM) < 0x10000) {
        draw_text(preview.x + 12, preview.y + 60, "VRAM INDISPONIVEL",
                  SDL_Color{255, 120, 120, 255}, 1);
        return;
    }
    if (!selection.sprite) {
        draw_text(preview.x + 12, preview.y + 60, "SEM SPRITE/OAM VISIVEL",
                  SDL_Color{160, 160, 160, 255}, 1);
        return;
    }

    char confidence[64];
    std::snprintf(confidence, sizeof(confidence),
                  "OAM%02d TILE $%03X PAL %u  %dx%d",
                  selection.index, selection.sprite->Name,
                  selection.sprite->Palette, selection.width, selection.height);
    draw_text(panel_x + 18, 418, confidence,
              SDL_Color{150, 200, 230, 255}, 1);

    SDL_Rect sprite_region = app.memory_editor.focused_screen_region;
    if (sprite_region.w <= 0 || sprite_region.h <= 0) {
        sprite_region = SDL_Rect{selection.sprite->HPos,
                                 selection.sprite->VPos,
                                 selection.width,
                                 selection.height};
    }
    if (draw_frame_asset_crop(preview,
                              expanded_region(sprite_region, 48, 48, 2.3f),
                              SDL_Color{110, 235, 150, 255},
                              "SEM SPRITE VISIVEL")) {
        return;
    }

    draw_preview_checker(preview);
    SDL_SetRenderDrawColor(app.renderer, 75, 170, 220, 255);
    SDL_RenderDrawRect(app.renderer, &preview);

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

struct ActiveStructField {
    size_t local_offset = 0;
    size_t absolute_offset = 0;
    uint32_t score = 0;
    uint8_t value = 0;
    std::string name;
};

std::string n64_struct_field_name(size_t local_offset) {
    const auto &signature = player_struct_signature();
    const NamedStructField builtin_fields[] = {
        {"actor_id", signature.actor_id_offset},
        {"pos_x", signature.pos_x},
        {"pos_y", signature.pos_y},
        {"pos_z", signature.pos_z},
        {"rot_x", signature.rot_x},
        {"rot_y", signature.rot_y},
        {"rot_z", signature.rot_z},
        {"vel_x", signature.vel_x},
        {"vel_y", signature.vel_y},
        {"vel_z", signature.vel_z},
        {"prev", signature.prev},
        {"next", signature.next},
        {"update_fn", signature.update_fn},
        {"draw_fn", signature.draw_fn},
    };
    for (const auto &field : builtin_fields) {
        if (field.offset == local_offset) {
            return field.name;
        }
    }
    for (const auto &field : signature.fields) {
        if (field.offset == local_offset) {
            return field.name;
        }
    }
    char label[16];
    std::snprintf(label, sizeof(label), "+$%03zX", local_offset);
    return label;
}

std::vector<ActiveStructField> active_n64_struct_fields() {
    std::vector<ActiveStructField> fields;
    if (app.system != ConsoleSystem::N64) {
        return fields;
    }
    const auto &editor = app.memory_editor;
    if (editor.region != 0 || !editor.focused_activity) {
        return fields;
    }
    const auto *memory = static_cast<const uint8_t *>(
        core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const size_t size = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const auto &signature = player_struct_signature();
    const size_t base = editor.focused_activity_offset;
    if (!memory || base >= size) {
        return fields;
    }

    constexpr size_t max_fields = 6;
    const size_t end =
        std::min(signature.struct_size, size > base ? size - base : 0);
    for (size_t local = 0; local < end; ++local) {
        const size_t absolute = base + local;
        if (absolute >= app.memory_activity.scores.size()) {
            continue;
        }
        const uint32_t score = app.memory_activity.scores[absolute];
        if (score == 0) {
            continue;
        }
        fields.push_back(ActiveStructField{
            local,
            absolute,
            score,
            memory[absolute],
            n64_struct_field_name(local),
        });
        std::sort(fields.begin(), fields.end(),
                  [](const ActiveStructField &a,
                     const ActiveStructField &b) {
                      return a.score > b.score;
                  });
        if (fields.size() > max_fields) {
            fields.pop_back();
        }
    }
    return fields;
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
    if (const auto *watch = memory_watch_for(editor.region, editor.offset)) {
        uint64_t raw = 0;
        const std::string value =
            read_watch_raw_value(*watch, raw) ? format_watch_value(*watch, raw)
                                              : "--";
        if (watch->trigger != CustomMemoryWatch::TriggerKind::None) {
            std::snprintf(line, sizeof(line), "%s %s  TRIG %s",
                          watch_type_name(watch->type), value.c_str(),
                          watch_trigger_name(watch->trigger));
        } else {
            std::snprintf(line, sizeof(line), "%s %s",
                          watch_type_name(watch->type), value.c_str());
        }
        draw_text(panel_x + 258, 292, line,
                  SDL_Color{150, 200, 230, 255}, 1);
    }

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

    if (app.system == ConsoleSystem::N64) {
        draw_text(panel_x + 258, 438, "CAMPOS ATIVOS",
                  SDL_Color{255, 220, 90, 255}, 1);
        const auto fields = active_n64_struct_fields();
        if (fields.empty()) {
            draw_text(panel_x + 258, 456, "MOVIMENTE/ESPERE PARA MEDIR",
                      SDL_Color{160, 160, 160, 255}, 1);
        }
        for (size_t index = 0; index < fields.size() && index < 2; ++index) {
            const auto &field = fields[index];
            std::snprintf(line, sizeof(line), "%-10.10s +$%03zX = $%02X %04X",
                          field.name.c_str(), field.local_offset,
                          field.value, field.score);
            draw_text(panel_x + 258, 456 + static_cast<int>(index) * 16,
                      line,
                      field.absolute_offset == editor.offset
                          ? SDL_Color{255, 220, 90, 255}
                          : SDL_Color{220, 220, 220, 255},
                      1);
        }
    }
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

void draw_search_popup(int panel_x) {
    const auto &editor = app.memory_editor;
    if (!editor.search_popup) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 248);
    const SDL_Rect popup{panel_x + 44, 204, 424, 184};
    SDL_RenderFillRect(app.renderer, &popup);
    SDL_SetRenderDrawColor(app.renderer, 255, 190, 60, 255);
    SDL_RenderDrawRect(app.renderer, &popup);

    draw_text(popup.x + 18, popup.y + 18, "9 - BUSCAR VALOR",
              SDL_Color{255, 220, 90, 255});
    draw_text(popup.x + 18, popup.y + 48,
              "DIGITE TEXTO OU LABEL",
              SDL_Color{180, 180, 180, 255}, 1);

    char line[96];
    std::snprintf(line, sizeof(line), "BUSCA     %-24.24s",
                  editor.search_query.empty()
                      ? "_"
                      : editor.search_query.c_str());
    draw_text(popup.x + 18, popup.y + 82, line,
              SDL_Color{255, 220, 90, 255});

    draw_text(popup.x + 18, popup.y + 124,
              "NUMERO DECIMAL REFINA CANDIDATOS\n"
              "ENTER BUSCA LABEL/TEXTO  ESC FECHA",
              SDL_Color{170, 200, 225, 255}, 1);
    if (editor.value_search_active) {
        std::snprintf(line, sizeof(line), "%zu CANDIDATOS PARA %lld",
                      editor.value_search_results.size(),
                      static_cast<long long>(editor.value_search_last_value));
        draw_text(popup.x + 18, popup.y + 154, line,
                  SDL_Color{110, 235, 150, 255}, 1);
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void draw_name_popup(int panel_x) {
    const auto &editor = app.memory_editor;
    if (!editor.name_popup) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 248);
    const SDL_Rect popup{panel_x + 44, 214, 424, 174};
    SDL_RenderFillRect(app.renderer, &popup);
    SDL_SetRenderDrawColor(app.renderer, 255, 190, 60, 255);
    SDL_RenderDrawRect(app.renderer, &popup);

    draw_text(popup.x + 18, popup.y + 18, "N - NOMEAR CAMPO",
              SDL_Color{255, 220, 90, 255});
    draw_text(popup.x + 18, popup.y + 48,
              "SALVA FIELD.NOME NO .STRUCT",
              SDL_Color{180, 180, 180, 255}, 1);

    char line[128];
    std::snprintf(line, sizeof(line), "NOME      %-24.24s",
                  editor.name_query.empty()
                      ? "_"
                      : editor.name_query.c_str());
    draw_text(popup.x + 18, popup.y + 82, line,
              SDL_Color{255, 220, 90, 255});

    draw_text(popup.x + 18, popup.y + 124,
              "ENTER SALVA  BACKSPACE APAGA\n"
              "ESC FECHA",
              SDL_Color{170, 200, 225, 255}, 1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

void draw_memory_debugger() {
    if (app.system == ConsoleSystem::N64) {
        find_link_actor_guess();
    } else if (app.system == ConsoleSystem::Snes) {
        find_snes_player_struct();
    }
    const int content_width = game_logical_width();
    const int panel_x = app.debug_overlay ? content_width - debugger_width
                                          : content_width;
    SDL_SetRenderDrawColor(app.renderer, 8, 10, 18, 255);
    const SDL_Rect panel{panel_x, 0, debugger_width, game_height};
    SDL_RenderFillRect(app.renderer, &panel);
    SDL_SetRenderDrawColor(app.renderer, 75, 170, 220, 255);
    SDL_RenderDrawRect(app.renderer, &panel);

    draw_text(panel_x + 18, 20, "DEBUG DE STRUCTS",
              SDL_Color{255, 220, 90, 255});
    draw_text(panel_x + 18, 44, "I FECHA  [ ] SELECIONA  ENTER FOCA",
              SDL_Color{170, 170, 170, 255}, 1);

    draw_text(panel_x + 18, 68,
              app.custom_memory_watches.empty()
                  ? app.system == ConsoleSystem::Snes
                        ? "STRUCTS OAM / ENTIDADES"
                        : app.system == ConsoleSystem::N64
                            ? "STRUCTS RDRAM / ENTIDADES"
                            : "STRUCTS / ENTIDADES"
                  : "STRUCTS VIGIADAS",
              SDL_Color{255, 220, 90, 255}, 1);
    if (app.system == ConsoleSystem::Snes && app.custom_memory_watches.empty()) {
        const auto structs = find_snes_sprite_structs();
        for (unsigned row = 0; row < structs.size(); ++row) {
            const auto &sprite = structs[row];
            const char marker =
                app.memory_editor.focused_activity &&
                        app.memory_editor.focused_activity_offset ==
                            static_cast<size_t>(sprite.index)
                    ? '>'
                    : row == 0 ? 'P' : ' ';
            char important_line[128];
            std::snprintf(important_line, sizeof(important_line),
                          "%c OAM%02d TILE $%03X PAL %u %dx%d SCORE $%04X",
                          marker, sprite.index, sprite.tile, sprite.palette,
                          sprite.width, sprite.height, sprite.score);
            draw_text(panel_x + 18, 88 + row * 16, important_line,
                      marker == '>' ? SDL_Color{255, 220, 90, 255}
                                    : row == 0
                                          ? SDL_Color{110, 235, 150, 255}
                                          : SDL_Color{220, 220, 220, 255},
                      1);
        }
        if (structs.empty()) {
            draw_text(panel_x + 18, 88, "SEM SPRITES VISIVEIS",
                      SDL_Color{170, 170, 170, 255}, 1);
        }
    } else if (app.system == ConsoleSystem::N64 &&
               app.custom_memory_watches.empty()) {
        const auto structs = find_n64_actor_structs();
        const N64ActorGuess player = find_link_actor_guess();
        const unsigned visible_rows = 8U;
        const unsigned count = static_cast<unsigned>(structs.size());
        const unsigned selected =
            count ? std::min(app.memory_editor.important_index, count - 1) : 0;
        const unsigned first =
            selected >= visible_rows - 1 ? selected - (visible_rows - 1) : 0;
        for (unsigned row = 0; row < visible_rows && first + row < count; ++row) {
            const unsigned index = first + row;
            const auto &actor = structs[index];
            const bool is_selected = index == selected;
            const bool is_player = player.found && actor.offset == player.offset;
            const char marker = is_selected ? '>' : is_player ? 'P' : ' ';
            char important_line[128];
            std::snprintf(important_line, sizeof(important_line),
                          "%c%02u $%08X SC $%04X XYZ %.0f %.0f %.0f",
                          marker, index + 1, actor.address, actor.score,
                          actor.x, actor.y, actor.z);
            draw_text(panel_x + 18, 88 + row * 16, important_line,
                      is_selected ? SDL_Color{255, 220, 90, 255}
                                  : is_player
                                      ? SDL_Color{110, 235, 150, 255}
                                      : SDL_Color{220, 220, 220, 255},
                      1);
        }
        if (structs.empty()) {
            draw_text(panel_x + 18, 88, "STRUCTS N64 EM BUSCA",
                      SDL_Color{170, 170, 170, 255}, 1);
        }
    } else if (!app.custom_memory_watches.empty()) {
        const unsigned count =
            static_cast<unsigned>(app.custom_memory_watches.size());
        const unsigned selected =
            std::min(app.memory_editor.important_index, count - 1);
        const unsigned visible_rows = app.system == ConsoleSystem::N64 ? 8U : 10U;
        const unsigned first =
            selected >= visible_rows - 1 ? selected - (visible_rows - 1) : 0;
        for (unsigned row = 0; row < visible_rows && first + row < count; ++row) {
            const unsigned index = first + row;
            const auto &watch = app.custom_memory_watches[index];
            uint64_t raw = 0;
            const bool has_value = read_watch_raw_value(watch, raw);
            const std::string value =
                has_value ? format_watch_value(watch, raw) : "--";
            char trigger[20]{};
            if (watch.trigger != CustomMemoryWatch::TriggerKind::None) {
                std::snprintf(trigger, sizeof(trigger), " %s",
                              watch_trigger_name(watch.trigger));
            }
            char important_line[128];
            std::snprintf(important_line, sizeof(important_line),
                          "%c%02u %-12.12s $%06X %-4s %-9.9s%s",
                          index == selected ? '>' : ' ', index + 1,
                          watch.label.c_str(), watch.address,
                          watch_type_name(watch.type), value.c_str(), trigger);
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
        const unsigned visible_rows = app.system == ConsoleSystem::N64 ? 8U : 10U;
        for (unsigned index = 0;
             index < app.memory_activity.hottest.size() && index < visible_rows;
             ++index) {
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
                          memory_regions[0].base +
                              static_cast<unsigned>(offset),
                          wram_memory &&
                                  offset <
                                      core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM)
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
    if (app.system == ConsoleSystem::Snes &&
        app.memory_activity.player_struct_score > 0) {
        std::snprintf(player_confidence, sizeof(player_confidence),
                      "P = STRUCT SPRITE  SCORE $%04X  EST %u",
                      app.memory_activity.player_struct_score,
                      app.memory_activity.player_struct_stability);
    } else if (app.system == ConsoleSystem::N64 &&
        app.memory_activity.player_struct_score > 0) {
        std::snprintf(player_confidence, sizeof(player_confidence),
                      "P = STRUCT PLAYER  SCORE $%04X  EST %u",
                      app.memory_activity.player_struct_score,
                      app.memory_activity.player_struct_stability);
    } else {
        std::snprintf(player_confidence, sizeof(player_confidence),
                      "P = PROVAVEL PLAYER  SCORE $%04X",
                      app.memory_activity.player_candidate_score);
    }
    draw_text(panel_x + 18, 218, player_confidence,
              SDL_Color{150, 200, 230, 255}, 1);

    const auto &region = selected_region();
    uint8_t *memory = selected_memory();
    const size_t size = selected_memory_size();
    char line[128];
    const int map_y = app.system == ConsoleSystem::N64 ? 494 : 438;
    const int hex_y = app.system == ConsoleSystem::N64 ? 554 : 498;
    const int hex_rows = app.system == ConsoleSystem::N64 ? 3 : 4;
    const int status_y = app.system == ConsoleSystem::N64 ? 626 : 594;
    const int help_y = app.system == ConsoleSystem::N64 ? 654 : 626;
    std::snprintf(line, sizeof(line), "%s  %s", region.name, region.map);
    draw_text(panel_x + 18, map_y, "MAPA DE MEMORIA",
              SDL_Color{255, 220, 90, 255}, 1);
    draw_text(panel_x + 18, map_y + 18, line,
              SDL_Color{104, 226, 255, 255}, 1);
    if (!memory || !size) {
        draw_text(panel_x + 18, map_y + 36, "REGIAO INDISPONIVEL",
                  SDL_Color{255, 120, 120, 255});
        return;
    }

    if (large_memory_region(size)) {
        const size_t sample = std::min<size_t>(size, 64U * 1024U);
        std::snprintf(line, sizeof(line), "TAM $%06zX  HASH64K $%08X", size,
                      memory_hash(std::span<const uint8_t>(memory, sample)));
    } else {
        std::snprintf(line, sizeof(line), "TAM $%06zX  HASH $%08X", size,
                      memory_hash(std::span<const uint8_t>(memory, size)));
    }
    draw_text(panel_x + 18, map_y + 36, line, SDL_Color{170, 170, 170, 255}, 1);

    const size_t selected = std::min(app.memory_editor.offset, size - 1);
    const size_t first = (selected / 16 >= 1 ? selected / 16 - 1 : 0) * 16;
    draw_text(panel_x + 362, hex_y - 16, "TEXTO",
              SDL_Color{120, 190, 230, 255}, 1);
    for (int row = 0; row < hex_rows && first + row * 16 < size; ++row) {
        const size_t row_offset = first + row * 16;
        std::snprintf(line, sizeof(line), "$%06X:",
                      region.base + static_cast<unsigned>(row_offset));
        draw_text(panel_x + 18, hex_y + row * 20, line,
                  SDL_Color{120, 190, 230, 255}, 1);
        char ascii[17]{};
        for (int column = 0; column < 16 &&
                             row_offset + column < size; ++column) {
            const size_t offset = row_offset + column;
            const int x = panel_x + 74 + column * 20;
            const int y = hex_y + row * 20;
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
            const int y = hex_y + row * 20;
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

    draw_text(panel_x + 18, status_y, app.memory_editor.status,
              SDL_Color{110, 235, 150, 255}, 1);
    draw_text(panel_x + 18, help_y,
              "ENTER EDITA  SPACE POKE  +/KP- MUDA\n"
              "F/L FREEZE  DEL LIMPA  TAB CAMPO\n"
              "G IR ENDERECO  9 BUSCA  N NOME  T TEXTO",
              SDL_Color{170, 170, 170, 255}, 1);
    draw_goto_popup(panel_x);
    draw_search_popup(panel_x);
    draw_name_popup(panel_x);
}

} // namespace snes::frontend
