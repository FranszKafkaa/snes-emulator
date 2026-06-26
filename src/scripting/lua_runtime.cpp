#include "frontend/runtime_context.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

extern char **environ;

namespace snes::frontend {

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
        core.get_memory_data(memory_regions[region].id));
    const size_t size = core.get_memory_size(memory_regions[region].id);
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
        core.get_memory_data(memory_regions[region].id));
    const size_t size = core.get_memory_size(memory_regions[region].id);
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

int lua_set_speed(lua_State *state) {
    const int speed =
        lua_isnoneornil(state, 1) ? 1 : static_cast<int>(luaL_checkinteger(state, 1));
    app.lua.speed_multiplier = std::clamp(speed, 1, 64);
    lua_pushinteger(state, app.lua.speed_multiplier);
    return 1;
}

int lua_speed(lua_State *state) {
    lua_pushinteger(state, effective_speed_multiplier());
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

SDL_Color lua_color(lua_State *state, int first_index) {
    const auto channel = [&](int index, int fallback) {
        if (lua_isnoneornil(state, index)) {
            return fallback;
        }
        return std::clamp(static_cast<int>(luaL_checkinteger(state, index)),
                          0, 255);
    };
    return SDL_Color{
        static_cast<Uint8>(channel(first_index, 255)),
        static_cast<Uint8>(channel(first_index + 1, 255)),
        static_cast<Uint8>(channel(first_index + 2, 255)),
        static_cast<Uint8>(channel(first_index + 3, 255)),
    };
}

int lua_draw_text(lua_State *state) {
    LuaRuntime::DrawCommand command;
    command.kind = LuaRuntime::DrawKind::Text;
    command.x = static_cast<int>(luaL_checkinteger(state, 1));
    command.y = static_cast<int>(luaL_checkinteger(state, 2));
    command.text = luaL_checkstring(state, 3);
    command.color = lua_color(state, 4);
    command.scale = std::clamp(
        lua_isnoneornil(state, 8) ? 1 : static_cast<int>(luaL_checkinteger(state, 8)),
        1, 6);
    app.lua.draw_commands.push_back(std::move(command));
    return 0;
}

int lua_draw_rect(lua_State *state) {
    LuaRuntime::DrawCommand command;
    command.kind = LuaRuntime::DrawKind::Rect;
    command.x = static_cast<int>(luaL_checkinteger(state, 1));
    command.y = static_cast<int>(luaL_checkinteger(state, 2));
    command.w = static_cast<int>(luaL_checkinteger(state, 3));
    command.h = static_cast<int>(luaL_checkinteger(state, 4));
    command.color = lua_color(state, 5);
    command.filled = lua_isnoneornil(state, 9) ? false : lua_toboolean(state, 9);
    app.lua.draw_commands.push_back(command);
    return 0;
}

int lua_draw_line(lua_State *state) {
    LuaRuntime::DrawCommand command;
    command.kind = LuaRuntime::DrawKind::Line;
    command.x = static_cast<int>(luaL_checkinteger(state, 1));
    command.y = static_cast<int>(luaL_checkinteger(state, 2));
    command.x2 = static_cast<int>(luaL_checkinteger(state, 3));
    command.y2 = static_cast<int>(luaL_checkinteger(state, 4));
    command.color = lua_color(state, 5);
    app.lua.draw_commands.push_back(command);
    return 0;
}

int lua_clear_overlay(lua_State *) {
    app.lua.draw_commands.clear();
    return 0;
}

int lua_save_state(lua_State *state) {
    lua_pushboolean(state, save_current_state());
    return 1;
}

int lua_load_state(lua_State *state) {
    lua_pushboolean(state, load_current_state());
    return 1;
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
    lua_pushcfunction(state, lua_set_speed);
    lua_setfield(state, -2, "set_speed");
    lua_pushcfunction(state, lua_speed);
    lua_setfield(state, -2, "speed");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");
    lua_pushcfunction(state, lua_save_state);
    lua_setfield(state, -2, "save_state");
    lua_pushcfunction(state, lua_load_state);
    lua_setfield(state, -2, "load_state");
    lua_pushcfunction(state, lua_draw_text);
    lua_setfield(state, -2, "draw_text");
    lua_pushcfunction(state, lua_draw_rect);
    lua_setfield(state, -2, "draw_rect");
    lua_pushcfunction(state, lua_draw_line);
    lua_setfield(state, -2, "draw_line");
    lua_pushcfunction(state, lua_clear_overlay);
    lua_setfield(state, -2, "clear_overlay");
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
    app.lua.speed_multiplier = 1;
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

std::string default_lua_script_text() {
    return
        "-- Script Lua para o emulador SNES.\n"
        "-- Pressione Ctrl-S para salvar e Ctrl-R para salvar/recarregar.\n"
        "\n"
        "function on_frame(frame)\n"
        "    snes.clear_input()\n"
        "\n"
        "end\n";
}

void write_text_file_if_missing(const std::filesystem::path &path,
                                const std::string &content) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return;
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream file(path);
    file << content;
}

void update_script_editor_timestamp() {
    std::error_code error;
    const auto timestamp =
        std::filesystem::last_write_time(app.script_editor.path, error);
    if (!error) {
        app.script_editor.last_write_time = timestamp;
        app.script_editor.has_timestamp = true;
    }
}

void open_script_editor() {
    auto &editor = app.script_editor;
    write_text_file_if_missing(editor.path, default_lua_script_text());
    update_script_editor_timestamp();

    if (editor.process > 0) {
        int status = 0;
        if (waitpid(editor.process, &status, WNOHANG) == 0) {
            editor.status = "SNES LUA STUDIO JA ESTA ABERTO";
            return;
        }
        editor.process = -1;
    }

    if (app.memory_editor.text_mode) {
        set_text_editor_enabled(false);
    }
    app.memory_editor.active = false;
    app.memory_editor.goto_popup = false;
    app.memory_editor.search_popup = false;
    app.memory_editor.name_popup = false;
    const auto editor_path =
        (std::filesystem::current_path() / "build/snes-lua-editor").string();
    const auto script_path = std::filesystem::absolute(editor.path).string();
    char *const args[] = {
        const_cast<char *>(editor_path.c_str()),
        const_cast<char *>(script_path.c_str()),
        nullptr,
    };
    pid_t pid = -1;
    const int result = posix_spawn(&pid, editor_path.c_str(), nullptr, nullptr,
                                   args, environ);
    if (result != 0) {
        editor.status = "NAO FOI POSSIVEL ABRIR O SNES LUA STUDIO";
        std::cerr << editor.status << ": " << editor_path << '\n';
        return;
    }
    editor.process = pid;
    editor.status = "SNES LUA STUDIO ABERTO";
}

void import_lua_script_from_path(const std::filesystem::path &selected_path) {
    auto &editor = app.script_editor;
    if (app.memory_editor.text_mode) {
        set_text_editor_enabled(false);
    }
    app.memory_editor.active = false;
    app.memory_editor.goto_popup = false;
    app.memory_editor.search_popup = false;
    app.memory_editor.name_popup = false;

    if (selected_path.empty()) {
        editor.status = "IMPORTACAO DE SCRIPT CANCELADA";
        return;
    }
    if (!std::filesystem::exists(selected_path)) {
        editor.status = "SCRIPT LUA NAO ENCONTRADO";
        app.script_import.status = "SCRIPT LUA NAO ENCONTRADO";
        std::cerr << editor.status << ": " << selected_path << '\n';
        return;
    }

    std::error_code error;
    const auto import_dir = std::filesystem::path{"scripts"} / "imported";
    std::filesystem::create_directories(import_dir, error);
    if (error) {
        editor.status = "NAO FOI POSSIVEL CRIAR PASTA DE SCRIPTS";
        std::cerr << editor.status << ": " << import_dir << '\n';
        return;
    }

    std::filesystem::path filename = selected_path.filename();
    if (filename.empty()) {
        filename = "importado.lua";
    }
    const auto imported_path = import_dir / filename;
    const auto selected_absolute = std::filesystem::absolute(selected_path, error);
    error.clear();
    const auto imported_absolute = std::filesystem::absolute(imported_path, error);
    error.clear();

    bool same_file = false;
    if (!selected_absolute.empty() && !imported_absolute.empty()) {
        same_file = std::filesystem::equivalent(selected_absolute,
                                                imported_absolute, error);
        error.clear();
    }

    if (!same_file) {
        std::filesystem::copy_file(selected_path, imported_path,
                                   std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error) {
            editor.status = "NAO FOI POSSIVEL IMPORTAR O SCRIPT LUA";
            app.script_import.status = "FALHA AO COPIAR SCRIPT";
            std::cerr << editor.status << ": " << selected_path
                      << " -> " << imported_path << '\n';
            return;
        }
    }

    editor.path = imported_path;
    update_script_editor_timestamp();
    load_lua_script(editor.path);
    editor.status = "SCRIPT LUA IMPORTADO E CARREGADO";
    app.script_import.status = "SCRIPT IMPORTADO";
}

void reload_script_if_changed() {
    auto &editor = app.script_editor;
    std::error_code error;
    const auto timestamp =
        std::filesystem::last_write_time(editor.path, error);
    if (error) {
        return;
    }
    if (!editor.has_timestamp) {
        editor.last_write_time = timestamp;
        editor.has_timestamp = true;
        return;
    }
    if (timestamp == editor.last_write_time) {
        return;
    }
    editor.last_write_time = timestamp;
    load_lua_script(editor.path);
    editor.status = "SCRIPT LUA RECARREGADO";
}

void run_lua_frame() {
    if (!app.lua.active || !app.lua.state) {
        return;
    }
    app.lua.draw_commands.clear();
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

} // namespace snes::frontend
