CXX ?= clang++
PKG_CONFIG ?= pkg-config
LUAJIT ?= luajit
SCRIPT ?= scripts/novo-script.lua

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2)
LUA_CFLAGS := $(shell $(PKG_CONFIG) --cflags luajit)
LUA_LIBS := $(shell $(PKG_CONFIG) --libs luajit)
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic $(SDL_CFLAGS) $(LUA_CFLAGS) -Iinclude
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := $(SDL_LIBS) $(LUA_LIBS) lib/snes9x_libretro.dylib -Wl,-rpath,@executable_path/../lib
SOURCES := $(wildcard src/*.cpp)

.PHONY: all run script-editor test test-sanitize clean

all: build/snes

build/snes: $(SOURCES) include/libretro.h lib/snes9x_libretro.dylib
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

build/snes-asan: $(SOURCES) include/libretro.h lib/snes9x_libretro.dylib
	@mkdir -p build
	$(CXX) -std=c++20 $(SANITIZE_FLAGS) -Wall -Wextra -Wpedantic $(SDL_CFLAGS) $(LUA_CFLAGS) -Iinclude $(SOURCES) -o $@ $(LDFLAGS) $(SANITIZE_FLAGS)

run: build/snes
	./build/snes mario.sfc

script-editor:
	$(LUAJIT) scripts/snes-editor.lua $(SCRIPT)

test: build/snes
	./build/snes mario.sfc --headless --frames 180

test-sanitize: build/snes-asan
	./build/snes-asan mario.sfc --headless --frames 180

clean:
	rm -rf build
