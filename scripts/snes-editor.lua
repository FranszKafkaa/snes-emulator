#!/usr/bin/env luajit

local keywords = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while",
}

local builtins = {
    "assert", "collectgarbage", "dofile", "error", "getmetatable",
    "ipairs", "load", "loadfile", "next", "pairs", "pcall", "print",
    "rawequal", "rawget", "rawlen", "rawset", "require", "select",
    "setmetatable", "tonumber", "tostring", "type", "xpcall",
    "math.abs", "math.ceil", "math.floor", "math.max", "math.min",
    "math.random", "math.sin", "math.cos", "math.sqrt",
    "string.byte", "string.char", "string.find", "string.format",
    "string.gmatch", "string.gsub", "string.len", "string.lower",
    "string.match", "string.rep", "string.sub", "string.upper",
    "table.concat", "table.insert", "table.remove", "table.sort",
}

local snes_api = {
    "snes.read8", "snes.write8", "snes.read16", "snes.write16",
    "snes.press", "snes.release", "snes.set_button", "snes.clear_input",
    "snes.frame", "snes.log",
}

local snes_buttons = {
    "up", "down", "left", "right", "a", "b", "x", "y", "l", "r",
    "start", "select",
}

local default_template = {
    "-- Script Lua para o emulador SNES.",
    "-- Salve com Ctrl-S e rode com:",
    "-- ./build/snes mario.sfc --script scripts/meu-script.lua",
    "",
    "function on_frame(frame)",
    "    snes.clear_input()",
    "",
    "end",
}

local function usage()
    io.write([[
Uso: luajit scripts/snes-editor.lua [arquivo.lua]

Atalhos:
  Ctrl-S  salvar
  Ctrl-Q  sair
  Tab     autocompletar Lua/snes
  Setas   mover cursor
  Home/End, PageUp/PageDown tambem funcionam quando o terminal envia essas teclas.

Opcoes de teste:
  --complete PREFIXO  imprime sugestoes de autocomplete
  --help             mostra esta ajuda
]])
end

local function sorted_keys(set)
    local out = {}
    for key in pairs(set) do
        out[#out + 1] = key
    end
    table.sort(out)
    return out
end

local function starts_with(text, prefix)
    return text:sub(1, #prefix) == prefix
end

local function collect_symbols(lines)
    local set = {}
    for _, line in ipairs(lines) do
        for word in line:gmatch("[%a_][%w_]*") do
            if #word > 2 then
                set[word] = true
            end
        end
    end
    return sorted_keys(set)
end

local function all_completions(lines)
    local out = {}
    for _, item in ipairs(keywords) do out[#out + 1] = item end
    for _, item in ipairs(builtins) do out[#out + 1] = item end
    for _, item in ipairs(snes_api) do out[#out + 1] = item end
    for _, item in ipairs(snes_buttons) do out[#out + 1] = item end
    for _, item in ipairs(collect_symbols(lines)) do out[#out + 1] = item end
    table.sort(out)
    return out
end

local function suggestions_for(prefix, lines)
    local matches = {}
    if prefix == "" then
        return matches
    end
    for _, item in ipairs(all_completions(lines)) do
        if starts_with(item, prefix) then
            matches[#matches + 1] = item
        end
    end
    return matches
end

if arg[1] == "--help" or arg[1] == "-h" then
    usage()
    os.exit(0)
end

if arg[1] == "--complete" then
    local matches = suggestions_for(arg[2] or "", default_template)
    for _, item in ipairs(matches) do
        print(item)
    end
    os.exit(0)
end

local function read_file(path)
    local file = io.open(path, "r")
    if not file then
        local copy = {}
        for index, line in ipairs(default_template) do
            copy[index] = line
        end
        return copy
    end

    local lines = {}
    for line in file:lines() do
        lines[#lines + 1] = line
    end
    file:close()
    if #lines == 0 then
        lines[1] = ""
    end
    return lines
end

local function write_file(path, lines)
    local file, message = io.open(path, "w")
    if not file then
        return false, message
    end
    for index, line in ipairs(lines) do
        file:write(line)
        if index < #lines then
            file:write("\n")
        end
    end
    file:close()
    return true
end

local function terminal_size()
    local handle = io.popen("stty size 2>/dev/null")
    if not handle then
        return 24, 80
    end
    local output = handle:read("*a") or ""
    handle:close()
    local rows, cols = output:match("(%d+)%s+(%d+)")
    return tonumber(rows) or 24, tonumber(cols) or 80
end

local function trim_to_width(text, width)
    if #text <= width then
        return text
    end
    if width <= 1 then
        return text:sub(1, width)
    end
    return text:sub(1, width - 1) .. ">"
end

local function common_prefix(items)
    if #items == 0 then
        return ""
    end
    local prefix = items[1]
    for index = 2, #items do
        local item = items[index]
        local limit = math.min(#prefix, #item)
        local pos = 1
        while pos <= limit and prefix:sub(pos, pos) == item:sub(pos, pos) do
            pos = pos + 1
        end
        prefix = prefix:sub(1, pos - 1)
        if prefix == "" then
            break
        end
    end
    return prefix
end

local Editor = {}
Editor.__index = Editor

function Editor:new(path)
    return setmetatable({
        path = path or "scripts/novo-script.lua",
        lines = read_file(path or "scripts/novo-script.lua"),
        row = 1,
        col = 1,
        row_offset = 0,
        col_offset = 0,
        dirty = false,
        status = "Tab completa Lua/snes | Ctrl-S salva | Ctrl-Q sai",
        status_tick = 0,
        quit = false,
    }, self)
end

function Editor:line()
    return self.lines[self.row] or ""
end

function Editor:set_status(text)
    self.status = text
    self.status_tick = os.clock()
end

function Editor:save()
    local ok, message = write_file(self.path, self.lines)
    if ok then
        self.dirty = false
        self:set_status("salvo: " .. self.path)
    else
        self:set_status("erro ao salvar: " .. tostring(message))
    end
end

function Editor:move_cursor(key)
    if key == "left" then
        if self.col > 1 then
            self.col = self.col - 1
        elseif self.row > 1 then
            self.row = self.row - 1
            self.col = #self:line() + 1
        end
    elseif key == "right" then
        if self.col <= #self:line() then
            self.col = self.col + 1
        elseif self.row < #self.lines then
            self.row = self.row + 1
            self.col = 1
        end
    elseif key == "up" then
        if self.row > 1 then
            self.row = self.row - 1
        end
    elseif key == "down" then
        if self.row < #self.lines then
            self.row = self.row + 1
        end
    elseif key == "home" then
        self.col = 1
    elseif key == "end" then
        self.col = #self:line() + 1
    end
    local line_len = #self:line()
    if self.col > line_len + 1 then
        self.col = line_len + 1
    end
end

function Editor:insert_char(char)
    local line = self:line()
    self.lines[self.row] = line:sub(1, self.col - 1) .. char .. line:sub(self.col)
    self.col = self.col + #char
    self.dirty = true
end

function Editor:newline()
    local line = self:line()
    local before = line:sub(1, self.col - 1)
    local after = line:sub(self.col)
    self.lines[self.row] = before
    table.insert(self.lines, self.row + 1, after)
    self.row = self.row + 1
    self.col = 1
    self.dirty = true
end

function Editor:backspace()
    if self.col > 1 then
        local line = self:line()
        self.lines[self.row] = line:sub(1, self.col - 2) .. line:sub(self.col)
        self.col = self.col - 1
    elseif self.row > 1 then
        local current = self:line()
        self.row = self.row - 1
        self.col = #self:line() + 1
        self.lines[self.row] = self.lines[self.row] .. current
        table.remove(self.lines, self.row + 1)
    end
    self.dirty = true
end

function Editor:delete_char()
    local line = self:line()
    if self.col <= #line then
        self.lines[self.row] = line:sub(1, self.col - 1) .. line:sub(self.col + 1)
    elseif self.row < #self.lines then
        self.lines[self.row] = line .. self.lines[self.row + 1]
        table.remove(self.lines, self.row + 1)
    end
    self.dirty = true
end

function Editor:current_prefix()
    local line = self:line()
    local left = line:sub(1, self.col - 1)
    local start_pos = #left + 1
    while start_pos > 1 do
        local char = left:sub(start_pos - 1, start_pos - 1)
        if not char:match("[%w_%.]") then
            break
        end
        start_pos = start_pos - 1
    end
    return left:sub(start_pos), start_pos
end

function Editor:complete()
    local prefix, start_pos = self:current_prefix()
    local matches = suggestions_for(prefix, self.lines)
    if #matches == 0 then
        self:set_status("sem sugestao para: " .. prefix)
        return
    end

    local replacement = common_prefix(matches)
    if #matches == 1 or #replacement > #prefix then
        local line = self:line()
        self.lines[self.row] = line:sub(1, start_pos - 1) ..
            replacement .. line:sub(self.col)
        self.col = start_pos + #replacement
        self.dirty = true
    end

    local preview = {}
    for index = 1, math.min(#matches, 8) do
        preview[#preview + 1] = matches[index]
    end
    self:set_status(table.concat(preview, "  "))
end

function Editor:scroll(screen_rows, screen_cols)
    local text_rows = screen_rows - 2
    if self.row <= self.row_offset then
        self.row_offset = self.row - 1
    elseif self.row > self.row_offset + text_rows then
        self.row_offset = self.row - text_rows
    end

    local gutter = 6
    local text_cols = screen_cols - gutter
    if self.col <= self.col_offset then
        self.col_offset = self.col - 1
    elseif self.col > self.col_offset + text_cols then
        self.col_offset = self.col - text_cols
    end
    if self.row_offset < 0 then self.row_offset = 0 end
    if self.col_offset < 0 then self.col_offset = 0 end
end

function Editor:render()
    local rows, cols = terminal_size()
    self:scroll(rows, cols)
    local text_rows = rows - 2
    io.write("\027[?25l\027[H")

    for screen_row = 1, text_rows do
        local file_row = self.row_offset + screen_row
        io.write("\027[K")
        if file_row <= #self.lines then
            local number = string.format("%4d ", file_row)
            local text = self.lines[file_row]:sub(self.col_offset + 1)
            io.write("\027[90m", number, "\027[0m", trim_to_width(text, cols - #number))
        else
            io.write("\027[90m~\027[0m")
        end
        io.write("\r\n")
    end

    local modified = self.dirty and " +" or ""
    local title = string.format(" %s%s  linha %d/%d col %d ",
        self.path, modified, self.row, #self.lines, self.col)
    io.write("\027[7m", trim_to_width(title, cols))
    if #title < cols then
        io.write(string.rep(" ", cols - #title))
    end
    io.write("\027[0m\r\n")

    local status = self.status
    if self.status_tick > 0 and os.clock() - self.status_tick > 5 then
        status = "Tab completa Lua/snes | Ctrl-S salva | Ctrl-Q sai"
    end
    io.write("\027[K", trim_to_width(" " .. status, cols))

    local cursor_row = self.row - self.row_offset
    local cursor_col = 6 + self.col - self.col_offset
    if cursor_row < 1 then cursor_row = 1 end
    if cursor_col < 1 then cursor_col = 1 end
    io.write(string.format("\027[%d;%dH\027[?25h", cursor_row, cursor_col))
    io.flush()
end

local function read_key()
    local char = io.read(1)
    if not char then
        return "quit"
    end
    local byte = char:byte()
    if byte == 17 then return "ctrl-q" end
    if byte == 19 then return "ctrl-s" end
    if byte == 9 then return "tab" end
    if byte == 13 or byte == 10 then return "enter" end
    if byte == 127 or byte == 8 then return "backspace" end
    if byte == 27 then
        local next1 = io.read(1)
        if next1 ~= "[" then
            return "escape"
        end
        local next2 = io.read(1)
        if next2 == "A" then return "up" end
        if next2 == "B" then return "down" end
        if next2 == "C" then return "right" end
        if next2 == "D" then return "left" end
        if next2 == "H" then return "home" end
        if next2 == "F" then return "end" end
        if next2 == "3" and io.read(1) == "~" then return "delete" end
        if next2 == "5" and io.read(1) == "~" then return "page-up" end
        if next2 == "6" and io.read(1) == "~" then return "page-down" end
        if next2 == "1" or next2 == "4" or next2 == "7" or next2 == "8" then
            io.read(1)
            if next2 == "1" or next2 == "7" then return "home" end
            return "end"
        end
        return "escape"
    end
    if byte >= 32 and byte <= 126 then
        return "char", char
    end
    return "unknown"
end

function Editor:process_key()
    local key, char = read_key()
    if key == "ctrl-q" then
        if self.dirty == "confirm-quit" then
            self.quit = true
        elseif self.dirty then
            self:set_status("arquivo modificado; Ctrl-S salva, Ctrl-Q de novo sai sem salvar")
            self.dirty = "confirm-quit"
        else
            self.quit = true
        end
    elseif key == "ctrl-s" then
        self:save()
    elseif key == "tab" then
        self:complete()
    elseif key == "enter" then
        self:newline()
    elseif key == "backspace" then
        self:backspace()
    elseif key == "delete" then
        self:delete_char()
    elseif key == "page-up" then
        local rows = terminal_size()
        self.row = math.max(1, self.row - rows + 2)
        self:move_cursor("up")
    elseif key == "page-down" then
        local rows = terminal_size()
        self.row = math.min(#self.lines, self.row + rows - 2)
        self:move_cursor("down")
    elseif key == "up" or key == "down" or key == "left" or key == "right" or
        key == "home" or key == "end" then
        self:move_cursor(key)
    elseif key == "char" then
        if self.dirty == "confirm-quit" then
            self.dirty = true
        end
        self:insert_char(char)
    elseif key == "quit" then
        self.quit = true
    end
end

local function run(path)
    local editor = Editor:new(path)
    os.execute("stty raw -echo")
    io.write("\027[?1049h\027[2J")
    local ok, message = xpcall(function()
        while not editor.quit do
            editor:render()
            editor:process_key()
        end
    end, debug.traceback)
    io.write("\027[?1049l\027[?25h")
    os.execute("stty sane")
    if not ok then
        io.stderr:write(message, "\n")
        os.exit(1)
    end
end

run(arg[1])
