#include "frontend/runtime_context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>

#include "debug_font.h"

extern char **environ;

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
        if (event.key.keysym.sym == SDLK_k) {
            open_lua_script_picker();
            continue;
        }
        if (event.key.keysym.sym == SDLK_g &&
            !app.memory_editor.goto_popup) {
            open_goto_popup();
            continue;
        }
        if (event.key.keysym.sym == SDLK_TAB &&
            !app.memory_editor.active &&
            !app.memory_editor.text_mode &&
            !app.memory_editor.goto_popup) {
            app.turbo = !app.turbo;
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

} // namespace snes::frontend
