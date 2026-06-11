CXX ?= clang++
PKG_CONFIG ?= pkg-config

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2)
LUA_CFLAGS := $(shell $(PKG_CONFIG) --cflags luajit)
LUA_LIBS := $(shell $(PKG_CONFIG) --libs luajit)
QT_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Widgets)
QT_LIBS := $(shell $(PKG_CONFIG) --libs Qt6Widgets)
QSCI_LIBS := -L/opt/homebrew/lib -lqscintilla2_qt6 -Wl,-rpath,/opt/homebrew/lib
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic $(SDL_CFLAGS) $(LUA_CFLAGS) -Iinclude
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := $(SDL_LIBS) $(LUA_LIBS) lib/snes9x_libretro.dylib -Wl,-rpath,@executable_path/../lib
CXX_SOURCES := $(filter-out src/lua_editor_qt.cpp,$(shell find src -name '*.cpp' -print))
SOURCES := $(CXX_SOURCES)

.PHONY: all run test test-sanitize clean

all: build/snes build/snes-lua-editor

build/snes: $(SOURCES) include/libretro.h lib/snes9x_libretro.dylib
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

build/snes-lua-editor: src/lua_editor_qt.cpp
	@mkdir -p build
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Wpedantic $(QT_CFLAGS) $< -o $@ $(QT_LIBS) $(QSCI_LIBS)

build/snes-asan: $(SOURCES) include/libretro.h lib/snes9x_libretro.dylib
	@mkdir -p build
	$(CXX) -std=c++20 $(SANITIZE_FLAGS) -Wall -Wextra -Wpedantic $(SDL_CFLAGS) $(LUA_CFLAGS) -Iinclude $(SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_FLAGS)

run: build/snes build/snes-lua-editor
	./build/snes mario.sfc

test: build/snes
	./build/snes mario.sfc --headless --frames 180

test-sanitize: build/snes-asan
	./build/snes-asan mario.sfc --headless --frames 180

clean:
	rm -rf build
