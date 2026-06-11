#include "frontend/runtime_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace snes::frontend {

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
            const int debug_width =
                app.debug_saved_w + app.debug_saved_w * debugger_width / game_width;
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
                core.get_memory_data(memory_regions[watch.region].id));
            const size_t size =
                core.get_memory_size(memory_regions[watch.region].id);
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

} // namespace snes::frontend
