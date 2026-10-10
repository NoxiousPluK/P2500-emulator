# P2500 emulator.
#
# The tree is split three ways:
#
#   src/core/ -> libp2500.a  the machine model. C11, depends on nothing but
#                libc, holds no global state, and must stay that way - it is
#                what lets `make test` run headless and what keeps other
#                embeddings (a GUI, Emscripten) possible.
#   src/cli/  -> p2500-emu   the headless harness. What `make test` drives.
#   src/gui/  -> p2500-gui   SDL3 + Dear ImGui, the only C++ in the tree.
#                Nothing in core/ or cli/ may depend on it.
#
# Core sources are picked up by wildcard, so adding a device to src/core/
# needs no edit here.

# MINGW=1 cross-compiles for Windows (x86_64-w64-mingw32-gcc/g++, needs the
# mingw-w64 toolchain and a mingw-w64 SDL3 build for the GUI). Everything
# below reads through CC/CXX/AR/PKG_CONFIG/EXE_SUFFIX/*_LDFLAGS rather than
# assuming the native toolchain, so this is the only place the two differ.
ifdef MINGW
CC = x86_64-w64-mingw32-gcc
CXX = x86_64-w64-mingw32-g++
AR = x86_64-w64-mingw32-ar
PKG_CONFIG = x86_64-w64-mingw32-pkg-config
WINDRES = x86_64-w64-mingw32-windres
EXE_SUFFIX = .exe
# The PE icon resource. Windows reads the .exe's own icon off this; the
# SDL window icon is set at runtime from the same artwork (src/gui/icon.cpp).
# ELF has no equivalent, so this is Windows-only and stays empty elsewhere.
RES_OBJ = src/win/p2500.res.o
# p2500-emu has no other dependencies, so link it fully static: no
# MinGW-runtime DLLs to carry, only the Universal CRT forwarder DLLs,
# which Windows 10+ ships itself.
BIN_LDFLAGS = -static
# p2500-gui keeps SDL3 dynamic (SDL3.dll ships alongside it - see
# publish.sh): linking SDL3 itself statically pulls in a long tail of
# Windows system libraries (ole32, setupapi, cfgmgr32, ...) that the
# mingw-w64-sdl3 package's pkg-config file does not fully account for.
# Only the MinGW runtime is forced static, via -Wl,-Bstatic bracketing
# just libwinpthread so -lSDL3 right after it still resolves against the
# shared import library.
GUI_LDFLAGS = -static-libgcc -static-libstdc++ \
              -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,-Bdynamic,--no-whole-archive
else
CC = cc
CXX = c++
AR = ar
PKG_CONFIG = pkg-config
EXE_SUFFIX =
RES_OBJ =
BIN_LDFLAGS =
GUI_LDFLAGS =
endif

# -MMD -MP emits a .d per object listing the headers it used, so editing a
# header rebuilds everything that includes it.
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc -MMD -MP
# The GUI is C++ because Dear ImGui is. It is confined to src/gui/ and the
# vendored ImGui is compiled without -Wall/-Wextra.
IMGUI_DIR = src/gui/vendor/imgui
CXXFLAGS = -std=c++17 -O2 -Isrc -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -MMD -MP
GUI_CXXFLAGS = $(CXXFLAGS) -Wall -Wextra

LIB = libp2500.a
BIN = p2500-emu$(EXE_SUFFIX)
GUI = p2500-gui$(EXE_SUFFIX)

# The GUI is built only when asked for, and only if SDL3 is present, so a
# headless checkout still builds and `make test` still runs.
# Test for the package itself: --cflags is legitimately EMPTY when SDL3's
# headers sit on the default include path, so it cannot be the probe.
HAVE_SDL3 := $(shell $(PKG_CONFIG) --exists sdl3 && echo yes)
SDL_CFLAGS = $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL_LIBS = $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)

CORE_SRC = $(wildcard src/core/*.c) src/core/vendor/superzazu_z80/z80.c
CLI_SRC = $(wildcard src/cli/*.c)
GUI_SRC = $(wildcard src/gui/*.cpp)
IMGUI_SRC = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
            $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
            $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer3.cpp
IMGUI_OBJ = $(IMGUI_SRC:.cpp=.o)
CORE_OBJ = $(CORE_SRC:.c=.o)
CLI_OBJ = $(CLI_SRC:.c=.o)
GUI_OBJ = $(GUI_SRC:.cpp=.o)
DEPS = $(CORE_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(IMGUI_OBJ:.o=.d)

.PHONY: all clean run test gui demos

# The GUI is part of the default build whenever SDL3 is present,because 
# tests/run_tests.sh tests ./p2500-gui if it exists. A machine with no
# SDL3 still builds and still runs the suite; it just skips the GUI checks.
GUI_IF_AVAILABLE = $(if $(HAVE_SDL3),$(GUI))

all: $(BIN) $(GUI_IF_AVAILABLE)

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BIN): $(CLI_OBJ) $(LIB) $(RES_OBJ)
	$(CC) $(CFLAGS) $(BIN_LDFLAGS) -o $@ $(CLI_OBJ) $(LIB) $(RES_OBJ)

gui: $(GUI)

$(GUI): $(GUI_OBJ) $(IMGUI_OBJ) $(LIB) $(RES_OBJ)
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	$(CXX) $(CXXFLAGS) $(GUI_LDFLAGS) -o $@ $(GUI_OBJ) $(IMGUI_OBJ) $(LIB) $(SDL_LIBS) $(RES_OBJ)

# -I. so the .rc's path to assets/icon/ resolves from the project root.
src/win/%.res.o: src/win/%.rc assets/icon/p2500icon.ico
	$(WINDRES) -I. -O coff -i $< -o $@

src/gui/%.o: src/gui/%.cpp
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	$(CXX) $(GUI_CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The graphics demos. Assembled by tools/z80asm.py, which verifies its own
# output against the disassembler in libp2500, and packaged by
# tools/cpm_build.py onto a bootable image. Not part of `all` since it's a
# separate build product, not something every build needs.
DEMO_ASM = demos/logo.asm demos/stars.asm demos/spiro.asm demos/bench.asm
DEMO_COM = $(DEMO_ASM:.asm=.COM)
DEMO_DISK = demos/P2500DEMO.raw
BOOT_DONOR ?= disks/P25K_B.raw

demos: $(DEMO_DISK)

demos/%.COM: demos/%.asm demos/p2500.inc demos/logo_sprite.asm tools/z80asm.py $(BIN)
	python3 tools/z80asm.py $< -o $@ --verify

$(DEMO_DISK): $(DEMO_COM) tools/cpm_build.py
	@test -f "$(BOOT_DONOR)" || { echo "need a bootable P2500 image: BOOT_DONOR=path make demos"; exit 1; }
	python3 tools/cpm_build.py $@ $(DEMO_COM) --boot-from "$(BOOT_DONOR)"

clean:
	rm -f $(BIN) $(GUI) $(LIB) $(CORE_OBJ) $(CLI_OBJ) $(GUI_OBJ) $(IMGUI_OBJ) $(DEPS)
	rm -f src/win/*.res.o
	rm -f $(DEMO_COM) $(DEMO_DISK)

-include $(DEPS)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

test: $(BIN) $(GUI_IF_AVAILABLE)
	@tests/run_tests.sh
