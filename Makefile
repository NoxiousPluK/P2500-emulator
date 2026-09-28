# P2500 emulator.
#
# The tree is split three ways (TODO.md T33):
#
#   src/core/ -> libp2500.a  the machine model. C11, depends on nothing but
#                libc, holds no global state, and must stay that way - it is
#                what lets `make test` run headless and what keeps other
#                embeddings (a GUI, Emscripten) possible.
#   src/cli/  -> p2500-emu   the headless harness. What `make test` drives.
#   src/gui/  -> p2500-gui   SDL3 + Dear ImGui (TODO.md T36+). Does not exist
#                yet; when it does it is the only C++ in the tree, and
#                nothing in core/ or cli/ may depend on it.
#
# Core sources are picked up by wildcard, so adding a device to src/core/
# needs no edit here.

CC = cc
CXX = c++
AR = ar
# -MMD -MP emits a .d per object listing the headers it used, so editing a
# header rebuilds everything that includes it. Without this, changing a
# struct in a header silently leaves other objects compiled against the old
# layout - which is a memory-corruption bug, not a stale-build annoyance.
CFLAGS = -std=c11 -Wall -Wextra -O2 -Isrc -MMD -MP
# The GUI is C++ because Dear ImGui is. It is confined to src/gui/ and the
# vendored ImGui is compiled without -Wall/-Wextra: it is upstream code, not
# ours, and its warnings would drown out warnings we should act on.
IMGUI_DIR = src/gui/vendor/imgui
CXXFLAGS = -std=c++17 -O2 -Isrc -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -MMD -MP
GUI_CXXFLAGS = $(CXXFLAGS) -Wall -Wextra

LIB = libp2500.a
BIN = p2500-emu
GUI = p2500-gui

# The GUI is built only when asked for, and only if SDL3 is present, so a
# headless checkout still builds and `make test` still runs.
# Test for the package itself: --cflags is legitimately EMPTY when SDL3's
# headers sit on the default include path, so it cannot be the probe.
HAVE_SDL3 := $(shell pkg-config --exists sdl3 && echo yes)
SDL_CFLAGS = $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL_LIBS = $(shell pkg-config --libs sdl3 2>/dev/null)

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

# The GUI is part of the default build whenever SDL3 is present. It must be,
# because tools/run_tests.sh tests ./p2500-gui if it exists: leaving it out of
# `all` meant a stale binary from an earlier build was silently tested - and
# passed - while the library under it had moved on. A headless machine with no
# SDL3 still builds and still runs the suite; it just skips the GUI checks.
GUI_IF_AVAILABLE = $(if $(HAVE_SDL3),$(GUI))

all: $(BIN) $(GUI_IF_AVAILABLE)

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BIN): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(LIB)

gui: $(GUI)

$(GUI): $(GUI_OBJ) $(IMGUI_OBJ) $(LIB)
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	$(CXX) $(CXXFLAGS) -o $@ $(GUI_OBJ) $(IMGUI_OBJ) $(LIB) $(SDL_LIBS)

src/gui/%.o: src/gui/%.cpp
	@test "$(HAVE_SDL3)" = yes || { echo "sdl3 not found by pkg-config - install it to build the GUI"; exit 1; }
	$(CXX) $(GUI_CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(IMGUI_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The graphics demos (TODO.md T47). Assembled by tools/z80asm.py, which
# verifies its own output against the disassembler in libp2500, and packaged
# by tools/cpm_build.py onto a bootable image. Not part of `all`: it needs a
# donor disk for the CP/M system files, which lives outside the repo.
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
	rm -f $(DEMO_COM) $(DEMO_DISK)

-include $(DEPS)

run: $(BIN)
	./$(BIN) --rom roms/ipl.bin

test: $(BIN) $(GUI_IF_AVAILABLE)
	@tools/run_tests.sh
