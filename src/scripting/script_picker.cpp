#include "frontend/runtime_context.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace snes::frontend {
namespace {

bool is_lua_script(const std::filesystem::path &path) {
    if (!path.has_extension() || path.extension() != ".lua") {
        return false;
    }
    const auto filename = path.filename().string();
    if (filename.find(".pool") != std::string::npos) {
        return false;
    }
    for (const auto &part : path) {
        if (part == "neat-islands") {
            return false;
        }
    }
    return true;
}

std::string lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string display_path(const std::filesystem::path &path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, ".", error);
    if (!error && !relative.empty()) {
        return relative.generic_string();
    }
    return path.generic_string();
}

void refresh_script_picker_list() {
    auto &picker = app.script_import;
    picker.scripts.clear();
    picker.selected = 0;
    picker.scroll = 0;

    std::error_code error;
    const std::filesystem::path root{"scripts"};
    if (!std::filesystem::exists(root, error)) {
        picker.status = "PASTA scripts/ NAO ENCONTRADA";
        return;
    }

    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied,
             error);
         !error && it != std::filesystem::recursive_directory_iterator{};
         it.increment(error)) {
        if (!it->is_regular_file(error) || !is_lua_script(it->path())) {
            continue;
        }
        picker.scripts.push_back(it->path());
    }

    std::sort(picker.scripts.begin(), picker.scripts.end(),
              [](const auto &left, const auto &right) {
                  return lower_ascii(left.generic_string()) <
                         lower_ascii(right.generic_string());
              });

    picker.status = picker.scripts.empty()
        ? "NENHUM .lua ENCONTRADO EM scripts/"
        : "SELECIONE UM SCRIPT LUA";
}

void clamp_script_picker_scroll() {
    auto &picker = app.script_import;
    constexpr size_t visible_rows = 10;
    if (picker.scripts.empty()) {
        picker.selected = 0;
        picker.scroll = 0;
        return;
    }
    if (picker.selected >= picker.scripts.size()) {
        picker.selected = picker.scripts.size() - 1;
    }
    if (picker.selected < picker.scroll) {
        picker.scroll = picker.selected;
    }
    if (picker.selected >= picker.scroll + visible_rows) {
        picker.scroll = picker.selected - visible_rows + 1;
    }
}

} // namespace

void open_lua_script_picker() {
    if (app.memory_editor.text_mode) {
        set_text_editor_enabled(false);
    }
    app.memory_editor.active = false;
    app.memory_editor.goto_popup = false;
    app.script_import.active = true;
    SDL_StopTextInput();
    refresh_script_picker_list();
}

bool handle_lua_script_picker_key(SDL_Keycode key) {
    auto &picker = app.script_import;
    if (!picker.active) {
        return false;
    }

    if (key == SDLK_ESCAPE || key == SDLK_k) {
        picker.active = false;
        picker.status = "SELECAO DE SCRIPT CANCELADA";
        return true;
    }

    if (picker.scripts.empty()) {
        if (key == SDLK_r) {
            refresh_script_picker_list();
        }
        return true;
    }

    constexpr size_t page_size = 10;
    if (key == SDLK_UP) {
        if (picker.selected > 0) {
            --picker.selected;
        }
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_DOWN) {
        if (picker.selected + 1 < picker.scripts.size()) {
            ++picker.selected;
        }
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_PAGEUP) {
        picker.selected = picker.selected > page_size
            ? picker.selected - page_size
            : 0;
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_PAGEDOWN) {
        picker.selected = std::min(picker.selected + page_size,
                                   picker.scripts.size() - 1);
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_HOME) {
        picker.selected = 0;
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_END) {
        picker.selected = picker.scripts.size() - 1;
        clamp_script_picker_scroll();
        return true;
    }
    if (key == SDLK_r) {
        refresh_script_picker_list();
        return true;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        const auto selected = picker.scripts[picker.selected];
        import_lua_script_from_path(selected);
        if (app.script_editor.status == "SCRIPT LUA IMPORTADO E CARREGADO") {
            picker.active = false;
        }
        return true;
    }

    return true;
}

void draw_lua_script_picker() {
    const auto &picker = app.script_import;
    if (!picker.active) {
        return;
    }

    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.renderer, 5, 8, 15, 240);
    const SDL_Rect box{126, 126, 772, 516};
    SDL_RenderFillRect(app.renderer, &box);
    SDL_SetRenderDrawColor(app.renderer, 120, 230, 255, 230);
    SDL_RenderDrawRect(app.renderer, &box);

    draw_text(box.x + 22, box.y + 20, "SCRIPTS LUA",
              SDL_Color{120, 230, 255, 255}, 2);
    draw_text(box.x + 22, box.y + 58, picker.status,
              SDL_Color{230, 237, 243, 255}, 1);

    const SDL_Rect list{box.x + 22, box.y + 86, box.w - 44, 360};
    SDL_SetRenderDrawColor(app.renderer, 16, 21, 31, 255);
    SDL_RenderFillRect(app.renderer, &list);
    SDL_SetRenderDrawColor(app.renderer, 65, 95, 120, 255);
    SDL_RenderDrawRect(app.renderer, &list);

    constexpr size_t visible_rows = 10;
    for (size_t row = 0; row < visible_rows; ++row) {
        const size_t index = picker.scroll + row;
        if (index >= picker.scripts.size()) {
            break;
        }
        const int y = list.y + 10 + static_cast<int>(row) * 34;
        if (index == picker.selected) {
            const SDL_Rect selection{list.x + 8, y - 5, list.w - 16, 28};
            SDL_SetRenderDrawColor(app.renderer, 255, 220, 90, 210);
            SDL_RenderFillRect(app.renderer, &selection);
        }

        const std::string marker = index == picker.selected ? "> " : "  ";
        const auto color = index == picker.selected
            ? SDL_Color{5, 8, 15, 255}
            : SDL_Color{225, 232, 240, 255};
        draw_text(list.x + 18, y, marker + display_path(picker.scripts[index]),
                  color, 1);
    }

    const std::string count = std::to_string(picker.scripts.size()) +
                              " SCRIPT(S) EM scripts/";
    draw_text(box.x + 22, box.y + 458, count,
              SDL_Color{145, 180, 205, 255}, 1);
    draw_text(box.x + 22, box.y + 482,
              "SETAS NAVEGAM  ENTER CARREGA  R ATUALIZA  ESC FECHA",
              SDL_Color{170, 200, 225, 255}, 1);
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_NONE);
}

} // namespace snes::frontend
