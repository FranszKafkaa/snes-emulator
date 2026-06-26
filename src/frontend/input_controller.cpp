#include "frontend/runtime_context.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace snes::frontend {

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
    editor.search_popup = false;
    editor.name_popup = false;
    editor.goto_editing_value = false;
    editor.goto_replace_on_type = true;
    editor.active = false;
    editor.status = "G ABERTO";
}

std::string search_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

bool contains_search_text(const std::string &text,
                          const std::string &query) {
    return search_lower(text).find(query) != std::string::npos;
}

const char *value_kind_name(MemoryValueKind kind) {
    switch (kind) {
    case MemoryValueKind::U8: return "u8";
    case MemoryValueKind::S8: return "s8";
    case MemoryValueKind::BE16: return "be16";
    case MemoryValueKind::LE16: return "le16";
    case MemoryValueKind::BE32: return "be32";
    case MemoryValueKind::LE32: return "le32";
    }
    return "u8";
}

void focus_search_result(unsigned region, size_t offset,
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
}

bool parse_decimal_search_value(const std::string &query, int64_t &value) {
    const std::string text = trim(query);
    if (text.empty()) {
        return false;
    }
    size_t index = 0;
    if (text[index] == '-' || text[index] == '+') {
        ++index;
    }
    if (index >= text.size()) {
        return false;
    }
    for (; index < text.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    try {
        size_t consumed = 0;
        value = std::stoll(text, &consumed, 10);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

uint32_t read_le_value(const uint8_t *memory, size_t size, size_t offset,
                       size_t bytes) {
    if (!memory || offset + bytes > size) {
        return 0;
    }
    uint32_t value = 0;
    for (size_t index = 0; index < bytes; ++index) {
        value |= static_cast<uint32_t>(memory[offset + index]) << (index * 8U);
    }
    return value;
}

uint32_t read_be_value(const uint8_t *memory, size_t size, size_t offset,
                       size_t bytes) {
    if (!memory || offset + bytes > size) {
        return 0;
    }
    uint32_t value = 0;
    for (size_t index = 0; index < bytes; ++index) {
        value = (value << 8U) | memory[offset + index];
    }
    return value;
}

bool memory_value_matches(const uint8_t *memory, size_t size, size_t offset,
                          MemoryValueKind kind, int64_t target) {
    switch (kind) {
    case MemoryValueKind::U8:
        return target >= 0 && target <= 0xff &&
               offset < size && memory[offset] == target;
    case MemoryValueKind::S8:
        return target >= -128 && target <= 127 &&
               offset < size && static_cast<int8_t>(memory[offset]) == target;
    case MemoryValueKind::BE16:
        return target >= 0 && target <= 0xffff &&
               read_be_value(memory, size, offset, 2) == target;
    case MemoryValueKind::LE16:
        return target >= 0 && target <= 0xffff &&
               read_le_value(memory, size, offset, 2) == target;
    case MemoryValueKind::BE32:
        return target >= 0 && target <= std::numeric_limits<uint32_t>::max() &&
               read_be_value(memory, size, offset, 4) ==
                   static_cast<uint32_t>(target);
    case MemoryValueKind::LE32:
        return target >= 0 && target <= std::numeric_limits<uint32_t>::max() &&
               read_le_value(memory, size, offset, 4) ==
                   static_cast<uint32_t>(target);
    }
    return false;
}

void add_value_search_match(std::vector<MemoryValueSearchResult> &results,
                            const uint8_t *memory, size_t size, size_t offset,
                            int64_t target) {
    constexpr size_t max_results = 200000;
    const MemoryValueKind kinds[] = {
        MemoryValueKind::U8, MemoryValueKind::S8,
        MemoryValueKind::BE16, MemoryValueKind::LE16,
        MemoryValueKind::BE32, MemoryValueKind::LE32,
    };
    for (MemoryValueKind kind : kinds) {
        if (results.size() >= max_results) {
            return;
        }
        if (memory_value_matches(memory, size, offset, kind, target)) {
            results.push_back(MemoryValueSearchResult{offset, kind});
        }
    }
}

bool find_numeric_memory_value(int64_t target) {
    auto &editor = app.memory_editor;
    const auto *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!memory || !size) {
        editor.status = "REGIAO INDISPONIVEL";
        return false;
    }

    const unsigned region = editor.region;
    std::vector<MemoryValueSearchResult> results;
    if (editor.value_search_active &&
        editor.value_search_region == region &&
        !editor.value_search_results.empty()) {
        results.reserve(editor.value_search_results.size());
        for (const auto &candidate : editor.value_search_results) {
            if (memory_value_matches(memory, size, candidate.offset,
                                     candidate.kind, target)) {
                results.push_back(candidate);
            }
        }
    } else {
        for (size_t offset = 0; offset < size; ++offset) {
            add_value_search_match(results, memory, size, offset, target);
            if (results.size() >= 200000) {
                break;
            }
        }
    }

    editor.value_search_active = true;
    editor.value_search_region = region;
    editor.value_search_last_value = target;
    editor.value_search_results = std::move(results);

    if (editor.value_search_results.empty()) {
        editor.status = "VALOR SEM RESULTADO";
        return false;
    }

    const auto &first = editor.value_search_results.front();
    char status[128];
    std::snprintf(status, sizeof(status), "VALOR %lld: %zu CAND %s",
                  static_cast<long long>(target),
                  editor.value_search_results.size(),
                  value_kind_name(first.kind));
    focus_search_result(region, first.offset, status);
    return true;
}

bool find_named_memory(const std::string &query) {
    auto &editor = app.memory_editor;
    if (query.empty()) {
        editor.status = "DIGITE TEXTO PARA BUSCAR";
        return false;
    }

    int64_t numeric_value = 0;
    if (parse_decimal_search_value(editor.search_query, numeric_value)) {
        return find_numeric_memory_value(numeric_value);
    }

    if (focus_debug_search_result(editor.search_query)) {
        return true;
    }

    const auto *memory = selected_memory();
    const size_t size = selected_memory_size();
    if (!memory || !size) {
        editor.status = "REGIAO INDISPONIVEL";
        return false;
    }
    const std::string needle = editor.search_query;
    if (needle.empty()) {
        editor.status = "DIGITE TEXTO PARA BUSCAR";
        return false;
    }
    const size_t start = editor.offset < size ? editor.offset + 1 : 0;
    for (size_t step = 0; step < size; ++step) {
        const size_t offset = (start + step) % size;
        if (offset + needle.size() > size) {
            continue;
        }
        bool match = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            const auto left = static_cast<unsigned char>(memory[offset + index]);
            const auto right = static_cast<unsigned char>(needle[index]);
            if (std::tolower(left) != std::tolower(right)) {
                match = false;
                break;
            }
        }
        if (match) {
            char status[96];
            std::snprintf(status, sizeof(status), "TEXTO EM $%06X",
                          selected_region().base +
                              static_cast<uint32_t>(offset));
            focus_search_result(editor.region, offset, status);
            return true;
        }
    }
    editor.status = "BUSCA SEM RESULTADO";
    return false;
}

void open_memory_search_popup() {
    auto &editor = app.memory_editor;
    if (!app.memory_debug) {
        app.memory_debug = true;
        set_debug_layout(true);
    }
    if (editor.text_mode) {
        set_text_editor_enabled(false);
    }
    editor.search_popup = true;
    editor.search_query.clear();
    editor.active = false;
    editor.goto_popup = false;
    editor.name_popup = false;
    editor.status = "BUSCA POR TEXTO";
    SDL_StartTextInput();
}

void append_memory_search_text(const char *text) {
    auto &editor = app.memory_editor;
    if (!editor.search_popup || !text) {
        return;
    }
    editor.search_query += text;
    editor.status = "ENTER BUSCA";
}

bool handle_memory_search_popup_key(SDL_Keycode key) {
    auto &editor = app.memory_editor;
    if (!editor.search_popup) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        editor.search_popup = false;
        editor.status = "BUSCA FECHADA";
        if (!editor.text_mode) {
            SDL_StopTextInput();
        }
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        if (!editor.search_query.empty()) {
            editor.search_query.pop_back();
        }
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        find_named_memory(search_lower(editor.search_query));
        editor.search_popup = false;
        if (!editor.text_mode) {
            SDL_StopTextInput();
        }
        return true;
    }
    if (key == SDLK_SPACE) {
        editor.search_query += ' ';
    }
    return true;
}

void open_memory_name_popup() {
    auto &editor = app.memory_editor;
    if (!app.memory_debug) {
        app.memory_debug = true;
        set_debug_layout(true);
    }
    if (editor.text_mode) {
        set_text_editor_enabled(false);
    }
    editor.name_popup = true;
    editor.name_query.clear();
    editor.active = false;
    editor.goto_popup = false;
    editor.search_popup = false;
    editor.status = "NOME DO CAMPO";
    SDL_StartTextInput();
}

void append_memory_name_text(const char *text) {
    auto &editor = app.memory_editor;
    if (!editor.name_popup || !text) {
        return;
    }
    editor.name_query += text;
    editor.status = "ENTER SALVA CAMPO";
}

bool handle_memory_name_popup_key(SDL_Keycode key) {
    auto &editor = app.memory_editor;
    if (!editor.name_popup) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        editor.name_popup = false;
        editor.status = "NOME FECHADO";
        if (!editor.text_mode) {
            SDL_StopTextInput();
        }
        return true;
    }
    if (key == SDLK_BACKSPACE) {
        if (!editor.name_query.empty()) {
            editor.name_query.pop_back();
        }
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        save_debug_field_name(editor.name_query);
        editor.name_popup = false;
        if (!editor.text_mode) {
            SDL_StopTextInput();
        }
        return true;
    }
    if (key == SDLK_SPACE) {
        editor.name_query += ' ';
    }
    return true;
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
        if (event.type == SDL_TEXTINPUT &&
            app.memory_editor.name_popup) {
            append_memory_name_text(event.text.text);
            continue;
        }
        if (event.type == SDL_TEXTINPUT &&
            app.memory_editor.search_popup) {
            append_memory_search_text(event.text.text);
            continue;
        }
        if (event.type == SDL_TEXTINPUT && app.memory_editor.text_mode) {
            write_text_to_memory(event.text.text);
            continue;
        }
        if (event.type != SDL_KEYDOWN || event.key.repeat) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_MINUS) {
            open_script_editor();
            continue;
        }
        if (handle_lua_script_picker_key(event.key.keysym.sym)) {
            continue;
        }
        if (handle_video_filter_menu_key(event.key.keysym.sym)) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_k) {
            open_lua_script_picker();
            continue;
        }
        if (event.key.keysym.sym == SDLK_g &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            open_goto_popup();
            continue;
        }
        if (event.key.keysym.sym == SDLK_TAB &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            app.turbo = !app.turbo;
            continue;
        }
        if (handle_goto_popup_key(event.key.keysym.sym)) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_n &&
            app.memory_debug &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            open_memory_name_popup();
            continue;
        }
        if (handle_memory_name_popup_key(event.key.keysym.sym)) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_9 &&
            app.memory_debug &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            open_memory_search_popup();
            continue;
        }
        if (handle_memory_search_popup_key(event.key.keysym.sym)) {
            continue;
        }
        if (event.key.keysym.sym == SDLK_v &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            open_video_filter_menu();
            continue;
        }
        if ((event.key.keysym.sym == SDLK_LEFTBRACKET ||
             event.key.keysym.sym == SDLK_RIGHTBRACKET) &&
            !app.memory_debug &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup &&
            !app.memory_editor.search_popup &&
            !app.memory_editor.name_popup) {
            adjust_video_filter_sharpness(
                event.key.keysym.sym == SDLK_RIGHTBRACKET ? 5 : -5);
            continue;
        }
        if (event.key.keysym.sym == SDLK_i) {
            app.memory_debug = !app.memory_debug;
            if (!app.memory_debug) {
                if (app.memory_editor.text_mode) {
                    set_text_editor_enabled(false);
                }
                app.memory_editor.active = false;
                app.memory_editor.goto_popup = false;
                app.memory_editor.search_popup = false;
                app.memory_editor.name_popup = false;
                SDL_StopTextInput();
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
        case SDLK_F2: core.reset(); break;
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

} // namespace snes::frontend
